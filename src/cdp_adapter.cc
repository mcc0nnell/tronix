// First native Celix/CDP operation adapter. No browser script, URL or raw CDP method input.
// MessagePack only between this bundle and a trusted, owned-target fixture bridge.
#include "tronix_service.h"
#include <celix_bundle_activator.h>
#include <celix_bundle_context.h>
#include <celix_constants.h>
#include <msgpack.hpp>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
struct CdpAdapter {
    long service_id{-1};
    tronix_operation_service_t service{};
    char socket_path[sizeof(sockaddr_un::sun_path)]{};
};
bool send_all(int fd,const void* ptr,size_t n) {
    const char* b=static_cast<const char*>(ptr);
    for(size_t pos=0;pos<n;) {
        ssize_t rc=send(fd,b+pos,n-pos,MSG_NOSIGNAL);
        if(rc<0&&errno==EINTR)continue;
        if(rc<=0)return false;
        pos+=static_cast<size_t>(rc);
    }
    return true;
}
bool recv_all(int fd,void* ptr,size_t n) {
    char* b=static_cast<char*>(ptr);
    for(size_t pos=0;pos<n;) {
        ssize_t rc=recv(fd,b+pos,n-pos,0);
        if(rc<0&&errno==EINTR)continue;
        if(rc<=0)return false;
        pos+=static_cast<size_t>(rc);
    }
    return true;
}
int lane_for(void*,const char* operation) {
    return operation&&std::strcmp(operation,"browser.constant")==0?1:-1;
}
uint32_t budget_for(void*,const char* op){return lane_for(nullptr,op)>=0?5000:0;}
const char* operations(void*){return "browser.constant";}
int execute(void* handle,const char* op,int64_t* value){
    auto* self=static_cast<CdpAdapter*>(handle);
    if(!value||lane_for(handle,op)<0)return 0;
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    if(fd<0)return 0;
    timeval timeout{3,0};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    sockaddr_un address{};address.sun_family=AF_UNIX;
    std::strcpy(address.sun_path,self->socket_path);
    bool ok=false;
    if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0){
        msgpack::sbuffer body;msgpack::packer<msgpack::sbuffer> packer(body);
        packer.pack_map(2);
        packer.pack(std::string("v"));packer.pack(1);
        packer.pack(std::string("op"));packer.pack(std::string("browser.constant"));
        uint32_t wire=htonl(static_cast<uint32_t>(body.size()));
        if(send_all(fd,&wire,4)&&send_all(fd,body.data(),body.size())){
            uint32_t n=0;
            if(recv_all(fd,&n,4)){
                n=ntohl(n);
                if(n>0&&n<=4096){
                    std::string response(n,'\0');
                    if(recv_all(fd,response.data(),n)){
                        try {
                            msgpack::object_handle h=msgpack::unpack(response.data(),response.size());
                            const msgpack::object& m=h.get();
                            if(m.type==msgpack::type::MAP&&m.via.map.size==2){
                                bool has_ok=false,has_value=false,is_ok=false;int64_t out=0;
                                for(uint32_t i=0;i<m.via.map.size;i++){
                                    const auto& p=m.via.map.ptr[i];
                                    if(p.key.type!=msgpack::type::STR)break;
                                    const std::string key=p.key.as<std::string>();
                                    if(key=="ok"&&p.val.type==msgpack::type::BOOLEAN&&!has_ok){
                                        is_ok=p.val.as<bool>();has_ok=true;
                                    }else if(key=="value"&&p.val.type==msgpack::type::POSITIVE_INTEGER&&!has_value){
                                        out=p.val.as<int64_t>();has_value=true;
                                    }else break;
                                }
                                if(has_ok&&is_ok&&has_value&&out==42){*value=out;ok=true;}
                            }
                        }catch(const std::exception&){}
                    }
                }
            }
        }
    }
    close(fd);return ok?1:0;
}
celix_status_t start(CdpAdapter* self,celix_bundle_context_t* ctx){
    self->service_id=-1;
    const char* sock=getenv("TRONIX_CDP_BRIDGE_SOCKET");
    if(!sock||sock[0]!='/'||std::strlen(sock)>=sizeof(self->socket_path))return CELIX_SERVICE_EXCEPTION;
    struct stat st{};
    if(lstat(sock,&st)!=0||!S_ISSOCK(st.st_mode)||
       st.st_uid!=geteuid()||(st.st_mode & 077)!=0)return CELIX_SERVICE_EXCEPTION;
    std::strcpy(self->socket_path,sock);
    self->service.handle=self;self->service.execute=execute;
    self->service.lane_for=lane_for;self->service.budget_for=budget_for;
    self->service.operations=operations;
    celix_service_registration_options_t opts{};
    opts.svc=&self->service;
    opts.serviceName=TRONIX_OPERATION_SERVICE;
    opts.serviceVersion=TRONIX_ABI_VERSION;
    self->service_id=celix_bundleContext_registerServiceWithOptions(ctx,&opts);
    return self->service_id>=0?CELIX_SUCCESS:CELIX_SERVICE_EXCEPTION;
}
celix_status_t stop(CdpAdapter* self,celix_bundle_context_t* ctx){
    if(self->service_id>=0)celix_bundleContext_unregisterService(ctx,self->service_id);
    return CELIX_SUCCESS;
}
}
CELIX_GEN_BUNDLE_ACTIVATOR(CdpAdapter,start,stop)

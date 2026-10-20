// Lab-only, fixed-profile bridge: daemon/worker never invokes Docker.
#include "tronix_service.h"
#include <celix_bundle_activator.h>
#include <celix_bundle_context.h>
#include <msgpack.hpp>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
struct Bridge {
    long service_id{-1};
    tronix_operation_service_t service{};
    char socket_path[sizeof(sockaddr_un::sun_path)]{};
};
bool fixed(const char* op) {
    return op && (std::strcmp(op,"fuzz.fixture.safe")==0 ||
                  std::strcmp(op,"fuzz.fixture.buggy")==0);
}
int lane_for(void*,const char* op){return fixed(op)?0:-1;}
uint32_t budget_for(void*,const char* op){return fixed(op)?28000:0;}
const char* operations(void*){return "fuzz.fixture.safe,fuzz.fixture.buggy";}
bool transmit(int fd,const void* data,size_t len) {
    auto* p=static_cast<const char*>(data);
    for(size_t n=0;n<len;) {
        ssize_t k=send(fd,p+n,len-n,MSG_NOSIGNAL);
        if(k<0 && errno==EINTR)continue;
        if(k<=0)return false;
        n+=static_cast<size_t>(k);
    }
    return true;
}
bool receive(int fd,void* data,size_t len) {
    auto* p=static_cast<char*>(data);
    for(size_t n=0;n<len;) {
        ssize_t k=recv(fd,p+n,len-n,0);
        if(k<0 && errno==EINTR)continue;
        if(k<=0)return false;
        n+=static_cast<size_t>(k);
    }
    return true;
}
int execute(void* handle,const char* op,int64_t* value) {
    if(!fixed(op) || !value || !handle)return 0;
    auto* self=static_cast<Bridge*>(handle);
    const int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    if(fd<0)return 0;
    timeval recv_timeout{22,0}, send_timeout{3,0};
    (void)setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&recv_timeout,sizeof(recv_timeout));
    (void)setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&send_timeout,sizeof(send_timeout));
    sockaddr_un address{};address.sun_family=AF_UNIX;
    std::strcpy(address.sun_path,self->socket_path);
    bool valid=false;
    if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0) {
        msgpack::sbuffer body;msgpack::packer<msgpack::sbuffer> packer(body);
        packer.pack_map(2);
        packer.pack(std::string("v"));packer.pack(1);
        packer.pack(std::string("op"));packer.pack(std::string(op));
        const uint32_t wire=htonl(static_cast<uint32_t>(body.size()));
        if(transmit(fd,&wire,4)&&transmit(fd,body.data(),body.size())) {
            uint32_t received=0;
            if(receive(fd,&received,4)) {
                const uint32_t len=ntohl(received);
                if(len>0 && len<=128) {
                    std::string reply(len,'\0');
                    if(receive(fd,reply.data(),len)) {
                        try {
                            auto message=msgpack::unpack(reply.data(),reply.size());
                            const auto& map=message.get();
                            if(map.type==msgpack::type::MAP&&map.via.map.size==2) {
                                bool got_ok=false,got_value=false,ok=false;
                                int64_t got=0;
                                for(uint32_t i=0;i<map.via.map.size;i++) {
                                    const auto& pair=map.via.map.ptr[i];
                                    if(pair.key.type!=msgpack::type::STR)break;
                                    auto key=pair.key.as<std::string>();
                                    if(key=="ok"&&!got_ok&&pair.val.type==msgpack::type::BOOLEAN) {
                                        got_ok=true;ok=pair.val.as<bool>();
                                    } else if(key=="value"&&!got_value&&
                                      pair.val.type==msgpack::type::POSITIVE_INTEGER) {
                                        got_value=true;got=pair.val.as<int64_t>();
                                    } else break;
                                }
                                const int64_t expected=std::strcmp(op,"fuzz.fixture.safe")==0?1:2;
                                valid=got_ok&&ok&&got_value&&got==expected;
                                if(valid)*value=got;
                            }
                        } catch(const std::exception&) {}
                    }
                }
            }
        }
    }
    close(fd);
    return valid?1:0;
}
celix_status_t start(Bridge* bridge,celix_bundle_context_t* ctx) {
    bridge->service_id=-1;
    const char* path=getenv("TRONIX_IHB_BROKER_SOCKET");
    if(!path||path[0]!='/'||std::strlen(path)>=sizeof(bridge->socket_path))
        return CELIX_SERVICE_EXCEPTION;
    struct stat st{};
    if(lstat(path,&st)!=0||!S_ISSOCK(st.st_mode)||st.st_uid!=geteuid()||
       (st.st_mode&077)!=0)return CELIX_SERVICE_EXCEPTION;
    std::strcpy(bridge->socket_path,path);
    bridge->service.handle=bridge;bridge->service.execute=execute;
    bridge->service.lane_for=lane_for;bridge->service.budget_for=budget_for;
    bridge->service.operations=operations;
    celix_service_registration_options_t options{};
    options.svc=&bridge->service;options.serviceName=TRONIX_OPERATION_SERVICE;
    options.serviceVersion=TRONIX_ABI_VERSION;
    bridge->service_id=celix_bundleContext_registerServiceWithOptions(ctx,&options);
    return bridge->service_id>=0?CELIX_SUCCESS:CELIX_SERVICE_EXCEPTION;
}
celix_status_t stop(Bridge* bridge,celix_bundle_context_t* ctx) {
    if(bridge->service_id>=0)celix_bundleContext_unregisterService(ctx,bridge->service_id);
    return CELIX_SUCCESS;
}
}
CELIX_GEN_BUNDLE_ACTIVATOR(Bridge,start,stop)

// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Restricted Rizzer offline harness adapter. No arbitrary CLI commands.
// TRONIX daemon enforces exact Celix archive grants before admission.
// The supervised worker has a stripped environment by design. The adapter
// derives an owner-private, UID-scoped local socket/token path and never
// forwards arbitrary client paths, environment, or commands. The Rizzer
// harness runs out of process, never in the Celix worker.
#include "tronix_service.h"
#include <celix_bundle_activator.h>
#include <celix_bundle_context.h>
#include <msgpack.hpp>
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
struct HarnessAdapter {
    long service_id{-1};
    tronix_operation_service_t service{};
    char path[sizeof(sockaddr_un::sun_path)]{};
    char token[65]{};
};
int lane_for(void*, const char* op) {
    return op && std::strcmp(op,"rizzer.harness.reference")==0 ? 0 : -1;
}
uint32_t budget_for(void*,const char* op) { return lane_for(nullptr,op)>=0 ? 12000 : 0; }
const char* operations(void*) { return "rizzer.harness.reference"; }
bool write_exact(int fd,const void* ptr,size_t n) {
    auto* b=static_cast<const char*>(ptr);
    for(size_t off=0;off<n;) {
        ssize_t k=send(fd,b+off,n-off,MSG_NOSIGNAL);
        if(k<0&&errno==EINTR)continue;
        if(k<=0)return false;
        off+=static_cast<size_t>(k);
    }
    return true;
}
bool read_exact(int fd,void* ptr,size_t n) {
    auto* b=static_cast<char*>(ptr);
    for(size_t off=0;off<n;) {
        ssize_t k=recv(fd,b+off,n-off,0);
        if(k<0&&errno==EINTR)continue;
        if(k<=0)return false;
        off+=static_cast<size_t>(k);
    }
    return true;
}
bool trusted_socket(const char* path) {
    struct stat st{},parent{};
    std::string dir(path);
    auto i=dir.rfind('/');
    if(i==std::string::npos)return false;
    dir.resize(i==0?1:i);
    return lstat(path,&st)==0&&S_ISSOCK(st.st_mode)&&
           st.st_uid==geteuid()&&(st.st_mode&077)==0&&
           stat(dir.c_str(),&parent)==0&&S_ISDIR(parent.st_mode)&&
           parent.st_uid==geteuid()&&(parent.st_mode&077)==0;
}
bool decode_response(const std::string& raw,int64_t* value) {
    try {
        auto h=msgpack::unpack(raw.data(),raw.size());
        const auto& obj=h.get();
        if(obj.type!=msgpack::type::MAP || obj.via.map.size!=2)return false;
        bool got_ok=false,got_value=false,ok=false;
        int64_t number=0;
        for(uint32_t i=0;i<obj.via.map.size;++i){
            const auto& p=obj.via.map.ptr[i];
            if(p.key.type!=msgpack::type::STR)return false;
            const auto k=p.key.as<std::string>();
            if(k=="ok"&&!got_ok && p.val.type==msgpack::type::BOOLEAN){
                got_ok=true;ok=p.val.as<bool>();
            }else if(k=="value"&&!got_value&&p.val.type==msgpack::type::POSITIVE_INTEGER){
                got_value=true;number=p.val.as<int64_t>();
            }else return false;
        }
        if(!got_ok||!ok||!got_value||number<0||number>32)return false;
        *value=number;return true;
    }catch(const std::exception&){return false;}
}
int execute(void* ptr,const char* op,int64_t* value){
    auto* self=static_cast<HarnessAdapter*>(ptr);
    if(!value||lane_for(ptr,op)<0||!trusted_socket(self->path))return 0;
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);
    if(fd<0)return 0;
    timeval timeout{9,0};
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    sockaddr_un addr{};addr.sun_family=AF_UNIX;
    std::strcpy(addr.sun_path,self->path);
    bool ok=false;
    if(connect(fd,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0){
        msgpack::sbuffer body;
        msgpack::packer<msgpack::sbuffer> pk(body);
        pk.pack_map(3);pk.pack(std::string("v"));pk.pack(1);
        pk.pack(std::string("op"));pk.pack(std::string(op));
        pk.pack(std::string("token"));pk.pack(std::string(self->token));
        uint32_t len=htonl(static_cast<uint32_t>(body.size()));
        if(write_exact(fd,&len,sizeof(len)) && write_exact(fd,body.data(),body.size())){
            uint32_t n=0;
            if(read_exact(fd,&n,sizeof(n))){
                n=ntohl(n);
                if(n>0&&n<=256){
                    std::string response(n,'\0');
                    if(read_exact(fd,response.data(),n))ok=decode_response(response,value);
                }
            }
        }
    }
    close(fd);return ok?1:0;
}
bool trusted_token(const char* path,char out[65]) {
    if(!path||path[0]!='/'||std::strlen(path)>4096)return false;
    int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return false;
    struct stat st{};
    bool ok=fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&
            st.st_uid==geteuid()&&(st.st_mode&077)==0&&
            st.st_nlink==1&&st.st_size==64;
    char buffer[64]{};
    if(ok){
        size_t off=0;
        while(off<64){
            auto n=read(fd,buffer+off,64-off);
            if(n<0&&errno==EINTR)continue;
            if(n<=0){ok=false;break;}
            off+=static_cast<size_t>(n);
        }
        char extra;
        if(ok&&read(fd,&extra,1)!=0)ok=false;
        for(char c:buffer)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))ok=false;
    }
    close(fd);
    if(ok){std::memcpy(out,buffer,64);out[64]='\0';}
    return ok;
}
celix_status_t start(HarnessAdapter* d,celix_bundle_context_t* ctx) {
    const std::string root="/tmp/tronix-rizzer-harness-"+std::to_string(geteuid());
    const std::string path=root+"/harness.sock";
    const std::string token=root+"/bridge.token";
    if(path.size()>=sizeof(d->path)||!trusted_socket(path.c_str()))
        return CELIX_SERVICE_EXCEPTION;
    std::strcpy(d->path,path.c_str());
    if(!trusted_token(token.c_str(),d->token))
        return CELIX_SERVICE_EXCEPTION;
    d->service.handle=d;d->service.execute=execute;
    d->service.lane_for=lane_for;d->service.budget_for=budget_for;
    d->service.operations=operations;
    celix_service_registration_options_t opts{};
    opts.svc=&d->service;opts.serviceName=TRONIX_OPERATION_SERVICE;
    opts.serviceVersion=TRONIX_ABI_VERSION;
    d->service_id=celix_bundleContext_registerServiceWithOptions(ctx,&opts);
    return d->service_id>=0?CELIX_SUCCESS:CELIX_SERVICE_EXCEPTION;
}
celix_status_t stop(HarnessAdapter* d,celix_bundle_context_t* ctx) {
    if(d->service_id>=0)celix_bundleContext_unregisterService(ctx,d->service_id);
    std::memset(d->token,0,sizeof(d->token));
    return CELIX_SUCCESS;
}
}
CELIX_GEN_BUNDLE_ACTIVATOR(HarnessAdapter,start,stop)

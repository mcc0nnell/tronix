// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Restricted Rizzer offline harness adapter. No arbitrary CLI commands.
// TRONIX daemon enforces exact Celix archive grants before admission.
// The supervised worker has a stripped environment by design. The adapter
// derives an owner-private, UID-scoped local socket/token path and never
// forwards arbitrary client paths, environment, or commands. The Rizzer
// harness runs out of process, never in the Celix worker.
#include "tronix_service.h"
#include "tronix_artifact_service.h"
#include <jansson.h>
#include <openssl/evp.h>
#include <celix_bundle_activator.h>
#include <celix_bundle_context.h>
#include <msgpack.hpp>
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <set>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
struct HarnessAdapter {
    long service_id{-1};
    long digest_service_id{-1};
    tronix_operation_service_t service{};
    tronix_artifact_service_t digest_service{};
    // CELIX_GEN_BUNDLE_ACTIVATOR uses calloc/free; bundle state MUST remain
    // trivially initialized/destructible. Use fixed C-compatible storage only.
    char operations[512];
    char allowed[8][49];
    unsigned allowed_count;
    char last_digest[65]{};
    bool digest_ready=false;
    char path[sizeof(sockaddr_un::sun_path)]{};
    char token[65]{};
};
int lane_for(void* ptr, const char* op) {
    auto* self=static_cast<HarnessAdapter*>(ptr);
    if(!self||!op)return -1;
    for(unsigned i=0;i<self->allowed_count;i++)
        if(std::strcmp(op,self->allowed[i])==0)return 0;
    return -1;
}
uint32_t budget_for(void* ptr,const char* op) { return lane_for(ptr,op)>=0 ? 12000 : 0; }
const char* operations(void* ptr) {
    return static_cast<HarnessAdapter*>(ptr)->operations;
}
int take_digest(void* ptr,char out[65]) {
    auto* self=static_cast<HarnessAdapter*>(ptr);
    if(!self||!out||!self->digest_ready)return 0;
    std::memcpy(out,self->last_digest,65);
    self->digest_ready=false;
    std::memset(self->last_digest,0,65);
    return 1;
}
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
bool hex_digest(const std::string& d) {
    return d.size()==64&&std::all_of(d.begin(),d.end(),[](unsigned char c){
        return (c>='0'&&c<='9')||(c>='a'&&c<='f');
    });
}
bool decode_response(const std::string& raw,int64_t* value,char digest[65]) {
    try {
        auto h=msgpack::unpack(raw.data(),raw.size());
        const auto& obj=h.get();
        if(obj.type!=msgpack::type::MAP || obj.via.map.size!=3)return false;
        bool got_ok=false,got_value=false,got_digest=false,ok=false;
        std::string found_digest;
        int64_t number=0;
        for(uint32_t i=0;i<obj.via.map.size;++i){
            const auto& p=obj.via.map.ptr[i];
            if(p.key.type!=msgpack::type::STR)return false;
            const auto k=p.key.as<std::string>();
            if(k=="ok"&&!got_ok && p.val.type==msgpack::type::BOOLEAN){
                got_ok=true;ok=p.val.as<bool>();
            }else if(k=="value"&&!got_value&&p.val.type==msgpack::type::POSITIVE_INTEGER){
                got_value=true;number=p.val.as<int64_t>();
            }else if(k=="digest"&&!got_digest&&p.val.type==msgpack::type::STR){
                got_digest=true;found_digest=p.val.as<std::string>();
            }else return false;
        }
        if(!got_ok||!ok||!got_value||!got_digest||!hex_digest(found_digest)||
           number<3||number>32)return false;
        *value=number;
        std::memcpy(digest,found_digest.c_str(),65);
        return true;
    }catch(const std::exception&){return false;}
}
int execute(void* ptr,const char* op,int64_t* value){
    auto* self=static_cast<HarnessAdapter*>(ptr);
    if(!value||lane_for(ptr,op)<0||!trusted_socket(self->path))return 0;
    self->digest_ready=false;
    std::memset(self->last_digest,0,65);
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
                    if(read_exact(fd,response.data(),n))
                        ok=decode_response(response,value,self->last_digest);
                }
            }
        }
    }
    close(fd);
    self->digest_ready=ok;
    if(!ok)std::memset(self->last_digest,0,65);
    return ok?1:0;
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
bool read_owned_file(const std::string& path,size_t max,std::string& out) {
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return false;
    struct stat st{};
    bool ok=fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&
            (st.st_mode&077)==0&&st.st_nlink==1&&st.st_size>0&&
            static_cast<size_t>(st.st_size)<=max;
    if(ok){
        out.resize(static_cast<size_t>(st.st_size));
        size_t pos=0;
        while(pos<out.size()){
            ssize_t n=read(fd,out.data()+pos,out.size()-pos);
            if(n<0&&errno==EINTR)continue;
            if(n<=0){ok=false;break;}
            pos+=static_cast<size_t>(n);
        }
    }
    close(fd);
    return ok;
}
bool load_catalog(HarnessAdapter* d,const std::string& root) {
    std::string raw,pin;
    if(!read_owned_file(root+"/catalog.json",8192,raw)||
       !read_owned_file(root+"/catalog.sha256",64,pin)||!hex_digest(pin))return false;
    unsigned char digest[32]{};
    unsigned int n=0;
    if(EVP_Digest(raw.data(),raw.size(),digest,&n,EVP_sha256(),nullptr)!=1||n!=32)
        return false;
    constexpr char hex[]="0123456789abcdef";
    std::string fingerprint;
    for(unsigned char b:digest){fingerprint+=hex[b>>4];fingerprint+=hex[b&15];}
    if(fingerprint!=pin)return false;
    json_error_t e{};
    std::unique_ptr<json_t,decltype(&json_decref)> catalog(
        json_loadb(raw.data(),raw.size(),JSON_REJECT_DUPLICATES,&e),json_decref);
    if(!catalog||!json_is_object(catalog.get())||json_object_size(catalog.get())!=2||
       !json_is_string(json_object_get(catalog.get(),"schema"))||
       std::strcmp(json_string_value(json_object_get(catalog.get(),"schema")),
                   "tronix.rizzer.catalog.v1")!=0)return false;
    json_t* entries=json_object_get(catalog.get(),"entries");
    if(!json_is_array(entries)||json_array_size(entries)<1||json_array_size(entries)>8)return false;
    std::set<std::string> discovered;
    std::string operations;
    for(size_t i=0;i<json_array_size(entries);i++){
        json_t* row=json_array_get(entries,i);
        if(!json_is_object(row)||json_object_size(row)!=4)return false;
        json_t* name=json_object_get(row,"operation");
        json_t* family=json_object_get(row,"family");
        json_t* casebook=json_object_get(row,"casebook");
        json_t* ir_pin=json_object_get(row,"ir_sha256");
        if(!json_is_string(name)||!json_is_string(family)||
           !json_is_string(casebook)||!json_is_string(ir_pin))return false;
        std::string op=json_string_value(name);
        std::string fam=json_string_value(family);
        std::string file=json_string_value(casebook);
        std::string sha=json_string_value(ir_pin);
        if(op.rfind("rizzer.harness.",0)!=0||op.size()>48||op.size()<=15||
           !std::all_of(op.begin()+15,op.end(),[](unsigned char c){
               return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_';
           })||!discovered.insert(op).second||!hex_digest(sha)||
           file.empty()||file[0]!='/'||file.size()>512||
           (fam!="spinel-packed-uint"&&fam!="mcp-authorization"&&
            fam!="sixlowpan-fragments"&&fam!="evm-fullmath"))return false;
        if(!operations.empty())operations+=",";
        operations+=op;
    }
    if(operations.empty()||operations.size()>=sizeof(d->operations)||
       discovered.size()>8)return false;
    std::memcpy(d->operations,operations.c_str(),operations.size()+1);
    unsigned index=0;
    for(const auto& name:discovered){
        if(name.size()>48)return false;
        std::memcpy(d->allowed[index++],name.c_str(),name.size()+1);
    }
    d->allowed_count=index;
    return true;
}
celix_status_t start(HarnessAdapter* d,celix_bundle_context_t* ctx) {
    d->service_id=-1;
    d->digest_service_id=-1;
    const std::string root="/tmp/tronix-rizzer-harness-"+std::to_string(geteuid());
    const std::string path=root+"/harness.sock";
    const std::string token=root+"/bridge.token";
    if(path.size()>=sizeof(d->path)||!trusted_socket(path.c_str()))
        return CELIX_SERVICE_EXCEPTION;
    std::strcpy(d->path,path.c_str());
    if(!trusted_token(token.c_str(),d->token)||!load_catalog(d,root))
        return CELIX_SERVICE_EXCEPTION;
    d->service.handle=d;d->service.execute=execute;
    d->service.lane_for=lane_for;d->service.budget_for=budget_for;
    d->service.operations=operations;
    celix_service_registration_options_t opts{};
    opts.svc=&d->service;opts.serviceName=TRONIX_OPERATION_SERVICE;
    opts.serviceVersion=TRONIX_ABI_VERSION;
    d->service_id=celix_bundleContext_registerServiceWithOptions(ctx,&opts);
    if(d->service_id<0)return CELIX_SERVICE_EXCEPTION;
    d->digest_service.handle=d;
    d->digest_service.take_digest=take_digest;
    celix_service_registration_options_t digest_opts{};
    digest_opts.svc=&d->digest_service;
    digest_opts.serviceName=TRONIX_ARTIFACT_SERVICE;
    digest_opts.serviceVersion=TRONIX_ARTIFACT_ABI_VERSION;
    d->digest_service_id=celix_bundleContext_registerServiceWithOptions(ctx,&digest_opts);
    return d->digest_service_id>=0?CELIX_SUCCESS:CELIX_SERVICE_EXCEPTION;
}
celix_status_t stop(HarnessAdapter* d,celix_bundle_context_t* ctx) {
    if(d->digest_service_id>=0)celix_bundleContext_unregisterService(ctx,d->digest_service_id);
    if(d->service_id>=0)celix_bundleContext_unregisterService(ctx,d->service_id);
    std::memset(d->token,0,sizeof(d->token));
    std::memset(d->last_digest,0,sizeof(d->last_digest));
    return CELIX_SUCCESS;
}
}
CELIX_GEN_BUNDLE_ACTIVATOR(HarnessAdapter,start,stop)

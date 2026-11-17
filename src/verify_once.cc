// SPDX-License-Identifier: Apache-2.0
// One-shot read-only deterministic Celix verifier. Not a security or truth proof.
#include "tronix_answer_verify.h"
#include <celix_bundle_context.h>
#include <celix_constants.h>
#include <celix_framework.h>
#include <celix_framework_factory.h>
#include <celix_properties.h>
#include <openssl/evp.h>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace {
using Digest = std::array<uint8_t, 32>;
struct Bad : std::runtime_error { using std::runtime_error::runtime_error; };
Digest sha(const uint8_t* data, size_t n) {
    Digest d{};
    unsigned int len=0;
    if (!EVP_Digest(data,n,d.data(),&len,EVP_sha256(),nullptr) || len!=32)
        throw Bad("digest unavailable");
    return d;
}
std::string hex(const Digest& d) {
    static constexpr char digits[]="0123456789abcdef";
    std::string s; s.reserve(64);
    for (uint8_t b:d) { s+=digits[b>>4];s+=digits[b&15]; }
    return s;
}
Digest parse_hex(const std::string& s) {
    if (s.size()!=64) throw Bad("digest must be 64 lowercase hex bytes");
    Digest out{};
    auto digit=[](char c)->int {
        if (c>='0' && c<='9') return c-'0';
        if (c>='a' && c<='f') return c-'a'+10;
        throw Bad("noncanonical digest");
    };
    for (size_t i=0;i<32;i++) out[i]=static_cast<uint8_t>(digit(s[i*2])*16+digit(s[i*2+1]));
    return out;
}
bool safe_id(const std::string& s) {
    if (s.empty()||s.size()>64)return false;
    for(unsigned char c:s)if(!(c>='a'&&c<='z')&&!(c>='A'&&c<='Z')&&
                              !(c>='0'&&c<='9')&&c!='-'&&c!='_')return false;
    return true;
}
std::vector<uint8_t> read_plain(const std::string& name, size_t limit) {
    std::filesystem::path p(name);
    if(!p.is_absolute())throw Bad("path must be absolute");
    std::filesystem::path walk("/");
    for (const auto& part:p.relative_path()) {
        if(part=="."||part==".."||part.empty())throw Bad("noncanonical path");
        walk/=part;
        struct stat st{};
        if(lstat(walk.c_str(),&st)!=0 || S_ISLNK(st.st_mode))throw Bad("symlink/missing path");
    }
    int fd=open(name.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)throw Bad("input open failed");
    struct Guard {int fd;~Guard(){close(fd);}} guard{fd};
    struct stat first{},last{};
    if(fstat(fd,&first)!=0 || !S_ISREG(first.st_mode) || first.st_size<0 ||
       static_cast<uint64_t>(first.st_size)>limit)throw Bad("not a bounded regular file");
    std::vector<uint8_t> result;
    result.reserve(static_cast<size_t>(first.st_size)+1);
    std::array<uint8_t,4096> chunk{};
    for(;;) {
        ssize_t n=read(fd,chunk.data(),chunk.size());
        if(n<0 && errno==EINTR)continue;
        if(n<0)throw Bad("input read failed");
        if(n==0)break;
        if(result.size()+static_cast<size_t>(n)>limit)throw Bad("input exceeded bound");
        result.insert(result.end(),chunk.begin(),chunk.begin()+n);
    }
    if(fstat(fd,&last)!=0 || first.st_dev!=last.st_dev || first.st_ino!=last.st_ino ||
       first.st_size!=last.st_size || result.size()!=static_cast<size_t>(first.st_size) ||
       first.st_mtim.tv_sec!=last.st_mtim.tv_sec ||
       first.st_mtim.tv_nsec!=last.st_mtim.tv_nsec)throw Bad("file changed while read");
    return result;
}
std::map<std::string,std::string> arguments(int argc,char** argv) {
    std::map<std::string,std::string> args;
    if((argc-1)%2!=0)throw Bad("expected key/value flags");
    for(int i=1;i<argc;i+=2) {
        std::string k=argv[i],v=argv[i+1];
        if(k!="--task-id" && k!="--input" && k!="--expected-sha256" &&
           k!="--bundle" && k!="--bundle-sha256")throw Bad("unknown flag");
        if(!args.emplace(k,v).second)throw Bad("duplicate flag");
    }
    if(args.size()!=5)throw Bad("missing required flag");
    if(!safe_id(args.at("--task-id")))throw Bad("invalid task id");
    return args;
}
std::string status_name(int s) {
    return s==TRONIX_ANSWER_VERIFIED ? "VERIFIED" :
           s==TRONIX_ANSWER_REJECTED ? "REJECTED" : "UNVERIFIABLE";
}
struct CelixLogRedirect {
    int original=-1;
    CelixLogRedirect() {
        std::fflush(stdout);
        original=dup(STDOUT_FILENO);
        if(original<0 || dup2(STDERR_FILENO,STDOUT_FILENO)<0) {
            if(original>=0)close(original);
            throw Bad("cannot redirect Celix logs");
        }
    }
    ~CelixLogRedirect() {
        std::fflush(stdout);
        dup2(original,STDOUT_FILENO);
        close(original);
    }
};
struct Call { const uint8_t* data; size_t len; Digest expected; Digest actual{};
              int result=TRONIX_ANSWER_UNVERIFIABLE; bool invoked=false; };
void cb(void* h,void* service) {
    auto& c=*static_cast<Call*>(h);
    auto* s=static_cast<tronix_answer_verify_service_t*>(service);
    if(!s || !s->verify)return;
    c.invoked=true;
    c.result=s->verify(s->handle,c.data,c.len,c.expected.data(),c.actual.data());
}
void emit(const std::string& id,int status,const Digest& expected,const Digest& actual,
          size_t size,uint64_t ns) {
    std::cout<<"{\"protocol\":\"tronix.answer.verify/v1\",\"taskId\":\""<<id
             <<"\",\"status\":\""<<status_name(status)
             <<"\",\"scope\":\"precommitted-sha256-byte-equality-only\","
             <<"\"expectedSha256\":\""<<hex(expected)
             <<"\",\"actualSha256\":\""<<hex(actual)
             <<"\",\"bytes\":"<<size<<",\"elapsedNs\":"<<ns
             <<",\"objectiveTruthEstablished\":false,\"published\":false}\n";
}
}
int main(int argc,char** argv) {
    const auto start=std::chrono::steady_clock::now();
    std::string task="invalid";
    Digest expected{},actual{};
    size_t size=0;
    int status=TRONIX_ANSWER_UNVERIFIABLE;
    try {
        auto a=arguments(argc,argv);
        task=a.at("--task-id");
        expected=parse_hex(a.at("--expected-sha256"));
        Digest bundle_pin=parse_hex(a.at("--bundle-sha256"));
        // Verify exact trusted bundle bytes before allowing its installation.
        auto zip=read_plain(a.at("--bundle"),4u*1024u*1024u);
        if(sha(zip.data(),zip.size())!=bundle_pin)throw Bad("bundle identity mismatch");
        auto data=read_plain(a.at("--input"),TRONIX_ANSWER_MAX_BYTES);
        size=data.size();
        if(data.empty())data.push_back(0); // non-null pointer, logical size remains zero
        char tmp[]="/tmp/tronix-answer-XXXXXX";
        char* dir=mkdtemp(tmp);
        if(!dir)throw Bad("cannot allocate private cache");
        std::string scratch=dir;
        struct Cache {std::string path;~Cache(){std::error_code ec;
            std::filesystem::remove_all(path,ec);}} cleanup{scratch};
        // Freeze the verified archive into our private directory, preventing
        // a replacement of the original pathname between hash and load.
        std::string staged=scratch+"/verifier.zip";
        int staged_fd=open(staged.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
        if(staged_fd<0)throw Bad("cannot stage verifier");
        size_t written=0;
        while(written<zip.size()) {
            ssize_t n=write(staged_fd,zip.data()+written,zip.size()-written);
            if(n<0 && errno==EINTR)continue;
            if(n<=0){close(staged_fd);throw Bad("staging write failed");}
            written+=static_cast<size_t>(n);
        }
        if(fsync(staged_fd)!=0){close(staged_fd);throw Bad("staging sync failed");}
        if(close(staged_fd)!=0)throw Bad("staging close failed");
        auto staged_bytes=read_plain(staged,4u*1024u*1024u);
        if(sha(staged_bytes.data(),staged_bytes.size())!=bundle_pin)
            throw Bad("staged bundle identity mismatch");
        std::string cache=scratch+"/framework";
        if(!std::filesystem::create_directory(cache))throw Bad("cache setup failed");
        CelixLogRedirect log_redirect;
        auto* properties=celix_properties_create();
        celix_properties_set(properties,CELIX_FRAMEWORK_CACHE_DIR,cache.c_str());
        celix_properties_set(properties,"CELIX_LOGGING_DEFAULT_ACTIVE_LOG_LEVEL","warning");
        celix_properties_setBool(properties,CELIX_FRAMEWORK_CLEAN_CACHE_DIR_ON_CREATE,true);
        auto* framework=celix_frameworkFactory_createFramework(properties);
        if(!framework)throw Bad("Celix framework unavailable");
        struct Framework {celix_framework_t* p;
            ~Framework(){celix_frameworkFactory_destroyFramework(p);}} holder{framework};
        auto* ctx=celix_framework_getFrameworkContext(framework);
        if(celix_bundleContext_installBundle(ctx,staged.c_str(),true)<0)
            throw Bad("trusted verifier bundle failed installation");
        Call call{data.data(),size,expected};
        celix_service_use_options_t opts{};
        opts.filter.serviceName=TRONIX_ANSWER_VERIFY_SERVICE;
        opts.filter.versionRange="[1.0.0,2.0.0)";
        opts.callbackHandle=&call;
        opts.use=cb;
        bool found=celix_bundleContext_useServiceWithOptions(ctx,&opts);
        if(!found||!call.invoked)throw Bad("verifier service unavailable");
        if(call.result!=TRONIX_ANSWER_VERIFIED &&
           call.result!=TRONIX_ANSWER_REJECTED)throw Bad("verifier refused data");
        status=call.result;
        actual=call.actual;
    } catch(const std::exception& e) {
        std::cerr<<"tronix_verify_once: "<<e.what()<<"\n";
    }
    auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-start).count();
    emit(task,status,expected,actual,size,static_cast<uint64_t>(elapsed));
    return status==TRONIX_ANSWER_VERIFIED ? 0 :
           status==TRONIX_ANSWER_REJECTED ? 1 : 3;
}

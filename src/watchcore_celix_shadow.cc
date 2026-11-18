// SPDX-License-Identifier: Apache-2.0
// Numeric-only Rizzer shadow adapter via an actual pinned Apache Celix bundle.
#include "tronix_watchcore_security.h"
#include <celix_bundle_context.h>
#include <celix_constants.h>
#include <celix_framework.h>
#include <celix_framework_factory.h>
#include <celix_properties.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <array>
#include <charconv>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

namespace {
struct Bad : std::runtime_error { using std::runtime_error::runtime_error; };
constexpr size_t kMaxBundle = 16u * 1024u * 1024u;
using Digest = std::array<unsigned char, 32>;

int integer(const char *p) {
    const std::string_view s(p);
    int x = 0;
    auto [end,err] = std::from_chars(s.data(),s.data()+s.size(),x);
    if (s.empty() || err != std::errc{} || end != s.data()+s.size())
        throw Bad("noncanonical numeric input");
    return x;
}
Digest parse_pin(const char *str) {
    if (!str || std::strlen(str) != 64) throw Bad("bundle SHA-256 pin required");
    Digest d{};
    for (size_t j=0;j<32;++j) {
        auto digit=[](char c)->int {
            if (c>='0' && c<='9') return c-'0';
            if (c>='a' && c<='f') return c-'a'+10;
            throw Bad("bundle SHA-256 must be lowercase hex");
        };
        d[j] = static_cast<unsigned char>((digit(str[j*2])<<4)|digit(str[j*2+1]));
    }
    return d;
}
Digest sha256(const std::vector<unsigned char>& bytes) {
    Digest d{};
    unsigned int n = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), d.data(), &n, EVP_sha256(), nullptr)!=1
        || n!=32) throw Bad("sha256 failed");
    return d;
}
bool same(const Digest& a, const Digest& b) {
    return CRYPTO_memcmp(a.data(),b.data(),a.size())==0;
}
std::vector<unsigned char> read_bundle(const std::string& name) {
    std::filesystem::path p(name);
    if (!p.is_absolute()) throw Bad("absolute bundle path required");
    std::filesystem::path walk("/");
    for (const auto &part:p.relative_path()) {
        if (part=="." || part==".." || part.empty()) throw Bad("noncanonical bundle path");
        walk /= part;
        struct stat st{};
        if (lstat(walk.c_str(),&st)!=0 || S_ISLNK(st.st_mode))
            throw Bad("missing or symlinked bundle path");
    }
    int fd = open(name.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if (fd<0) throw Bad("bundle open failed");
    struct Guard {int fd; ~Guard(){close(fd);}} guard{fd};
    struct stat first{},last{};
    if (fstat(fd,&first)!=0 || !S_ISREG(first.st_mode) || first.st_size<=0 ||
        static_cast<unsigned long long>(first.st_size)>kMaxBundle ||
        (first.st_mode & 0022) || (first.st_uid!=getuid() && first.st_uid!=0))
        throw Bad("untrusted bundle permissions or size");
    std::vector<unsigned char> data;
    data.reserve(static_cast<size_t>(first.st_size));
    std::array<unsigned char,4096> chunk{};
    for (;;) {
        ssize_t n=read(fd,chunk.data(),chunk.size());
        if (n<0 && errno==EINTR) continue;
        if (n<0) throw Bad("bundle read failed");
        if (n==0) break;
        if (data.size()+static_cast<size_t>(n)>kMaxBundle) throw Bad("bundle too large");
        data.insert(data.end(),chunk.begin(),chunk.begin()+n);
    }
    if (fstat(fd,&last)!=0 || first.st_dev!=last.st_dev || first.st_ino!=last.st_ino ||
        first.st_size!=last.st_size || data.size()!=static_cast<size_t>(first.st_size) ||
        first.st_mtim.tv_sec!=last.st_mtim.tv_sec ||
        first.st_mtim.tv_nsec!=last.st_mtim.tv_nsec)
        throw Bad("bundle modified during read");
    return data;
}
struct PrivateDir {
    std::string path;
    PrivateDir() {
        char tmpl[]="/tmp/tronix-watchcore-XXXXXX";
        char *created=mkdtemp(tmpl);
        if (!created) throw Bad("private cache creation failed");
        path=created;
    }
    ~PrivateDir() { if (!path.empty()) {std::error_code ec;std::filesystem::remove_all(path,ec);} }
};
void stage(const std::string &path,const std::vector<unsigned char>& data) {
    int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if (fd<0) throw Bad("bundle staging failed");
    struct Guard {int fd; ~Guard(){close(fd);}} guard{fd};
    size_t offset=0;
    while (offset<data.size()) {
        ssize_t n=write(fd,data.data()+offset,data.size()-offset);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) throw Bad("bundle staging write failed");
        offset+=static_cast<size_t>(n);
    }
    if (fsync(fd)!=0) throw Bad("bundle staging sync failed");
}
struct QuietCelix {
    int saved{-1};
    QuietCelix() {
        std::fflush(stdout);
        saved=dup(STDOUT_FILENO);
        if (saved<0 || dup2(STDERR_FILENO,STDOUT_FILENO)<0) {
            if(saved>=0) close(saved);
            throw Bad("Celix output redirect failed");
        }
    }
    ~QuietCelix(){std::fflush(stdout);dup2(saved,STDOUT_FILENO);close(saved);}
};
struct Framework {
    celix_framework_t *fw{};
    explicit Framework(celix_properties_t *p):fw(celix_frameworkFactory_createFramework(p)) {
        if(!fw) throw Bad("Celix framework creation failed");
    }
    ~Framework(){celix_frameworkFactory_destroyFramework(fw);}
};
struct Call {
    tronix_watchcore_security_input_t in{};
    tronix_watchcore_security_output_t out{};
    bool called{false};
    int rc{-1};
};
void use(void *handle,void *service) {
    auto &call=*static_cast<Call*>(handle);
    auto *svc=static_cast<tronix_watchcore_security_service_t*>(service);
    if (!svc || !svc->decide) return;
    call.called=true;
    call.rc=svc->decide(svc->handle,&call.in,&call.out);
}
const char* risk_name(int n) {
    constexpr const char* names[]={"low","medium","high","critical"};
    if(n<0 || n>3) throw Bad("invalid native risk");
    return names[n];
}
const char* decision_name(int n) {
    constexpr const char* names[]={"proceed","warn","block"};
    if(n<0 || n>2) throw Bad("invalid native decision");
    return names[n];
}
tronix_watchcore_security_output_t invoke(const tronix_watchcore_security_input_t &in,
                                            const char *bundle,const Digest &pin) {
    auto bytes=read_bundle(bundle);
    if (!same(sha256(bytes),pin)) throw Bad("bundle identity mismatch");
    PrivateDir tmp;
    const std::string staged=tmp.path+"/watchcore.zip";
    stage(staged,bytes);
    if (!same(sha256(read_bundle(staged)),pin)) throw Bad("staged identity mismatch");
    auto cache=tmp.path+"/cache";
    if (!std::filesystem::create_directory(cache)) throw Bad("Celix cache creation failed");
    Call call;
    call.in=in;
    {
        QuietCelix quiet;
        auto *props=celix_properties_create();
        celix_properties_set(props,CELIX_FRAMEWORK_CACHE_DIR,cache.c_str());
        celix_properties_set(props,"CELIX_LOGGING_DEFAULT_ACTIVE_LOG_LEVEL","warning");
        celix_properties_setBool(props,CELIX_FRAMEWORK_CLEAN_CACHE_DIR_ON_CREATE,true);
        Framework fw(props);
        auto *ctx=celix_framework_getFrameworkContext(fw.fw);
        if (celix_bundleContext_installBundle(ctx,staged.c_str(),true)<0)
            throw Bad("Celix bundle install/start failed");
        celix_service_use_options_t opts{};
        opts.filter.serviceName=TRONIX_WATCHCORE_SECURITY_NAME;
        opts.filter.versionRange="[1.0.0,2.0.0)";
        opts.callbackHandle=&call;
        opts.use=use;
        if(!celix_bundleContext_useServiceWithOptions(ctx,&opts) || !call.called || call.rc!=0)
            throw Bad("Celix Watchcore service unavailable/invalid");
    }
    return call.out;
}
}
int main(int argc,char **argv) {
    if (argc!=13 || std::string_view(argv[1])!="security") {
        std::cerr<<"usage: tronix_watchcore_shadow security "
                    "h_score h_risk h_signals h_decisive m_score m_risk "
                    "m_fallback m_rc warn block corroboration\n";
        return 2;
    }
    try {
        const char *bundle=getenv("WATCHCORE_CELIX_BUNDLE");
        const char *raw_pin=getenv("WATCHCORE_CELIX_BUNDLE_SHA256");
        if(!bundle || !raw_pin) throw Bad("operator bundle and SHA256 pin required");
        const Digest pin=parse_pin(raw_pin);
        tronix_watchcore_security_input_t input{
            integer(argv[2]),integer(argv[3]),integer(argv[4]),integer(argv[5]),
            integer(argv[6]),integer(argv[7]),integer(argv[8]),integer(argv[9]),
            integer(argv[10]),integer(argv[11]),integer(argv[12])
        };
        const auto o=invoke(input,bundle,pin);
        std::cout<<"{\"score\":"<<o.score<<",\"risk\":\""<<risk_name(o.risk)
                 <<"\",\"decision\":\""<<decision_name(o.decision)
                 <<"\",\"block_downgraded_to_warn\":"
                 <<(o.block_downgraded_to_warn?"true":"false")
                 <<",\"fallback_upgraded_to_warn\":"
                 <<(o.fallback_upgraded_to_warn?"true":"false")<<"}\n";
        return 0;
    } catch(const std::exception &e) {
        std::cerr<<"Celix Watchcore shadow unavailable: "<<e.what()<<'\n';
        return 2;
    }
}

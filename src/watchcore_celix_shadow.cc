// SPDX-License-Identifier: Apache-2.0
// Numeric-only Rizzer shadow adapter via an actual pinned Apache Celix bundle.
#include "tronix_watchcore_security.h"
#include "tronix_watchcore_ledger.h"
#include <jansson.h>
#include <algorithm>
#include <memory>
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

// Shared trusted loader for distinct versioned Celix services in one pinned
// bundle. No local C++ fallback exists when discovery or execution fails.
void invoke_service(void* handle, void (*callback)(void*,void*),
                    const char* service_name, const char* bundle, const Digest& pin) {
    auto bytes=read_bundle(bundle);
    if (!same(sha256(bytes),pin)) throw Bad("bundle identity mismatch");
    PrivateDir tmp;
    const std::string staged=tmp.path+"/watchcore.zip";
    stage(staged,bytes);
    if (!same(sha256(read_bundle(staged)),pin)) throw Bad("staged identity mismatch");
    auto cache=tmp.path+"/cache";
    if (!std::filesystem::create_directory(cache)) throw Bad("Celix cache creation failed");
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
        opts.filter.serviceName=service_name;
        opts.filter.versionRange="[1.0.0,2.0.0)";
        opts.callbackHandle=handle;
        opts.use=callback;
        if (!celix_bundleContext_useServiceWithOptions(ctx,&opts))
            throw Bad("Celix Watchcore requested service unavailable");
    }
}
tronix_watchcore_security_output_t invoke(const tronix_watchcore_security_input_t &in,
                                            const char *bundle,const Digest &pin) {
    Call call;
    call.in=in;
    invoke_service(&call,use,TRONIX_WATCHCORE_SECURITY_NAME,bundle,pin);
    if (!call.called || call.rc!=0)
        throw Bad("Celix Watchcore security service unavailable/invalid");
    return call.out;
}

struct LedgerCall {
    tronix_watchcore_ledger_request_t in{};
    tronix_watchcore_ledger_result_t out{};
    bool called{false};
    int rc{-1};
};
void use_ledger(void* handle, void* service) {
    auto& call=*static_cast<LedgerCall*>(handle);
    auto* svc=static_cast<tronix_watchcore_ledger_service_t*>(service);
    if (!svc || !svc->decide) return;
    call.called=true;
    call.rc=svc->decide(svc->handle,&call.in,&call.out);
}
struct JsonDeleter {
    void operator()(json_t* ptr) const { if(ptr) json_decref(ptr); }
};
using JsonPtr=std::unique_ptr<json_t, JsonDeleter>;
json_t* require_key(json_t* obj,const char* key) {
    if (!json_is_object(obj)) throw Bad("expected a JSON object");
    auto* value=json_object_get(obj,key);
    if (!value) throw Bad("missing required JSON key");
    return value;
}
long long json_int(json_t* obj,const char* key) {
    auto* value=require_key(obj,key);
    if (!json_is_integer(value)) throw Bad("expected an integer");
    return json_integer_value(value);
}
int bounded_int(json_t* obj,const char* key,int lower,int upper) {
    auto n=json_int(obj,key);
    if(n<lower || n>upper) throw Bad("out-of-bounds integer");
    return static_cast<int>(n);
}
bool json_bool(json_t* obj,const char* key) {
    auto* value=require_key(obj,key);
    if (!json_is_boolean(value)) throw Bad("expected a boolean");
    return json_is_true(value);
}
std::string json_str(json_t* obj,const char* key,bool allow_empty=false) {
    auto* value=require_key(obj,key);
    if (!json_is_string(value)) throw Bad("expected string");
    size_t n=json_string_length(value);
    if (n>TRONIX_WATCHCORE_LEDGER_MAX_STRING || (!allow_empty && n==0))
        throw Bad("invalid string length");
    const char* data=json_string_value(value);
    for (size_t i=0;i<n;++i) {
        unsigned char c=static_cast<unsigned char>(data[i]);
        if(c<32 || c>=127) throw Bad("invalid string bytes");
    }
    return std::string(data,n);
}
struct RowOwned {
    std::string repo;
    std::string revision;
    tronix_watchcore_ledger_row_t row{};
};
struct LedgerOwned {
    std::string repo, revision, default_repo;
    std::vector<RowOwned> owned;
    std::vector<tronix_watchcore_ledger_row_t> rows;
    tronix_watchcore_ledger_request_t req{};
};
LedgerOwned parse_ledger_input() {
    // Bounded stdin, not argv: keep repo IDs/revision strings out of ps output.
    std::string data;
    data.reserve(4096);
    char buf[4096];
    for (;;) {
        ssize_t n=read(STDIN_FILENO,buf,sizeof(buf));
        if (n<0 && errno==EINTR) continue;
        if (n<0) throw Bad("ledger stdin read failed");
        if (n==0) break;
        if (data.size()+static_cast<size_t>(n)>65536)
            throw Bad("ledger JSON exceeds 64 KiB");
        data.append(buf,static_cast<size_t>(n));
    }
    json_error_t error{};
    JsonPtr document(json_loadb(data.data(),data.size(),JSON_REJECT_DUPLICATES,&error));
    if(!document || !json_is_object(document.get()) || json_object_size(document.get())!=10)
        throw Bad("invalid ledger JSON root schema");
    LedgerOwned o;
    o.repo=json_str(document.get(),"repo");
    o.revision=json_str(document.get(),"revision");
    o.default_repo=json_str(document.get(),"default_repo",true);
    const int kind=bounded_int(document.get(),"kind",0,1);
    const int64_t number=json_int(document.get(),"number");
    if(number<1) throw Bad("invalid item number");
    const int64_t now=json_int(document.get(),"now");
    const int max_retries=bounded_int(document.get(),"max_retries",0,10000);
    const int64_t base=json_int(document.get(),"backoff_base_seconds");
    const int64_t max=json_int(document.get(),"backoff_max_seconds");
    if(base<0 || max<0) throw Bad("negative retry policy");
    auto* arr=require_key(document.get(),"rows");
    if(!json_is_array(arr) || json_array_size(arr)>TRONIX_WATCHCORE_LEDGER_MAX_ROWS)
        throw Bad("oversized/nonarray ledger");
    o.owned.reserve(json_array_size(arr));
    for (size_t index=0;index<json_array_size(arr);++index) {
        json_t* item=json_array_get(arr,index);
        if(!json_is_object(item) || json_object_size(item)!=8)
            throw Bad("invalid ledger row schema");
        RowOwned own{};
        own.repo=json_str(item,"repo",true);
        own.revision=json_str(item,"revision");
        own.row.kind=bounded_int(item,"kind",0,1);
        own.row.number=json_int(item,"number");
        if(own.row.number<1) throw Bad("invalid row number");
        own.row.status=bounded_int(item,"status",0,4);
        own.row.has_timestamp=json_bool(item,"has_timestamp")?1:0;
        own.row.timestamp_utc_seconds=json_int(item,"timestamp");
        own.row.failure_was_limit=json_bool(item,"failure_was_limit")?1:0;
        o.owned.push_back(std::move(own));
    }
    o.rows.reserve(o.owned.size());
    for (auto& owner:o.owned) {
        owner.row.repo=owner.repo.c_str();
        owner.row.revision=owner.revision.c_str();
        o.rows.push_back(owner.row);
    }
    o.req={o.repo.c_str(),kind,number,o.revision.c_str(),o.default_repo.c_str(),
           now,static_cast<uint32_t>(max_retries),base,max,
           o.rows.empty()?nullptr:o.rows.data(),o.rows.size()};
    return o;
}
tronix_watchcore_ledger_result_t invoke_ledger(LedgerOwned& o,
                                                const char* bundle,const Digest& pin) {
    // Rebind after parse_ledger_input() returns: short-string optimization
    // means moved std::string::c_str() addresses are not stable across moves.
    o.req.repo=o.repo.c_str();
    o.req.revision=o.revision.c_str();
    o.req.default_repo=o.default_repo.c_str();
    for (size_t index=0;index<o.owned.size();++index) {
        o.rows[index].repo=o.owned[index].repo.c_str();
        o.rows[index].revision=o.owned[index].revision.c_str();
    }
    o.req.rows=o.rows.empty()?nullptr:o.rows.data();
    LedgerCall call;
    call.in=o.req;
    invoke_service(&call,use_ledger,TRONIX_WATCHCORE_LEDGER_NAME,bundle,pin);
    if (!call.called || call.rc!=0)
        throw Bad("Celix Watchcore ledger service unavailable/invalid");
    return call.out;
}
const char* eligibility_name(int value) {
    static constexpr const char* names[]={
        "ready","already_terminal","retries_exhausted","backoff"
    };
    if(value<0 || value>3) throw Bad("invalid ledger eligibility");
    return names[value];
}

}
int main(int argc,char **argv) {
    const bool security_mode=argc==13 && std::string_view(argv[1])=="security";
    const bool ledger_mode=argc==2 && std::string_view(argv[1])=="ledger";
    if (!security_mode && !ledger_mode) {
        std::cerr<<"usage: tronix_watchcore_shadow security "
                    "h_score h_risk h_signals h_decisive m_score m_risk "
                    "m_fallback m_rc warn block corroboration | ledger < validated.json\n";
        return 2;
    }
    try {
        const char *bundle=getenv("WATCHCORE_CELIX_BUNDLE");
        const char *raw_pin=getenv("WATCHCORE_CELIX_BUNDLE_SHA256");
        if(!bundle || !raw_pin) throw Bad("operator bundle and SHA256 pin required");
        const Digest pin=parse_pin(raw_pin);
        if (ledger_mode) {
            LedgerOwned owner=parse_ledger_input();
            const auto o=invoke_ledger(owner,bundle,pin);
            std::cout<<"{\"eligibility\":\""<<eligibility_name(o.eligibility)
                     <<"\",\"failure_count\":"<<o.failure_count
                     <<",\"next_retry_utc_seconds\":";
            if (o.has_next_retry) std::cout<<o.next_retry_utc_seconds;
            else std::cout<<"null";
            std::cout<<"}\n";
            return 0;
        }
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

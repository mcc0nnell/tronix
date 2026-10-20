// TRONIX opt-in one-shot Celix operation worker.
// This is fault and timeout containment, NOT a security sandbox.
#include "tronix_service.h"
#include <celix_bundle_context.h>
#include <celix_constants.h>
#include <celix_framework.h>
#include <celix_framework_factory.h>
#include <celix_properties.h>
#include <msgpack.hpp>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <vector>
#include <set>
#include <string>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
constexpr int OUTPUT_FD=3;
void limit(int resource,rlim_t amount){
    const rlimit lim{amount,amount};
    if(setrlimit(resource,&lim)!=0){std::perror("setrlimit");std::_Exit(72);}
}
void send_result(bool ok,int64_t value){
    msgpack::sbuffer out;
    msgpack::packer<msgpack::sbuffer> pk(out);
    pk.pack_map(3);
    pk.pack(std::string("v"));pk.pack(1);
    pk.pack(std::string("ok"));pk.pack(ok);
    pk.pack(std::string("value"));pk.pack(value);
    if(out.size()>256)std::_Exit(73);
    size_t pos=0;
    while(pos<out.size()){
        ssize_t n=write(OUTPUT_FD,out.data()+pos,out.size()-pos);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)std::_Exit(74);
        pos+=static_cast<size_t>(n);
    }
}
}
struct OpPolicy {std::string name;int lane;uint32_t budget;};
void send_policy(const std::vector<OpPolicy>& policies){
    msgpack::sbuffer out;
    msgpack::packer<msgpack::sbuffer> pk(out);
    pk.pack_map(3);
    pk.pack(std::string("v"));pk.pack(1);
    pk.pack(std::string("ok"));pk.pack(true);
    pk.pack(std::string("operations"));
    pk.pack_array(static_cast<uint32_t>(policies.size()));
    for(const auto& entry:policies){
        pk.pack_map(3);
        pk.pack(std::string("name"));pk.pack(entry.name);
        pk.pack(std::string("lane"));pk.pack(entry.lane);
        pk.pack(std::string("budgetMs"));pk.pack(entry.budget);
    }
    if(out.size()>2048)std::_Exit(73);
    size_t pos=0;
    while(pos<out.size()){
        ssize_t n=write(OUTPUT_FD,out.data()+pos,out.size()-pos);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)std::_Exit(74);
        pos+=static_cast<size_t>(n);
    }
}
bool op_valid(const std::string& name){
    if(name.empty()||name.size()>TRONIX_OP_MAX)return false;
    for(unsigned char ch:name)if(ch<33||ch>126||ch==',')return false;
    return true;
}
int main(int argc,char** argv) {
    const bool inspect=argc==6 &&
        std::strcmp(argv[1],"--adapter")==0 &&
        std::strcmp(argv[3],"--cache")==0 &&
        std::strcmp(argv[5],"--inspect")==0;
    const bool execute=argc==11 &&
        std::strcmp(argv[1],"--adapter")==0 &&
        std::strcmp(argv[3],"--operation")==0 &&
        std::strcmp(argv[5],"--cache")==0 &&
        std::strcmp(argv[7],"--lane")==0 &&
        std::strcmp(argv[9],"--budget-ms")==0;
    if(!inspect&&!execute)return 2;
    const std::string adapter=argv[2],op=execute?argv[4]:"",cache=inspect?argv[4]:argv[6];
    int expected_lane=-1;
    unsigned long expected_budget=0;
    if(execute){
        if((strcmp(argv[8],"0")!=0 && strcmp(argv[8],"1")!=0) ||
           strlen(argv[10])>5 || !*argv[10])return 2;
        for(const char* c=argv[10];*c;c++)if(*c<'0'||*c>'9')return 2;
        expected_lane=argv[8][0]-'0';
        expected_budget=strtoul(argv[10],NULL,10);
        if(expected_budget==0||expected_budget>30000)return 2;
    }
    if(adapter.empty()||adapter[0]!='/'||(execute&&!op_valid(op))||
       cache.rfind("/tmp/tronix-worker-",0)!=0)return 2;
    // The parent spawns us with a single reviewed adapter and a trusted op.
    // A child worker may still access the host under its UID; no sandbox claim.
    if(prctl(PR_SET_PDEATHSIG,SIGKILL)!=0)return 3;
    if(getppid()==1)return 3;
    limit(RLIMIT_CORE,0);
    limit(RLIMIT_NOFILE,64);
    limit(RLIMIT_AS,1024ULL*1024*1024);
    limit(RLIMIT_CPU,5);
    celix_properties_t* cfg=celix_properties_create();
    // Parent creates and cleans this private directory even after SIGKILL.
    celix_properties_set(cfg,CELIX_FRAMEWORK_CACHE_DIR,cache.c_str());
    celix_properties_setBool(cfg,CELIX_FRAMEWORK_CLEAN_CACHE_DIR_ON_CREATE,true);
    auto* framework=celix_frameworkFactory_createFramework(cfg);
    if(!framework)return 5;
    auto* ctx=celix_framework_getFrameworkContext(framework);
    if(celix_bundleContext_installBundle(ctx,adapter.c_str(),true)<0){
        celix_frameworkFactory_destroyFramework(framework);
        return 6;
    }
    int ok=0;int64_t value=0;
    bool described=false;
    std::vector<OpPolicy> policies;
    std::function<void(tronix_operation_service_t*)> callback=[&](auto* svc){
        if(inspect){
            if(!svc->operations||!svc->lane_for||!svc->budget_for)return;
            const char* raw=svc->operations(svc->handle);
            if(!raw)return;
            std::string names(raw);
            if(names.size()>1000)return;
            std::set<std::string> seen;
            size_t begin=0;
            while(begin<names.size()){
                const auto end=names.find(',',begin);
                std::string name=names.substr(begin,end==std::string::npos?
                                          std::string::npos:end-begin);
                if(!op_valid(name)||!seen.insert(name).second||policies.size()>=16){
                    policies.clear();return;
                }
                const int lane=svc->lane_for(svc->handle,name.c_str());
                const uint32_t budget=svc->budget_for(svc->handle,name.c_str());
                if((lane!=0&&lane!=1)||budget==0||budget>30000){
                    policies.clear();return;
                }
                policies.push_back({name,lane,budget});
                if(end==std::string::npos)break;
                if(end+1==names.size()){policies.clear();return;}
                begin=end+1;
            }
            described=!policies.empty();
            return;
        }
        // Validate authorization in the child too, rather than merely
        // trusting the parent to pass a known operation.
        if(!svc->execute || !svc->lane_for || !svc->budget_for ||
           !svc->operations)return;
        const char* available=svc->operations(svc->handle);
        if(!available)return;
        const std::string names=available;
        bool listed=false;
        size_t from=0;
        while(from<names.size()){
            const auto end=names.find(',',from);
            if(names.substr(from,end==std::string::npos?
                                   std::string::npos:end-from)==op){listed=true;break;}
            if(end==std::string::npos)break;
            from=end+1;
        }
        if(!listed)return;
        const int lane=svc->lane_for(svc->handle,op.c_str());
        const uint32_t budget=svc->budget_for(svc->handle,op.c_str());
        if(lane!=expected_lane || budget!=expected_budget)return;
        ok=svc->execute(svc->handle,op.c_str(),&value);
    };
    celix_service_use_options_t opts{};
    opts.filter.serviceName=TRONIX_OPERATION_SERVICE;
    opts.filter.versionRange="[2.0.0,3.0.0)";
    opts.callbackHandle=&callback;
    opts.use=[](void* p,void* svc){
        (*static_cast<std::function<void(tronix_operation_service_t*)>*>(p))(
            static_cast<tronix_operation_service_t*>(svc));
    };
    bool found=celix_bundleContext_useServiceWithOptions(ctx,&opts);
    // Destruct the in-process Celix framework before signaling completion,
    // so an operation that hangs during cleanup can also be contained.
    celix_frameworkFactory_destroyFramework(framework);
    if(inspect){
        if(!found||!described)return 7;
        send_policy(policies);
    }else send_result(found && ok,value);
    return 0;
}

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
int main(int argc,char** argv) {
    if(argc!=7 || std::strcmp(argv[1],"--adapter")!=0 ||
       std::strcmp(argv[3],"--operation")!=0 ||
       std::strcmp(argv[5],"--cache")!=0)return 2;
    const std::string adapter=argv[2],op=argv[4],cache=argv[6];
    if(adapter.empty()||adapter[0]!='/'||op.empty()||op.size()>TRONIX_OP_MAX||
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
    std::function<void(tronix_operation_service_t*)> callback=[&](auto* svc){
        // Validate authorization in the child too, rather than merely
        // trusting the parent to pass a known operation.
        if(svc->execute && svc->lane_for && svc->budget_for &&
           (svc->lane_for(svc->handle,op.c_str())==0 ||
            svc->lane_for(svc->handle,op.c_str())==1) &&
            svc->budget_for(svc->handle,op.c_str())>0)
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
    send_result(found && ok,value);
    return 0;
}

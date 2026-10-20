// TRONIX v0.1 — experimental Celix services + bounded MessagePack IPC.
#include "tronix_service.h"
#include "events.hpp"
#include <celix_bundle_context.h>
#include <celix_constants.h>
#include <celix_framework.h>
#include <celix_framework_factory.h>
#include <celix_properties.h>
#include <msgpack.hpp>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
constexpr uint32_t MAX_FRAME=4096;
volatile sig_atomic_t stop_requested=0;
void signal_handler(int){stop_requested=1;}
[[noreturn]] void bad(const std::string& msg){throw std::runtime_error(msg);}
template<class S,class F> bool with_service(celix_bundle_context_t* ctx,const char* name,F fn) {
    std::function<void(S*)> cb=fn;
    celix_service_use_options_t opts{};
    opts.filter.serviceName=name;
    opts.filter.versionRange="[2.0.0,3.0.0)";
    opts.callbackHandle=&cb;
    opts.use=[](void* h,void* s){(*static_cast<std::function<void(S*)>*>(h))(static_cast<S*>(s));};
    return celix_bundleContext_useServiceWithOptions(ctx,&opts);
}
struct Runtime {
    celix_bundle_context_t* ctx;
    tronix::Ledger* ledger;
    std::atomic<bool> running{true};
    std::atomic<uint64_t> admissions{0};
    std::mutex wake_mutex;
    std::condition_variable wake;
};
void worker(Runtime* r){
    while(r->running.load(std::memory_order_acquire)){
        // Capture the generation before claim so an admission between claim
        // and wait cannot be lost.
        const uint64_t observed=r->admissions.load(std::memory_order_acquire);
        tronix_job_t j{};int claimed=0;
        with_service<tronix_scheduler_service_t>(r->ctx,TRONIX_SCHEDULER_SERVICE,
            [&](auto* s){claimed=s->claim(s->handle,&j);});
        if(claimed==1){
            if(!r->ledger->emit(j.id,j.agent,j.operation,"running",j.dispatch_seq))
                std::fprintf(stderr,"TRONIX_LEDGER_ERROR running %s\n",j.id);
            int64_t value=0;int ok=0;
            const auto began=std::chrono::steady_clock::now();
            with_service<tronix_operation_service_t>(r->ctx,TRONIX_OPERATION_SERVICE,
                [&](auto* s){ok=s->execute(s->handle,j.operation,&value);});
            const auto execution_ns=static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now()-began).count());
            tronix_job_t terminal{};
            with_service<tronix_scheduler_service_t>(r->ctx,TRONIX_SCHEDULER_SERVICE,
                [&](auto* s){
                    if(s->complete(s->handle,j.id,ok,value,execution_ns))
                        s->lookup(s->handle,j.id,&terminal);
                });
            if(terminal.state) {
                const std::string state=terminal.state==TRONIX_DONE?"completed":
                    terminal.state==TRONIX_OVERRUN?"overrun":"failed";
                if(!r->ledger->emit(j.id,j.agent,j.operation,state,j.dispatch_seq,
                                     terminal.state==TRONIX_DONE,terminal.value))
                    std::fprintf(stderr,"TRONIX_LEDGER_ERROR terminal %s\n",j.id);
            }
        } else {
            std::unique_lock<std::mutex> lock(r->wake_mutex);
            r->wake.wait(lock,[&]{
                return !r->running.load(std::memory_order_acquire) ||
                    r->admissions.load(std::memory_order_acquire)!=observed;
            });
        }
    }
}
struct Request{std::string action,id,agent,operation;uint32_t deadline_ms=0,limit=16;uint64_t after=0;};
bool printable(const std::string& s,size_t max){
    if(s.empty()||s.size()>max)return false;
    for(unsigned char c:s)if(c<33||c>126)return false;
    return true;
}
Request parse(const std::vector<char>& payload){
    auto oh=msgpack::unpack(payload.data(),payload.size());
    const auto& obj=oh.get();
    if(obj.type!=msgpack::type::MAP)bad("BAD_REQUEST");
    Request out;std::set<std::string> keys;
    for(uint32_t i=0;i<obj.via.map.size;i++){
        const auto& item=obj.via.map.ptr[i];
        if(item.key.type!=msgpack::type::STR)bad("BAD_REQUEST");
        std::string key=item.key.as<std::string>();
        if(!keys.insert(key).second)bad("BAD_REQUEST");
        if(key=="v"){
            if(item.val.type!=msgpack::type::POSITIVE_INTEGER || item.val.as<uint64_t>()!=1)bad("BAD_VERSION");
        }else if(key=="action"||key=="id"||key=="agent"||key=="operation"){
            if(item.val.type!=msgpack::type::STR)bad("BAD_REQUEST");
            std::string v=item.val.as<std::string>();
            if(key=="action")out.action=v;
            else if(key=="id")out.id=v;
            else if(key=="agent")out.agent=v;
            else out.operation=v;
        }else if(key=="after"){
            if(item.val.type!=msgpack::type::POSITIVE_INTEGER)bad("BAD_REQUEST");
            out.after=item.val.as<uint64_t>();
        }else if(key=="limit"){
            if(item.val.type!=msgpack::type::POSITIVE_INTEGER)bad("BAD_REQUEST");
            uint64_t n=item.val.as<uint64_t>();
            if(n<1 || n>16)bad("BAD_REQUEST");
            out.limit=static_cast<uint32_t>(n);
        }else if(key=="deadlineMs"){
            if(item.val.type!=msgpack::type::POSITIVE_INTEGER)bad("BAD_REQUEST");
            uint64_t n=item.val.as<uint64_t>();
            if(n>30000)bad("BAD_REQUEST");
            out.deadline_ms=static_cast<uint32_t>(n);
        }else bad("BAD_REQUEST");
    }
    const std::set<std::string> submission={"v","action","id","agent","operation","deadlineMs"};
    const std::set<std::string> by_id={"v","action","id"};
    const std::set<std::string> simple={"v","action"};
    const std::set<std::string> event_keys={"v","action","after","limit"};
    if(out.action=="submit"){
        if(keys!=submission||!printable(out.id,TRONIX_ID_MAX)||
           !printable(out.agent,TRONIX_AGENT_MAX)||!printable(out.operation,TRONIX_OP_MAX)||
           out.deadline_ms==0)bad("BAD_REQUEST");
    } else if(out.action=="result"||out.action=="ack"||out.action=="receipt"){
        if(keys!=by_id||!printable(out.id,TRONIX_ID_MAX))bad("BAD_REQUEST");
    }else if(out.action=="events"){
        if(keys!=event_keys)bad("BAD_REQUEST");
    }else if(out.action=="ping"||out.action=="snapshot"||out.action=="capabilities"){
        if(keys!=simple)bad("BAD_REQUEST");
    }else bad("BAD_ACTION");
    return out;
}
using P=msgpack::packer<msgpack::sbuffer>;
void key(P& pk,const char* str){pk.pack(std::string(str));}
void pack_event(P& pk,const tronix::Event& e) {
    pk.pack_map(e.has_value?9:8);
    key(pk,"seq");pk.pack(e.seq);
    key(pk,"timeMs");pk.pack(e.time_ms);
    key(pk,"id");pk.pack(e.id);
    key(pk,"agent");pk.pack(e.agent);
    key(pk,"operation");pk.pack(e.operation);
    key(pk,"state");pk.pack(e.state);
    key(pk,"dispatchSequence");pk.pack(e.dispatch);
    key(pk,"kind");pk.pack(std::string("agent.lifecycle"));
    if(e.has_value){key(pk,"value");pk.pack(e.value);}
}
std::string reply(celix_bundle_context_t* ctx,const Request& q,Runtime* runtime){
    msgpack::sbuffer b;P pk(b);
    if(q.action=="ping"){
        pk.pack_map(3);key(pk,"v");pk.pack(1);key(pk,"ok");pk.pack(true);
        key(pk,"runtime");pk.pack(std::string("tronix/0.2"));
    }else if(q.action=="capabilities"){
        std::string list;
        bool found=with_service<tronix_operation_service_t>(ctx,TRONIX_OPERATION_SERVICE,
           [&](auto* svc){if(svc->operations)list=svc->operations(svc->handle);});
        std::vector<std::string> ops;
        size_t pos=0;
        while(pos<list.size()){
            size_t end=list.find(',',pos);
            if(end==std::string::npos)end=list.size();
            if(end>pos)ops.emplace_back(list.substr(pos,end-pos));
            pos=end+1;
        }
        pk.pack_map(4);key(pk,"v");pk.pack(1);key(pk,"ok");pk.pack(found);
        key(pk,"service");pk.pack(std::string(TRONIX_OPERATION_SERVICE));
        key(pk,"operations");pk.pack_array(static_cast<uint32_t>(ops.size()));
        for(const auto& op:ops)pk.pack(op);
    }else if(q.action=="submit"){
        // Trusted op policy chooses priority. Never accept caller-supplied lanes.
        int lane=-1;uint32_t budget_ms=0;
        with_service<tronix_operation_service_t>(ctx,TRONIX_OPERATION_SERVICE,
            [&](auto* svc){
                if(svc->lane_for)lane=svc->lane_for(svc->handle,q.operation.c_str());
                if(svc->budget_for)budget_ms=svc->budget_for(svc->handle,q.operation.c_str());
            });
        int code=-5;
        if(runtime->ledger->durable() && (!runtime->ledger->can_admit() ||
           runtime->ledger->known(q.id))) {
            code=runtime->ledger->can_admit()?-3:-6;
        }else if((lane==0||lane==1)&&budget_ms>0&&budget_ms<=30000){
            code=-4;
            with_service<tronix_scheduler_service_t>(ctx,TRONIX_SCHEDULER_SERVICE,
                [&](auto* s){code=s->admit(s->handle,q.id.c_str(),q.agent.c_str(),
                                           q.operation.c_str(),lane,q.deadline_ms,budget_ms);});
        }
        if(code==0){
            if(!runtime->ledger->emit(q.id,q.agent,q.operation,"queued"))
                std::fprintf(stderr,"TRONIX_LEDGER_ERROR queued %s\n",q.id.c_str());
            // Hold the same mutex as wait(): no notify-before-sleep gap.
            {
                std::lock_guard<std::mutex> guard(runtime->wake_mutex);
                runtime->admissions.fetch_add(1,std::memory_order_release);
            }
            runtime->wake.notify_one();
        }
        pk.pack_map(4);key(pk,"v");pk.pack(1);key(pk,"ok");pk.pack(code==0);
        key(pk,"id");pk.pack(q.id);key(pk,code==0?"state":"error");
        pk.pack(std::string(code==0?"queued":code==-2?"QUEUE_FULL":
            code==-3?"DUPLICATE_ID":code==-6?"LEDGER_UNAVAILABLE":code==-5?"DENIED":code==-4?"NO_SCHEDULER":"BAD_REQUEST"));
    }else if(q.action=="result"){
        tronix_job_t j{};int found=0;
        with_service<tronix_scheduler_service_t>(ctx,TRONIX_SCHEDULER_SERVICE,
            [&](auto* s){found=s->lookup(s->handle,q.id.c_str(),&j);});
        if(found && j.state==TRONIX_EXPIRED)
            runtime->ledger->emit(j.id,j.agent,j.operation,"expired",j.dispatch_seq);
        pk.pack_map(found?(j.state==TRONIX_DONE?9:8):5);
        key(pk,"v");pk.pack(1);key(pk,"ok");pk.pack(found!=0);
        key(pk,"id");pk.pack(q.id);key(pk,found?"state":"error");
        pk.pack(std::string(!found?"NOT_FOUND":j.state==TRONIX_QUEUED?"queued":
            j.state==TRONIX_RUNNING?"running":j.state==TRONIX_DONE?"completed":
            j.state==TRONIX_FAILED?"failed":j.state==TRONIX_OVERRUN?"overrun":"expired"));
        key(pk,"dispatchSequence");pk.pack(j.dispatch_seq);
        if(found){
            key(pk,"budgetMs");pk.pack(j.budget_ms);
            key(pk,"queueWaitNs");pk.pack(j.queue_wait_ns);
            key(pk,"executionNs");pk.pack(j.execution_ns);
        }
        if(found&&j.state==TRONIX_DONE){key(pk,"value");pk.pack(j.value);}
    }else if(q.action=="ack"){
        tronix_job_t prior{};
        with_service<tronix_scheduler_service_t>(ctx,TRONIX_SCHEDULER_SERVICE,
            [&](auto* s){s->lookup(s->handle,q.id.c_str(),&prior);});
        int done=0;
        with_service<tronix_scheduler_service_t>(ctx,TRONIX_SCHEDULER_SERVICE,
            [&](auto* s){done=s->ack(s->handle,q.id.c_str());});
        if(done && prior.state) {
            if(prior.state==TRONIX_EXPIRED)
                runtime->ledger->emit(prior.id,prior.agent,prior.operation,"expired",prior.dispatch_seq);
            runtime->ledger->emit(prior.id,prior.agent,prior.operation,"acked",prior.dispatch_seq);
        }
        pk.pack_map(3);key(pk,"v");pk.pack(1);key(pk,"ok");pk.pack(done!=0);
        key(pk,"state");pk.pack(std::string(done?"acked":"NOT_TERMINAL"));
    }else if(q.action=="receipt"){
        tronix::Event e;
        const bool found=runtime->ledger->get(q.id,e);
        pk.pack_map(found?4:3);key(pk,"v");pk.pack(1);
        key(pk,"ok");pk.pack(found);
        if(found){key(pk,"receipt");pack_event(pk,e);key(pk,"persisted");pk.pack(runtime->ledger->durable());}
        else{key(pk,"error");pk.pack(std::string("NOT_FOUND"));}
    }else if(q.action=="events"){
        // Expiry can occur while the single execution worker is busy.
        // Reconcile only bounded pending IDs against the native scheduler.
        for(const auto& pending:runtime->ledger->pending()) {
            tronix_job_t j{};int found=0;
            with_service<tronix_scheduler_service_t>(ctx,TRONIX_SCHEDULER_SERVICE,
                [&](auto* s){found=s->lookup(s->handle,pending.id.c_str(),&j);});
            if(found && j.state==TRONIX_EXPIRED)
                runtime->ledger->emit(j.id,j.agent,j.operation,"expired",j.dispatch_seq);
        }
        auto ev=runtime->ledger->since(q.after,q.limit);
        auto oldest=runtime->ledger->oldest();
        pk.pack_map(7);
        key(pk,"v");pk.pack(1);key(pk,"ok");pk.pack(true);
        key(pk,"oldestSeq");pk.pack(oldest);
        key(pk,"lastSeq");pk.pack(runtime->ledger->last());
        key(pk,"truncated");pk.pack(oldest>0 && q.after<oldest-1);
        key(pk,"persisted");pk.pack(runtime->ledger->durable());
        key(pk,"events");pk.pack_array(static_cast<uint32_t>(ev.size()));
        for(const auto& e:ev)pack_event(pk,e);
    }else if(q.action=="snapshot"){
        tronix_stats_t s{};
        with_service<tronix_scheduler_service_t>(ctx,TRONIX_SCHEDULER_SERVICE,
            [&](auto* svc){svc->snapshot(svc->handle,&s);});
        pk.pack_map(3);key(pk,"v");pk.pack(1);key(pk,"ok");pk.pack(true);
        key(pk,"stats");pk.pack_map(15);
        key(pk,"admitted");pk.pack(s.admitted);key(pk,"rejected");pk.pack(s.rejected);
        key(pk,"expired");pk.pack(s.expired);key(pk,"dispatched");pk.pack(s.dispatched);
        key(pk,"completed");pk.pack(s.completed);key(pk,"failed");pk.pack(s.failed);
        key(pk,"pending");pk.pack(s.pending);key(pk,"active");pk.pack(s.active);
        key(pk,"peakPending");pk.pack(s.peak_pending);
        key(pk,"dispatchSequence");pk.pack(s.sequence);
        key(pk,"overrun");pk.pack(s.overrun);
        key(pk,"totalQueueWaitNs");pk.pack(s.total_queue_wait_ns);
        key(pk,"maxQueueWaitNs");pk.pack(s.max_queue_wait_ns);
        key(pk,"totalExecutionNs");pk.pack(s.total_execution_ns);
        key(pk,"maxExecutionNs");pk.pack(s.max_execution_ns);
    }
    return std::string(b.data(),b.size());
}
bool exact_read(int fd,void* ptr,size_t len){
    auto* b=(char*)ptr;size_t pos=0;
    while(pos<len){ssize_t n=recv(fd,b+pos,len-pos,0);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return false;pos+=static_cast<size_t>(n);}
    return true;
}
bool exact_send(int fd,const void* ptr,size_t len){
    auto* b=(const char*)ptr;size_t pos=0;
    while(pos<len){ssize_t n=send(fd,b+pos,len-pos,MSG_NOSIGNAL);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return false;pos+=static_cast<size_t>(n);}
    return true;
}
void send_frame(int fd,const std::string& bytes){
    if(bytes.empty()||bytes.size()>MAX_FRAME)return;
    uint32_t n=htonl(static_cast<uint32_t>(bytes.size()));
    if(exact_send(fd,&n,4))exact_send(fd,bytes.data(),bytes.size());
}
void handle(int fd,celix_bundle_context_t* ctx,Runtime* runtime){
    uint32_t wire=0;if(!exact_read(fd,&wire,4))return;
    uint32_t len=ntohl(wire);if(len==0||len>MAX_FRAME)return;
    std::vector<char> bytes(len);if(!exact_read(fd,bytes.data(),len))return;
    try{send_frame(fd,reply(ctx,parse(bytes),runtime));}
    catch(const std::exception& e){
        msgpack::sbuffer b;P pk(b);pk.pack_map(3);key(pk,"v");pk.pack(1);
        key(pk,"ok");pk.pack(false);key(pk,"error");
        pk.pack(std::string(std::strcmp(e.what(),"BAD_VERSION")==0?"BAD_VERSION":"BAD_REQUEST"));
        send_frame(fd,std::string(b.data(),b.size()));
    }
}
}
int main(int argc,char** argv){
    if((argc!=7&&argc!=9)||std::strcmp(argv[1],"--socket")!=0||
       std::strcmp(argv[3],"--scheduler")!=0||std::strcmp(argv[5],"--adapter")!=0||
       (argc==9 && std::strcmp(argv[7],"--journal")!=0)){
        std::fprintf(stderr,"usage: tronixd --socket PRIVATE.sock --scheduler scheduler.zip --adapter adapter.zip [--journal PRIVATE.events]\n");
        return 2;
    }
    const std::string path=argv[2];
    if(path.empty()||path.size()>=sizeof(((sockaddr_un*)0)->sun_path)||path[0]!='/')return 2;
    struct stat st{};
    if(lstat(path.c_str(),&st)==0){std::fprintf(stderr,"refusing to replace existing path\n");return 2;}
    std::unique_ptr<tronix::Ledger> ledger;
    try { ledger=std::make_unique<tronix::Ledger>(argc==9?argv[8]:""); }
    catch(const std::exception& e) {
        std::fprintf(stderr,"journal initialization failed: %s\n",e.what());
        return 2;
    }
    celix_properties_t* cfg=celix_properties_create();
    char cache_name[]="/tmp/tronix-celix-XXXXXX";
    if(!mkdtemp(cache_name)){std::fprintf(stderr,"private Celix cache allocation failed\n");return 3;}
    const std::string cache=cache_name;
    celix_properties_set(cfg,CELIX_FRAMEWORK_CACHE_DIR,cache.c_str());
    celix_properties_setBool(cfg,CELIX_FRAMEWORK_CLEAN_CACHE_DIR_ON_CREATE,true);
    auto* framework=celix_frameworkFactory_createFramework(cfg);
    if(!framework){std::fprintf(stderr,"Celix initialization failed\n");std::filesystem::remove_all(cache);return 3;}
    auto* ctx=celix_framework_getFrameworkContext(framework);
    if(celix_bundleContext_installBundle(ctx,argv[4],true)<0 ||
       celix_bundleContext_installBundle(ctx,argv[6],true)<0){
        std::fprintf(stderr,"bundle installation failed\n");
        celix_frameworkFactory_destroyFramework(framework);std::filesystem::remove_all(cache);return 3;
    }
    bool sch=with_service<tronix_scheduler_service_t>(ctx,TRONIX_SCHEDULER_SERVICE,[](auto*){});
    bool ada=with_service<tronix_operation_service_t>(ctx,TRONIX_OPERATION_SERVICE,[](auto*){});
    if(!sch||!ada){std::fprintf(stderr,"required services missing\n");
        celix_frameworkFactory_destroyFramework(framework);std::filesystem::remove_all(cache);return 3;}
    Runtime rt{ctx,ledger.get()};std::thread work(worker,&rt);
    std::signal(SIGINT,signal_handler);std::signal(SIGTERM,signal_handler);
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0);bool bound=false;
    if(fd>=0){
        sockaddr_un addr{};addr.sun_family=AF_UNIX;std::strcpy(addr.sun_path,path.c_str());
        mode_t previous=umask(0077);
        bound=bind(fd,(sockaddr*)&addr,sizeof(addr))==0;
        umask(previous);
        if(bound){chmod(path.c_str(),0600);if(listen(fd,128)!=0)bound=false;}
    }
    if(bound){
        std::printf("TRONIX_READY %s\n",path.c_str());std::fflush(stdout);
        while(!stop_requested){
            // Finite wait: exit on SIGTERM even if signal() uses SA_RESTART.
            pollfd pfd{fd,POLLIN,0};
            int events=poll(&pfd,1,100);
            if(events==0 || (events<0 && errno==EINTR))continue;
            if(events<0 || (pfd.revents&(POLLERR|POLLHUP|POLLNVAL)))break;
            if(!(pfd.revents&POLLIN))continue;
            int c=accept4(fd,nullptr,nullptr,SOCK_CLOEXEC);
            if(c<0){if(errno==EINTR)continue;break;}
            timeval timeout{2,0};
            setsockopt(c,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
            setsockopt(c,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
            handle(c,ctx,&rt);close(c);
        }
    }else std::fprintf(stderr,"private socket bind failed\n");
    {
        std::lock_guard<std::mutex> guard(rt.wake_mutex);
        rt.running.store(false,std::memory_order_release);
    }
    rt.wake.notify_all();
    work.join();
    if(fd>=0)close(fd);
    if(bound)unlink(path.c_str());
    celix_frameworkFactory_destroyFramework(framework);
    std::filesystem::remove_all(cache);
    return bound?0:4;
}

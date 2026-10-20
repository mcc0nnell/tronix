// One-shot supervised worker execution (Linux/POSIX).
// Never fork() a live Celix daemon: use posix_spawn() and a fresh process.
#pragma once
#include <msgpack.hpp>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;

namespace tronix {
struct WorkerOutcome {
    bool ok=false;
    bool timed_out=false;
    int64_t value=0;
};
inline WorkerOutcome supervise(const std::string& binary,
                               const std::string& adapter,
                               const std::string& operation,
                               uint32_t budget_ms,
                               const std::atomic<bool>& running) {
    WorkerOutcome result;
    char cache_name[]="/tmp/tronix-worker-XXXXXX";
    if(!mkdtemp(cache_name))return result;
    const std::string cache=cache_name;
    const auto cleanup=[&](){
        std::error_code ec;
        std::filesystem::remove_all(cache,ec);
    };
    int fds[2];
    if(pipe2(fds,O_CLOEXEC|O_NONBLOCK)!=0){cleanup();return result;}
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // Close the old fd 3 *before* dup2 when the pipe read end is fd 3.
    posix_spawn_file_actions_addclose(&actions,fds[0]);
    posix_spawn_file_actions_adddup2(&actions,fds[1],3);
    if(fds[1]!=3)posix_spawn_file_actions_addclose(&actions,fds[1]);
    posix_spawnattr_t attrs;
    posix_spawnattr_init(&attrs);
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&attrs,POSIX_SPAWN_SETSID);
#endif
    char* const args[]{
        const_cast<char*>(binary.c_str()),
        const_cast<char*>("--adapter"),
        const_cast<char*>(adapter.c_str()),
        const_cast<char*>("--operation"),
        const_cast<char*>(operation.c_str()),
        const_cast<char*>("--cache"),
        const_cast<char*>(cache.c_str()),
        nullptr
    };
    pid_t pid=-1;
    const int err=posix_spawn(&pid,binary.c_str(),&actions,&attrs,args,environ);
    posix_spawnattr_destroy(&attrs);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if(err!=0){close(fds[0]);cleanup();return result;}
    // Startup gets up to 200ms beyond the trusted operation budget;
    // execution budget accounting still measures the complete elapsed time.
    const auto start=std::chrono::steady_clock::now();
    const auto end=start+std::chrono::milliseconds(budget_ms+200);
    std::string data;
    bool eof=false,exited=false,waited=false;
    int status=0;
    for(;;){
        if(!exited) {
            pid_t w=waitpid(pid,&status,WNOHANG);
            if(w==pid){exited=true;waited=true;}
            else if(w<0 && errno!=EINTR)break;
        }
        for(;;){
            char buf[256];
            ssize_t n=read(fds[0],buf,sizeof(buf));
            if(n>0){
                data.append(buf,static_cast<size_t>(n));
                if(data.size()>256)break;
            } else if(n==0){eof=true;break;}
            else if(errno==EINTR)continue;
            else if(errno==EAGAIN || errno==EWOULDBLOCK)break;
            else{eof=true;break;}
        }
        if(data.size()>256)break;
        if(exited && eof)break;
        if(!running.load(std::memory_order_acquire) ||
           std::chrono::steady_clock::now()>=end){
            result.timed_out=true;
            break;
        }
        auto remain=std::chrono::duration_cast<std::chrono::milliseconds>(
            end-std::chrono::steady_clock::now()).count();
        pollfd pfd{fds[0],POLLIN|POLLHUP,0};
        (void)poll(&pfd,1,static_cast<int>(std::max<int64_t>(1,std::min<int64_t>(10,remain))));
    }
    close(fds[0]);
    if(!exited){
#ifdef POSIX_SPAWN_SETSID
        // The new session owns the worker's descendants too.
        (void)kill(-pid,SIGKILL);
#endif
        (void)kill(pid,SIGKILL);
        pid_t w;
        do {w=waitpid(pid,&status,0);}while(w<0 && errno==EINTR);
        if(w==pid)waited=true;
    }
    cleanup();
    if(!waited || result.timed_out || !WIFEXITED(status) || WEXITSTATUS(status)!=0 ||
       data.empty() || data.size()>256)return result;
    try {
        auto handle=msgpack::unpack(data.data(),data.size());
        const auto& root=handle.get();
        if(root.type!=msgpack::type::MAP || root.via.map.size!=3)return result;
        bool seen_v=false,seen_ok=false,seen_value=false;
        for(uint32_t i=0;i<root.via.map.size;i++){
            const auto& item=root.via.map.ptr[i];
            auto key=item.key.as<std::string>();
            if(key=="v"&&!seen_v){seen_v=true;if(item.val.as<int>()!=1)return result;}
            else if(key=="ok"&&!seen_ok){seen_ok=true;result.ok=item.val.as<bool>();}
            else if(key=="value"&&!seen_value){seen_value=true;result.value=item.val.as<int64_t>();}
            else return WorkerOutcome{};
        }
        if(!seen_v||!seen_ok||!seen_value)return WorkerOutcome{};
    } catch(...) {return WorkerOutcome{};}
    return result;
}
} // namespace tronix

// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
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
#include <vector>
#include <set>
#include <sys/wait.h>
#include <unistd.h>

namespace tronix {
struct OperationPolicy {
    std::string name;
    int lane=-1;
    uint32_t budget_ms=0;
};
struct WorkerOutcome {
    bool ok=false;
    bool timed_out=false;
    int64_t value=0;
    std::vector<OperationPolicy> policies;
};
inline WorkerOutcome supervise(const std::string& binary,
                               const std::string& adapter,
                               const std::string& operation,
                               uint32_t budget_ms,
                               const std::atomic<bool>& running,
                               bool inspect=false,
                               int expected_lane=-1) {
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
    if(posix_spawn_file_actions_init(&actions)!=0){
        close(fds[0]);close(fds[1]);cleanup();return result;
    }
    // Only fd 0..2 and the result pipe on fd 3 reach the worker.
    // The close-from glibc action rejects accidentally inherited daemon FDs.
    bool actions_ok =
        posix_spawn_file_actions_addclose(&actions,fds[0])==0 &&
        posix_spawn_file_actions_adddup2(&actions,fds[1],3)==0;
    if(fds[1]!=3) actions_ok=actions_ok &&
        posix_spawn_file_actions_addclose(&actions,fds[1])==0;
    actions_ok=actions_ok &&
        posix_spawn_file_actions_addclosefrom_np(&actions,4)==0 &&
        posix_spawn_file_actions_addopen(&actions,0,"/dev/null",O_RDONLY,0)==0 &&
        posix_spawn_file_actions_addopen(&actions,1,"/dev/null",O_WRONLY,0)==0;
    posix_spawnattr_t attrs;
    bool attrs_ok=posix_spawnattr_init(&attrs)==0;
#ifdef POSIX_SPAWN_SETSID
    if(attrs_ok) attrs_ok=posix_spawnattr_setflags(&attrs,POSIX_SPAWN_SETSID)==0;
#else
    attrs_ok=false;
#endif
    if(!actions_ok || !attrs_ok){
        if(attrs_ok) posix_spawnattr_destroy(&attrs);
        posix_spawn_file_actions_destroy(&actions);
        close(fds[0]);close(fds[1]);cleanup();
        return result;
    }
    const std::string lane_arg=std::to_string(expected_lane);
    const std::string budget_arg=std::to_string(budget_ms);
    char* const execute_args[]{
        const_cast<char*>(binary.c_str()),
        const_cast<char*>("--adapter"),
        const_cast<char*>(adapter.c_str()),
        const_cast<char*>("--operation"),
        const_cast<char*>(operation.c_str()),
        const_cast<char*>("--cache"),
        const_cast<char*>(cache.c_str()),
        const_cast<char*>("--lane"),
        const_cast<char*>(lane_arg.c_str()),
        const_cast<char*>("--budget-ms"),
        const_cast<char*>(budget_arg.c_str()),
        nullptr
    };
    char* const inspect_args[]{
        const_cast<char*>(binary.c_str()),
        const_cast<char*>("--adapter"),
        const_cast<char*>(adapter.c_str()),
        const_cast<char*>("--cache"),
        const_cast<char*>(cache.c_str()),
        const_cast<char*>("--inspect"),
        nullptr
    };
    // Credentials and inherited environment are not forwarded.
    // Two fixture-only paths are passed explicitly for regression tests,
    // and never unless an operator opted into the synthetic fixture mode.
    const bool fixture=getenv("TRONIX_ISOLATION_FIXTURES") &&
        std::strcmp(getenv("TRONIX_ISOLATION_FIXTURES"),"1")==0;
    const auto fixture_path=[&](const char* name)->std::string {
        const char* value=fixture?getenv(name):nullptr;
        if(!value || value[0]!='/' || std::strlen(value)>512)return "";
        return std::string(name)+"="+value;
    };
    std::string marker=fixture_path("TRONIX_ACTIVATOR_MARKER");
    std::string drift=fixture_path("TRONIX_POLICY_DRIFT_FILE");
    char path_env[]="PATH=/usr/bin:/bin";
    char lang_env[]="LANG=C";
    char locale_env[]="LC_ALL=C";
    char fixture_env[]="TRONIX_ISOLATION_FIXTURES=1";
    char* envp[7]{};
    size_t e=0;
    envp[e++]=path_env;envp[e++]=lang_env;envp[e++]=locale_env;
    if(fixture)envp[e++]=fixture_env;
    if(!marker.empty())envp[e++]=marker.data();
    if(!drift.empty())envp[e++]=drift.data();
    envp[e]=nullptr;
    pid_t pid=-1;
    const int err=posix_spawn(&pid,binary.c_str(),&actions,&attrs,
                              inspect?inspect_args:execute_args,envp);
    posix_spawnattr_destroy(&attrs);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if(err!=0){close(fds[0]);cleanup();return result;}
    // Startup gets up to 200ms beyond the trusted operation budget;
    // execution budget accounting still measures the complete elapsed time.
    const auto start=std::chrono::steady_clock::now();
    const auto end=start+std::chrono::milliseconds(inspect?3000:budget_ms+200);
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
                if(data.size()>(inspect?2048u:256u))break;
            } else if(n==0){eof=true;break;}
            else if(errno==EINTR)continue;
            else if(errno==EAGAIN || errno==EWOULDBLOCK)break;
            else{eof=true;break;}
        }
        if(data.size()>(inspect?2048u:256u))break;
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
       data.empty() || data.size()>(inspect?2048u:256u))return result;
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
            else if(key=="value"&&!seen_value&&!inspect){
                seen_value=true;result.value=item.val.as<int64_t>();
            }else if(key=="operations"&&!seen_value&&inspect){
                seen_value=true;
                if(item.val.type!=msgpack::type::ARRAY ||
                   item.val.via.array.size==0 || item.val.via.array.size>16)return WorkerOutcome{};
                std::set<std::string> unique;
                for(uint32_t j=0;j<item.val.via.array.size;j++){
                    const auto& row=item.val.via.array.ptr[j];
                    if(row.type!=msgpack::type::MAP||row.via.map.size!=3)return WorkerOutcome{};
                    OperationPolicy policy;
                    unsigned seen=0;
                    for(uint32_t k=0;k<row.via.map.size;k++){
                        const auto& field=row.via.map.ptr[k];
                        auto name=field.key.as<std::string>();
                        if(name=="name"&&!(seen&1)){policy.name=field.val.as<std::string>();seen|=1;}
                        else if(name=="lane"&&!(seen&2)){policy.lane=field.val.as<int>();seen|=2;}
                        else if(name=="budgetMs"&&!(seen&4)){policy.budget_ms=field.val.as<uint32_t>();seen|=4;}
                        else return WorkerOutcome{};
                    }
                    if(seen!=7||policy.name.empty()||policy.name.size()>48||
                       (policy.lane!=0&&policy.lane!=1)||
                       policy.budget_ms==0||policy.budget_ms>30000||
                       !unique.insert(policy.name).second)return WorkerOutcome{};
                    for(unsigned char c:policy.name)if(c<33||c>126||c==',')return WorkerOutcome{};
                    result.policies.push_back(policy);
                }
            }else return WorkerOutcome{};
        }
        if(!seen_v||!seen_ok||!seen_value||!result.ok)return WorkerOutcome{};
    } catch(...) {return WorkerOutcome{};}
    return result;
}
inline WorkerOutcome inspect_policy(const std::string& binary,
                                    const std::string& adapter,
                                    const std::atomic<bool>& running){
    return supervise(binary,adapter,"",0,running,true);
}
} // namespace tronix

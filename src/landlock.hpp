// Landlock filesystem + TCP policy v1 for reviewed Celix adapters.
// No fallback: if the kernel cannot enforce all requested rights, deny startup.
// Applied inside the subprocess after exec(), before loading any adapter code.
#pragma once
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <limits.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/landlock.h>
#endif

namespace tronix {
inline bool install_worker_landlock(const std::string& adapter,
                                    const std::string& cache) {
#if !defined(__linux__) || !defined(SYS_landlock_create_ruleset) || \
    !defined(SYS_landlock_add_rule) || !defined(SYS_landlock_restrict_self)
    (void)adapter;(void)cache;
    return false;
#else
    const int abi=static_cast<int>(::syscall(SYS_landlock_create_ruleset,
                                           nullptr,0,LANDLOCK_CREATE_RULESET_VERSION));
    // Version 4 is essential to enforce TCP connect and bind denial.
    if(abi<4)return false;
    constexpr uint64_t read_rights=LANDLOCK_ACCESS_FS_EXECUTE |
        LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
    constexpr uint64_t write_rights=LANDLOCK_ACCESS_FS_WRITE_FILE |
        LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
        LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
        LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
        LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
        LANDLOCK_ACCESS_FS_MAKE_SYM | LANDLOCK_ACCESS_FS_REFER |
        LANDLOCK_ACCESS_FS_TRUNCATE;
    landlock_ruleset_attr attr{};
    attr.handled_access_fs=read_rights|write_rights;
    attr.handled_access_net=LANDLOCK_ACCESS_NET_BIND_TCP|
                            LANDLOCK_ACCESS_NET_CONNECT_TCP;
    int rules=static_cast<int>(::syscall(SYS_landlock_create_ruleset,
                                       &attr,sizeof(attr),0));
    if(rules<0)return false;
    bool success=true;
    const auto add=[&](const std::string& path,uint64_t allowed)->bool{
        char resolved[PATH_MAX];
        if(!::realpath(path.c_str(),resolved))return false;
        int fd=::open(resolved,O_PATH|O_CLOEXEC);
        if(fd<0)return false;
        struct landlock_path_beneath_attr rule{};
        rule.allowed_access=allowed;
        rule.parent_fd=fd;
        const bool ok=(::syscall(SYS_landlock_add_rule,rules,
                                LANDLOCK_RULE_PATH_BENEATH,&rule,0)==0);
        ::close(fd);
        return ok;
    };
    // Only host runtime objects needed by a dynamically linked Celix worker.
    // The bundle archive itself is readable but its parent directory is not.
    // The private cache subtree alone is writable/executable.
    if(!add("/usr",read_rights))success=false;
    if(success && !add("/etc/ld.so.cache",LANDLOCK_ACCESS_FS_READ_FILE))success=false;
    if(success && !add(adapter,LANDLOCK_ACCESS_FS_READ_FILE))success=false;
    if(success && !add(cache,read_rights|write_rights))success=false;
    if(success && ::prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)!=0)success=false;
    if(success && ::syscall(SYS_landlock_restrict_self,rules,0)!=0)success=false;
    ::close(rules);
    return success;
#endif
}
} // namespace tronix

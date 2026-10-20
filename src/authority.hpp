// Optional Linux peer-UID authority for private Tronix AF_UNIX socket.
// Binds each authorized UID to exactly one agent label and an allowlist.
// This is NOT same-UID process isolation or multi-user socket provisioning.
#pragma once
#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace tronix {
struct Identity {
    std::string agent;
    std::set<std::string> operations;
    bool allows(const std::string& op) const {
        return operations.count(op)!=0;
    }
};

class Authority {
    std::map<uid_t,Identity> by_uid_;
    static bool valid_name(const std::string& name,size_t limit,bool comma=false){
        if(name.empty()||name.size()>limit)return false;
        for(unsigned char c:name) {
            if(c<33||c>126||(!comma&&c==','))return false;
        }
        return true;
    }
public:
    explicit Authority(const std::string& path) {
        if(path.empty()||path[0]!='/')throw std::runtime_error("authority path must be absolute");
        int fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if(fd<0)throw std::runtime_error("cannot open authority file");
        std::string source;
        try {
            struct stat st{};
            if(::fstat(fd,&st)!=0||!S_ISREG(st.st_mode)||st.st_uid!=::geteuid()||
               (st.st_mode&077)!=0||st.st_nlink!=1||st.st_size<1||st.st_size>8192)
                throw std::runtime_error("authority file must be an owned private regular file <=8192 bytes");
            char buf[1024];
            while(true){
                ssize_t n=::read(fd,buf,sizeof(buf));
                if(n<0&&errno==EINTR)continue;
                if(n<0)throw std::runtime_error("cannot read authority file");
                if(n==0)break;
                source.append(buf,static_cast<size_t>(n));
                if(source.size()>8192)throw std::runtime_error("authority file too large");
            }
            ::close(fd);
        }catch(...){::close(fd);throw;}
        if(source.empty()||source.find('\0')!=std::string::npos)
            throw std::runtime_error("empty or invalid authority file");
        std::istringstream lines(source);
        std::string line;
        std::set<std::string> agents;
        while(std::getline(lines,line)){
            if(line.empty()||line[0]=='#')continue;
            std::istringstream fields(line);
            std::string uid_text,agent,allowlist,unexpected;
            if(!(fields>>uid_text>>agent>>allowlist) || (fields>>unexpected))
                throw std::runtime_error("authority line must be: UID agent op1,op2");
            if(uid_text.empty()||uid_text.size()>10)
                throw std::runtime_error("invalid authority uid");
            uint64_t uid=0;
            for(char c:uid_text){
                if(c<'0'||c>'9')throw std::runtime_error("invalid authority uid");
                uid=uid*10+static_cast<unsigned>(c-'0');
                if(uid>std::numeric_limits<uid_t>::max())
                    throw std::runtime_error("authority uid overflow");
            }
            if(!valid_name(agent,64)||!agents.insert(agent).second)
                throw std::runtime_error("invalid or duplicate authority agent");
            Identity entry;entry.agent=agent;
            size_t start=0;
            while(start<allowlist.size()){
                auto stop=allowlist.find(',',start);
                const std::string op=allowlist.substr(start,
                    stop==std::string::npos?std::string::npos:stop-start);
                if(!valid_name(op,48)||!entry.operations.insert(op).second||
                   entry.operations.size()>16)
                    throw std::runtime_error("invalid or duplicate authority operation");
                if(stop==std::string::npos)break;
                start=stop+1;
                if(start==allowlist.size())
                    throw std::runtime_error("authority trailing comma");
            }
            if(entry.operations.empty()||!by_uid_.emplace(static_cast<uid_t>(uid),entry).second ||
               by_uid_.size()>32)
                throw std::runtime_error("invalid or duplicate authority uid");
        }
        if(by_uid_.empty())throw std::runtime_error("no authorized uid entries");
    }
    const Identity* lookup(uid_t uid)const{
        auto iter=by_uid_.find(uid);
        return iter==by_uid_.end()?nullptr:&iter->second;
    }
};
} // namespace tronix

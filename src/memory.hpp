// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Native, opt-in per-agent key/value memory.
// Scope is only the daemon's current Unix UID; agent strings are NOT identities.
// This is a bounded append-only store, not an LLM or semantic memory index.
#pragma once
#include <msgpack.hpp>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <arpa/inet.h>

namespace tronix {
class AgentMemory {
    using Key=std::pair<std::string,std::string>;
    mutable std::mutex mutex_;
    std::map<Key,std::string> values_;
    int fd_=-1;
    bool healthy_=true;
    uint64_t bytes_=0;
    static constexpr size_t max_keys_=512;
    static constexpr uint64_t max_bytes_=4ULL*1024*1024;
    static constexpr uint32_t max_record_=1024;
    static bool write_all(int fd,const char* buf,size_t n) {
        while(n){
            ssize_t w=::write(fd,buf,n);
            if(w<0&&errno==EINTR)continue;
            if(w<=0)return false;
            buf+=w;n-=static_cast<size_t>(w);
        }
        return true;
    }
    static bool printable(const std::string& s,size_t max,bool spaces=false){
        if(s.empty()||s.size()>max)return false;
        for(unsigned char c:s)if(c<(spaces?32:33)||c>126)return false;
        return true;
    }
    static std::string encode(const std::string& op,const std::string& agent,
                              const std::string& key,const std::string& value) {
        msgpack::sbuffer b;
        msgpack::packer<msgpack::sbuffer> pk(b);
        pk.pack_map(4);
        pk.pack(std::string("action"));pk.pack(op);
        pk.pack(std::string("agent"));pk.pack(agent);
        pk.pack(std::string("key"));pk.pack(key);
        pk.pack(std::string("value"));pk.pack(value);
        return std::string(b.data(),b.size());
    }
    void replay(const std::string& s) {
        const auto handle=msgpack::unpack(s.data(),s.size());
        const auto& root=handle.get();
        if(root.type!=msgpack::type::MAP || root.via.map.size!=4)
            throw std::runtime_error("bad memory record");
        std::string action,agent,key,value;
        unsigned mask=0;
        for(uint32_t i=0;i<root.via.map.size;i++){
            const auto& kv=root.via.map.ptr[i];
            const auto name=kv.key.as<std::string>();
            if(name=="action"&&!(mask&1)){action=kv.val.as<std::string>();mask|=1;}
            else if(name=="agent"&&!(mask&2)){agent=kv.val.as<std::string>();mask|=2;}
            else if(name=="key"&&!(mask&4)){key=kv.val.as<std::string>();mask|=4;}
            else if(name=="value"&&!(mask&8)){value=kv.val.as<std::string>();mask|=8;}
            else throw std::runtime_error("invalid memory fields");
        }
        if(mask!=15 || !printable(agent,64) || !printable(key,64) ||
           (action!="put" && action!="delete") ||
           (action=="put"&&!printable(value,512,true)) ||
           (action=="delete"&&!value.empty()))
            throw std::runtime_error("invalid memory entry");
        if(action=="delete")values_.erase({agent,key});
        else{
            if(values_.find({agent,key})==values_.end()&&values_.size()==max_keys_)
                throw std::runtime_error("too many memory keys");
            values_[{agent,key}]=value;
        }
    }
    void load(){
        off_t good=0;
        for(;;){
            uint32_t net=0;
            ssize_t n=::read(fd_,&net,4);
            if(n==0)break;
            if(n<0&&errno==EINTR)continue;
            if(n!=4){if(::ftruncate(fd_,good)!=0)throw std::runtime_error("memory truncate failed");break;}
            uint32_t len=ntohl(net);
            if(!len || len>max_record_)throw std::runtime_error("bad memory frame");
            std::string payload(len,static_cast<char>(0));
            size_t pos=0;
            while(pos<len){
                n=::read(fd_,payload.data()+pos,len-pos);
                if(n<0&&errno==EINTR)continue;
                if(n<=0)break;
                pos+=static_cast<size_t>(n);
            }
            if(pos!=len){if(::ftruncate(fd_,good)!=0)throw std::runtime_error("memory truncate failed");break;}
            replay(payload);
            good+=4+len;
        }
        const auto end=::lseek(fd_,0,SEEK_END);
        if(end<0||static_cast<uint64_t>(end)>max_bytes_)
            throw std::runtime_error("memory journal limit exceeded");
        bytes_=static_cast<uint64_t>(end);
    }
    bool append(const std::string& op,const std::string& agent,
                const std::string& key,const std::string& value){
        auto content=encode(op,agent,key,value);
        if(content.size()>max_record_||bytes_+content.size()+4>max_bytes_)
            return false;
        uint32_t n=htonl(static_cast<uint32_t>(content.size()));
        if(!write_all(fd_,reinterpret_cast<const char*>(&n),4) ||
           !write_all(fd_,content.data(),content.size()) || ::fdatasync(fd_)!=0){
            healthy_=false;return false;
        }
        bytes_+=4+content.size();return true;
    }
public:
    explicit AgentMemory(const std::string& path){
        if(path.empty()||path[0]!='/')throw std::runtime_error("memory path must be absolute");
        fd_=::open(path.c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd_<0)throw std::runtime_error("cannot open memory file");
        try{
            struct stat st{};
            if(::fstat(fd_,&st)!=0||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||
               (st.st_mode&077)!=0||st.st_nlink!=1)
                throw std::runtime_error("memory file must be private and owned");
            if(::flock(fd_,LOCK_EX|LOCK_NB)!=0)
                throw std::runtime_error("memory file already held by another writer");
            load();
        }catch(...){::close(fd_);fd_=-1;throw;}
    }
    ~AgentMemory(){if(fd_>=0)::close(fd_);}
    AgentMemory(const AgentMemory&)=delete;
    AgentMemory& operator=(const AgentMemory&)=delete;
    bool put(const std::string& agent,const std::string& key,const std::string& value){
        if(!printable(agent,64)||!printable(key,64)||!printable(value,512,true))return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if(!healthy_||(values_.find({agent,key})==values_.end()&&values_.size()>=max_keys_))
            return false;
        if(!append("put",agent,key,value))return false;
        values_[{agent,key}]=value;return true;
    }
    bool erase(const std::string& agent,const std::string& key){
        std::lock_guard<std::mutex> lock(mutex_);
        auto it=values_.find({agent,key});
        if(it==values_.end()||!healthy_)return false;
        if(!append("delete",agent,key,""))return false;
        values_.erase(it);return true;
    }
    bool get(const std::string& agent,const std::string& key,std::string& value)const{
        std::lock_guard<std::mutex> lock(mutex_);
        auto it=values_.find({agent,key});
        if(it==values_.end())return false;
        value=it->second;return true;
    }
    std::vector<std::string> keys(const std::string& agent,
                                  const std::string& after,size_t limit=16)const{
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> result;
        auto it=values_.upper_bound({agent,after});
        for(;it!=values_.end()&&it->first.first==agent&&result.size()<limit;++it)
            result.push_back(it->first.second);
        return result;
    }
};
}

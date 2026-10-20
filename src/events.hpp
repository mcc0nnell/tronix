// TRONIX experimental lifecycle ledger. No agent input or code execution here.
// Native, bounded in-memory event fanout plus optional append-only durable record.
// Journal records are length-prefixed MessagePack maps. A torn final write is
// discarded at startup; an invalid complete record fails startup.
#pragma once
#include <msgpack.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <arpa/inet.h>

namespace tronix {
struct Event {
    uint64_t seq=0, time_ms=0, dispatch=0;
    std::string id, agent, operation, state;
    bool has_value=false;
    int64_t value=0;
};
class Ledger {
    mutable std::mutex mutex_;
    std::deque<Event> recent_;
    std::unordered_map<std::string,Event> latest_;
    std::unordered_map<std::string,Event> pending_;
    uint64_t sequence_=0;
    int fd_=-1;
    bool healthy_=true;
    uint64_t bytes_=0;
    static constexpr uint64_t max_bytes_=16ULL*1024*1024;
    static constexpr uint64_t admission_reserve_=1024*1024;
    static constexpr size_t capacity_=512;
    static constexpr uint32_t max_record_=2048;

    static uint64_t millis() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }
    static std::string serialize(const Event& e) {
        msgpack::sbuffer b;
        msgpack::packer<msgpack::sbuffer> pk(b);
        pk.pack_map(8);
        pk.pack(std::string("seq")); pk.pack(e.seq);
        pk.pack(std::string("timeMs")); pk.pack(e.time_ms);
        pk.pack(std::string("id")); pk.pack(e.id);
        pk.pack(std::string("agent")); pk.pack(e.agent);
        pk.pack(std::string("operation")); pk.pack(e.operation);
        pk.pack(std::string("state")); pk.pack(e.state);
        pk.pack(std::string("dispatchSequence")); pk.pack(e.dispatch);
        pk.pack(std::string("value"));
        if (e.has_value) pk.pack(e.value); else pk.pack_nil();
        return std::string(b.data(),b.size());
    }
    static Event decode(const char* p,size_t n) {
        const auto handle=msgpack::unpack(p,n);
        const auto& obj=handle.get();
        if(obj.type!=msgpack::type::MAP || obj.via.map.size!=8)
            throw std::runtime_error("invalid journal record");
        Event e; unsigned mask=0;
        for(uint32_t i=0;i<obj.via.map.size;i++){
            const auto& kv=obj.via.map.ptr[i];
            const std::string key=kv.key.as<std::string>();
            if(key=="seq"){e.seq=kv.val.as<uint64_t>();mask|=1;}
            else if(key=="timeMs"){e.time_ms=kv.val.as<uint64_t>();mask|=2;}
            else if(key=="id"){e.id=kv.val.as<std::string>();mask|=4;}
            else if(key=="agent"){e.agent=kv.val.as<std::string>();mask|=8;}
            else if(key=="operation"){e.operation=kv.val.as<std::string>();mask|=16;}
            else if(key=="state"){e.state=kv.val.as<std::string>();mask|=32;}
            else if(key=="dispatchSequence"){e.dispatch=kv.val.as<uint64_t>();mask|=64;}
            else if(key=="value"){
                if(kv.val.type!=msgpack::type::NIL){e.value=kv.val.as<int64_t>();e.has_value=true;}
                mask|=128;
            }else throw std::runtime_error("unexpected journal key");
        }
        if(mask!=255 || e.seq==0 || e.id.empty() || e.agent.empty() ||
           e.operation.empty() || e.state.empty()) throw std::runtime_error("invalid journal fields");
        return e;
    }
    void remember(const Event& e) {
        sequence_=e.seq;
        recent_.push_back(e);
        if(recent_.size()>capacity_) recent_.pop_front();
        latest_[e.id]=e;
        if(e.state=="queued" || e.state=="running")pending_[e.id]=e;
        else pending_.erase(e.id);
    }
    static bool write_all(int fd,const char* data,size_t n) {
        while(n) {
            ssize_t k=::write(fd,data,n);
            if(k<0 && errno==EINTR)continue;
            if(k<=0)return false;
            data+=k;n-=static_cast<size_t>(k);
        }
        return true;
    }
    void load() {
        off_t good=0;
        while(true) {
            uint32_t net=0;
            ssize_t n=::read(fd_,&net,4);
            if(n==0)break;
            if(n<0 && errno==EINTR)continue;
            if(n!=4) { if(::ftruncate(fd_,good)!=0)throw std::runtime_error("journal tail truncate failed");break; }
            uint32_t len=ntohl(net);
            if(len==0||len>max_record_)throw std::runtime_error("invalid journal frame size");
            std::string payload(len,static_cast<char>(0));
            size_t pos=0;
            while(pos<len) {
                n=::read(fd_,payload.data()+pos,len-pos);
                if(n<0 && errno==EINTR)continue;
                if(n<=0)break;
                pos+=static_cast<size_t>(n);
            }
            if(pos!=len){if(::ftruncate(fd_,good)!=0)throw std::runtime_error("journal tail truncate failed");break;}
            Event e=decode(payload.data(),payload.size());
            if(e.seq!=sequence_+1)throw std::runtime_error("journal sequence discontinuity");
            remember(e);
            good+=4+len;
        }
        auto end=::lseek(fd_,0,SEEK_END);
        if(end<0)throw std::runtime_error("journal seek failed");
        bytes_=static_cast<uint64_t>(end);
        if(bytes_>max_bytes_)throw std::runtime_error("journal file exceeds 16 MiB cap");
    }
public:
    explicit Ledger(const std::string& path) {
        if(path.empty())return;
        if(path[0]!='/')throw std::runtime_error("journal path must be absolute");
        fd_=::open(path.c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd_<0)throw std::runtime_error("cannot open journal");
        try {
            struct stat st{};
            if(::fstat(fd_,&st)!=0 || !S_ISREG(st.st_mode) ||
               st.st_uid!=::getuid() || (st.st_mode&077)!=0 || st.st_nlink!=1)
                throw std::runtime_error("journal must be a private regular owned file");
            if(::flock(fd_,LOCK_EX|LOCK_NB)!=0)
                throw std::runtime_error("journal already held by another writer");
            load();
            std::vector<Event> interrupted;
            for(const auto& entry:latest_) {
                if(entry.second.state=="queued"||entry.second.state=="running")
                    interrupted.push_back(entry.second);
            }
            std::sort(interrupted.begin(),interrupted.end(),
                      [](const Event& a,const Event& b){return a.seq<b.seq;});
            for(const auto& e:interrupted)
                if(!emit(e.id,e.agent,e.operation,"interrupted",e.dispatch))
                    throw std::runtime_error("cannot write interruption record");
        } catch(...) {::close(fd_);fd_=-1;throw;}
    }
    ~Ledger(){if(fd_>=0)::close(fd_);}
    bool durable()const{return fd_>=0;}
    bool healthy()const{
        std::lock_guard<std::mutex> l(mutex_);
        return healthy_;
    }
    bool can_admit()const{
        std::lock_guard<std::mutex> l(mutex_);
        return healthy_ && (fd_<0 || bytes_<max_bytes_-admission_reserve_);
    }
    uint64_t last()const{std::lock_guard<std::mutex> l(mutex_);return sequence_;}
    bool known(const std::string& id)const {
        std::lock_guard<std::mutex> l(mutex_);
        return latest_.find(id)!=latest_.end();
    }
    bool get(const std::string& id,Event& out)const {
        std::lock_guard<std::mutex> l(mutex_);
        auto it=latest_.find(id);
        if(it==latest_.end())return false;
        out=it->second;return true;
    }
    std::vector<Event> since(uint64_t after,size_t limit)const {
        std::lock_guard<std::mutex> l(mutex_);
        std::vector<Event> out;
        for(const auto& e:recent_) {
            if(e.seq>after){out.push_back(e);if(out.size()==limit)break;}
        }
        return out;
    }
    std::vector<Event> pending()const {
        std::lock_guard<std::mutex> l(mutex_);
        std::vector<Event> out;
        out.reserve(pending_.size());
        for(const auto& entry:pending_)out.push_back(entry.second);
        return out;
    }
    uint64_t oldest()const{
        std::lock_guard<std::mutex> l(mutex_);
        return recent_.empty()?0:recent_.front().seq;
    }
    bool emit(const std::string& id,const std::string& agent,const std::string& op,
              const std::string& state,uint64_t dispatch=0,bool has_value=false,int64_t value=0){
        std::lock_guard<std::mutex> l(mutex_);
        if(!healthy_)return false;
        auto prev=latest_.find(id);
        if(prev!=latest_.end() && prev->second.state==state)return true;
        Event e;
        e.seq=sequence_+1;e.time_ms=millis();e.id=id;e.agent=agent;
        e.operation=op;e.state=state;e.dispatch=dispatch;
        e.has_value=has_value;e.value=value;
        if(state=="acked"&&prev!=latest_.end()){
            e.dispatch=prev->second.dispatch;
            e.has_value=prev->second.has_value;e.value=prev->second.value;
        }
        if(fd_>=0){
            const std::string data=serialize(e);
            if(data.size()>max_record_ || bytes_+data.size()+4>max_bytes_){
                healthy_=false;return false;
            }
            uint32_t wire=htonl(static_cast<uint32_t>(data.size()));
            if(!write_all(fd_,reinterpret_cast<const char*>(&wire),4) ||
               !write_all(fd_,data.data(),data.size()) || ::fdatasync(fd_)!=0){
                healthy_=false;return false;
            }
            bytes_+=4+data.size();
        }
        remember(e);return true;
    }
};
} // namespace tronix

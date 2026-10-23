// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Optional owner-private HMAC-chained terminal receipts. No replay/rollback claim.
#pragma once
#include <openssl/hmac.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <string>
#include <utility>
#include <stdexcept>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tronix {
class GrantEvidence {
    int fd_=-1;
    std::string key_, prev_=std::string(64,'0'),artifact_,grant_;
    uint64_t seq_=0;
    std::atomic<bool> healthy_{true};
    static std::string hex(const unsigned char* p,size_t size) {
        static constexpr char chars[]="0123456789abcdef";
        std::string s;
        s.reserve(size*2);
        for(size_t i=0;i<size;i++){s.push_back(chars[p[i]>>4]);s.push_back(chars[p[i]&15]);}
        return s;
    }
    static std::string hex(const std::string& s) {
        return hex(reinterpret_cast<const unsigned char*>(s.data()),s.size());
    }
    static bool write_all(int fd,const std::string& s) {
        size_t at=0;
        while(at<s.size()){
            ssize_t n=write(fd,s.data()+at,s.size()-at);
            if(n<0&&errno==EINTR)continue;
            if(n<=0)return false;
            at+=static_cast<size_t>(n);
        }
        return true;
    }
public:
    GrantEvidence(const std::string& path,const std::string& key_path,
                  std::string artifact,std::string grant)
        :artifact_(std::move(artifact)),grant_(std::move(grant)) {
        if(path.empty()||path[0]!='/'||key_path.empty()||key_path[0]!='/')
            throw std::runtime_error("evidence and key paths must be absolute");
        int key_fd=open(key_path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if(key_fd<0)throw std::runtime_error("cannot open evidence key");
        try {
            struct stat st{};
            if(fstat(key_fd,&st)!=0 || !S_ISREG(st.st_mode) ||
               st.st_uid!=getuid() || (st.st_mode&077)!=0 ||
               st.st_nlink!=1 || st.st_size!=32)
                throw std::runtime_error("evidence key requires private 32-byte regular file");
            key_.resize(32);
            size_t at=0;
            while(at<32){
                ssize_t n=read(key_fd,key_.data()+at,32-at);
                if(n<0&&errno==EINTR)continue;
                if(n<=0)throw std::runtime_error("evidence key read incomplete");
                at+=static_cast<size_t>(n);
            }
        }catch(...){close(key_fd);throw;}
        close(key_fd);
        fd_=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd_<0)throw std::runtime_error("evidence file must be new and private");
    }
    GrantEvidence(const GrantEvidence&)=delete;
    GrantEvidence& operator=(const GrantEvidence&)=delete;
    ~GrantEvidence(){if(fd_>=0)close(fd_);}
    bool healthy()const{return healthy_;}
    bool append(const std::string& id,const std::string& agent,const std::string& op,
                const std::string& state,uint64_t dispatch,bool has_value,int64_t value) {
        if(!healthy_)return false;
        const auto seq=seq_+1;
        std::string data="v1 "+std::to_string(seq)+" "+hex(id)+" "+hex(agent)+" "+
           hex(op)+" "+state+" "+std::to_string(dispatch)+" "+
           (has_value?std::to_string(value):"-")+" "+artifact_+" "+grant_+" "+prev_;
        unsigned char result[EVP_MAX_MD_SIZE]{};
        unsigned int n=0;
        if(!HMAC(EVP_sha256(),key_.data(),static_cast<int>(key_.size()),
                 reinterpret_cast<const unsigned char*>(data.data()),data.size(),result,&n) ||
           n!=32){healthy_=false;return false;}
        const std::string tag=hex(result,n);
        if(!write_all(fd_,data+" "+tag+"\n")||fdatasync(fd_)!=0){
            healthy_=false;return false;
        }
        seq_=seq;prev_=tag;return true;
    }
};
} // namespace tronix

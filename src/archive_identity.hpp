// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Operator-pinned archive bytes. No Celix bundle activation before verification.
#pragma once
#include <openssl/evp.h>
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace tronix {
inline bool valid_sha256(const std::string& hex) {
    if(hex.size()!=64)return false;
    for(unsigned char c:hex)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
    return true;
}
inline std::string snapshot_adapter(const std::string& source,
                                    const std::string& private_cache,
                                    const std::string& expected_sha256) {
    if(expected_sha256.empty())return source; // Backward-compatible opt-in mode.
    if(!valid_sha256(expected_sha256))throw std::runtime_error("invalid adapter SHA-256");
    const int input=open(source.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(input<0)throw std::runtime_error("cannot open adapter without following symlinks");
    const std::string output_path=private_cache+"/tronix-verified-adapter.zip";
    int output=-1;
    EVP_MD_CTX* hash=nullptr;
    try {
        struct stat st{};
        if(fstat(input,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<=0 ||
           st.st_size>32*1024*1024)
            throw std::runtime_error("adapter archive must be a bounded regular file");
        output=open(output_path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
        if(output<0)throw std::runtime_error("cannot create private adapter snapshot");
        hash=EVP_MD_CTX_new();
        if(!hash || EVP_DigestInit_ex(hash,EVP_sha256(),nullptr)!=1)
            throw std::runtime_error("SHA-256 initialization failed");
        std::array<unsigned char,16384> buf{};
        size_t total=0;
        for(;;){
            ssize_t count=read(input,buf.data(),buf.size());
            if(count<0 && errno==EINTR)continue;
            if(count<0)throw std::runtime_error("cannot read adapter archive");
            if(count==0)break;
            total+=static_cast<size_t>(count);
            if(total>32*1024*1024)throw std::runtime_error("adapter archive grew past limit");
            if(EVP_DigestUpdate(hash,buf.data(),static_cast<size_t>(count))!=1)
                throw std::runtime_error("SHA-256 update failed");
            size_t offset=0;
            while(offset<static_cast<size_t>(count)){
                ssize_t wrote=write(output,buf.data()+offset,static_cast<size_t>(count)-offset);
                if(wrote<0 && errno==EINTR)continue;
                if(wrote<=0)throw std::runtime_error("cannot write private adapter snapshot");
                offset+=static_cast<size_t>(wrote);
            }
        }
        unsigned char digest[EVP_MAX_MD_SIZE]{};
        unsigned int n=0;
        if(total==0 || EVP_DigestFinal_ex(hash,digest,&n)!=1 || n!=32)
            throw std::runtime_error("SHA-256 finalization failed");
        static constexpr char hexdigits[]="0123456789abcdef";
        std::string actual;
        for(unsigned i=0;i<n;i++){
            actual.push_back(hexdigits[digest[i]>>4]);
            actual.push_back(hexdigits[digest[i]&15]);
        }
        if(actual!=expected_sha256)
            throw std::runtime_error("adapter archive SHA-256 does not match operator pin");
        if(fchmod(output,0400)!=0)
            throw std::runtime_error("cannot protect adapter snapshot");
        close(input);
        close(output);
        EVP_MD_CTX_free(hash);
        return output_path;
    }catch(...){
        if(hash)EVP_MD_CTX_free(hash);
        close(input);
        if(output>=0){close(output);unlink(output_path.c_str());}
        throw;
    }
}
} // namespace tronix

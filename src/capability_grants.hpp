// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Frozen, owner-private, exact-archive Celix capability grants (v1).
#pragma once
#include "archive_identity.hpp"
#include "tronix_service.h"
#include <jansson.h>
#include <zip.h>
#include <openssl/evp.h>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace tronix {
inline std::string digest_hex(const std::string& data) {
    unsigned char digest[32]{};
    unsigned int n=0;
    if(EVP_Digest(data.data(),data.size(),digest,&n,EVP_sha256(),nullptr)!=1 || n!=32)
        throw std::runtime_error("cannot calculate configuration SHA-256");
    static const char chars[]="0123456789abcdef";
    std::string out;
    for(unsigned char c:digest){out.push_back(chars[c>>4]);out.push_back(chars[c&15]);}
    return out;
}
inline bool grant_name(const std::string& s,size_t max) {
    if(s.empty()||s.size()>max)return false;
    for(unsigned char c:s)if(!std::isalnum(c)&&c!='.'&&c!='_'&&c!='-')return false;
    return true;
}
class CapabilityGrants {
    std::set<std::string> operations_;
    std::string bundle_,version_,digest_;
public:
    CapabilityGrants(const std::string& grants_path,
                     const std::string& archive_path,
                     const std::string& pinned_sha) {
        if(grants_path.empty()||grants_path[0]!='/'||!valid_sha256(pinned_sha))
            throw std::runtime_error("grants require absolute policy and pinned archive");
        int fd=open(grants_path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
        if(fd<0)throw std::runtime_error("grants cannot be opened");
        std::string contents;
        try {
            struct stat st{};
            if(fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) || st.st_uid!=getuid() ||
               (st.st_mode&077)!=0 || st.st_nlink!=1 || st.st_size<=0 || st.st_size>4096)
                throw std::runtime_error("grants must be private owned regular file under 4KiB");
            contents.resize(static_cast<size_t>(st.st_size));
            size_t pos=0;
            while(pos<contents.size()){
                ssize_t n=read(fd,contents.data()+pos,contents.size()-pos);
                if(n<0&&errno==EINTR)continue;
                if(n<=0)throw std::runtime_error("grants read incomplete");
                pos+=static_cast<size_t>(n);
            }
        }catch(...){close(fd);throw;}
        close(fd);
        digest_=digest_hex(contents);
        if(contents.rfind("tronix.grants.v1\n",0)!=0 || contents.back()!='\n')
            throw std::runtime_error("invalid grants format");
        bool seen_bundle=false,seen_version=false,seen_artifact=false;
        size_t pos=17; // bytes in "tronix.grants.v1\n"
        while(pos<contents.size()){
            const size_t end=contents.find('\n',pos);
            if(end==std::string::npos)throw std::runtime_error("unterminated grants line");
            const std::string line=contents.substr(pos,end-pos);
            pos=end+1;
            if(line.rfind("bundle ",0)==0&&!seen_bundle){
                bundle_=line.substr(7);seen_bundle=true;
                if(!grant_name(bundle_,128))throw std::runtime_error("invalid bundle name");
            }else if(line.rfind("version ",0)==0&&!seen_version){
                version_=line.substr(8);seen_version=true;
                if(!grant_name(version_,48))throw std::runtime_error("invalid bundle version");
            }else if(line.rfind("artifact ",0)==0&&!seen_artifact){
                seen_artifact=true;
                if(line.substr(9)!=pinned_sha)throw std::runtime_error("artifact grant mismatch");
            }else if(line.rfind("allow ",0)==0){
                const std::string operation=line.substr(6);
                if(!grant_name(operation,TRONIX_OP_MAX)||
                   !operations_.insert(operation).second || operations_.size()>16)
                    throw std::runtime_error("invalid or duplicate grant operation");
            }else throw std::runtime_error("unknown or duplicate grants directive");
        }
        if(!seen_bundle||!seen_version||!seen_artifact||operations_.empty())
            throw std::runtime_error("incomplete operator grants");
        // Read metadata from exact previously verified private archive.
        int zip_error=0;
        zip_t* archive=zip_open(archive_path.c_str(),ZIP_RDONLY,&zip_error);
        if(!archive)throw std::runtime_error("cannot read pinned Celix bundle metadata");
        try {
            zip_stat_t meta{};
            if(zip_stat(archive,"META-INF/MANIFEST.json",0,&meta)!=0 ||
               meta.size==0 || meta.size>4096)
                throw std::runtime_error("missing bounded Celix bundle manifest");
            zip_file_t* entry=zip_fopen(archive,"META-INF/MANIFEST.json",0);
            if(!entry)throw std::runtime_error("cannot open Celix bundle manifest");
            std::string bytes(meta.size,'\0');
            auto n=zip_fread(entry,bytes.data(),bytes.size());
            zip_fclose(entry);
            if(n!=static_cast<zip_int64_t>(bytes.size()))
                throw std::runtime_error("Celix bundle manifest read incomplete");
            json_error_t err{};
            json_t* manifest=json_loadb(bytes.data(),bytes.size(),JSON_REJECT_DUPLICATES,&err);
            if(!manifest)throw std::runtime_error("invalid Celix bundle manifest JSON");
            const char* name=json_string_value(json_object_get(manifest,"CELIX_BUNDLE_SYMBOLIC_NAME"));
            const char* version=json_string_value(json_object_get(manifest,"CELIX_BUNDLE_VERSION"));
            bool matches=name && version && bundle_==name &&
                         std::string(version)==("version<"+version_+">");
            json_decref(manifest);
            if(!matches)throw std::runtime_error("pinned bundle name/version mismatch");
        }catch(...){zip_close(archive);throw;}
        zip_close(archive);
    }
    bool permits(const std::string& operation)const{return operations_.count(operation)>0;}
    const std::set<std::string>& operations()const{return operations_;}
    const std::string& digest()const{return digest_;}
    const std::string& bundle()const{return bundle_;}
    const std::string& version()const{return version_;}
};
} // namespace tronix

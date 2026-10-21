// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// TRONIX strict local peer policy. Linux AF_UNIX SO_PEERCRED authenticates UID.
// This intentionally represents ONE Unix security principal, not agent identities
// or process isolation. No untrusted client may choose its scheduling identity.
#pragma once
#include "tronix_service.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace tronix {
class PeerPolicy {
    uid_t owner_uid_{};
    std::set<std::string> allowed_;
    std::string principal_;

    static bool operation_name(const std::string& value) {
        if (value.empty() || value.size() > TRONIX_OP_MAX) return false;
        for (unsigned char c : value) {
            if (!(std::isalnum(c) || c == '.' || c == '_' || c == '-')) return false;
        }
        return true;
    }
public:
    explicit PeerPolicy(const std::string& path) {
#ifndef __linux__
        throw std::runtime_error("strict peer policy requires Linux SO_PEERCRED");
#else
        if (path.empty() || path[0] != '/') throw std::runtime_error("policy path must be absolute");
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) throw std::runtime_error("cannot open peer policy");
        std::string content;
        try {
            struct stat st{};
            if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != ::getuid() ||
                (st.st_mode & 077) != 0 || st.st_nlink != 1 || st.st_size <= 0 ||
                st.st_size > 4096) throw std::runtime_error("peer policy must be private, owned, regular and <=4096 bytes");
            content.resize(static_cast<size_t>(st.st_size));
            size_t off = 0;
            while (off < content.size()) {
                const ssize_t n = ::read(fd, content.data() + off, content.size() - off);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) throw std::runtime_error("cannot read peer policy completely");
                off += static_cast<size_t>(n);
            }
        } catch (...) {
            ::close(fd);
            throw;
        }
        ::close(fd);
        bool has_uid = false;
        size_t pos = 0;
        while (pos < content.size()) {
            const size_t end = content.find('\n', pos);
            if (end == std::string::npos) throw std::runtime_error("policy line must end with newline");
            const std::string line = content.substr(pos, end - pos);
            pos = end + 1;
            if (line.empty() || line[0] == '#') continue;
            if (line.rfind("uid ", 0) == 0 && !has_uid) {
                const std::string digits = line.substr(4);
                if (digits.empty() || !std::all_of(digits.begin(), digits.end(),
                     [](unsigned char c) { return c >= '0' && c <= '9'; }))
                    throw std::runtime_error("invalid policy uid");
                unsigned long long n = 0;
                try { n = std::stoull(digits); }
                catch (...) { throw std::runtime_error("invalid policy uid"); }
                if (n > std::numeric_limits<uid_t>::max()) throw std::runtime_error("policy uid overflow");
                owner_uid_ = static_cast<uid_t>(n);
                has_uid = true;
            } else if (line.rfind("allow ", 0) == 0) {
                const auto op = line.substr(6);
                if (!operation_name(op) || !allowed_.insert(op).second)
                    throw std::runtime_error("invalid or duplicate policy operation");
            } else {
                throw std::runtime_error("unknown or duplicate policy directive");
            }
        }
        if (!has_uid || owner_uid_ != ::getuid() || allowed_.empty())
            throw std::runtime_error("policy requires daemon owner uid and explicit allowed operations");
        principal_ = "uid:" + std::to_string(static_cast<unsigned long long>(owner_uid_));
#endif
    }

    uid_t uid() const { return owner_uid_; }
    const std::string& principal() const { return principal_; }
    bool permits(const std::string& operation) const { return allowed_.count(operation) != 0; }
};
} // namespace tronix

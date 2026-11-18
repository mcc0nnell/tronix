// SPDX-License-Identifier: Apache-2.0
// Stateless, numeric-only shadow entrypoint for the existing native Watchcore.
// No git, model, shell, credentials, network or publishing authority.
#include "watchcore/watchcore.hpp"
#include <array>
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string_view>
using namespace watchcore;
namespace {
int integer(const char* arg) {
    const std::string_view s(arg);
    int x{};
    auto [p, error] = std::from_chars(s.data(), s.data() + s.size(), x);
    if (s.empty() || error != std::errc{} || p != s.data() + s.size())
        throw std::invalid_argument("invalid integer");
    return x;
}
Risk risk(const char* s) {
    const int i = integer(s);
    if (i < 0 || i > 3) throw std::invalid_argument("risk outside range");
    return static_cast<Risk>(i);
}
bool boolean(const char* s) {
    const int i = integer(s);
    if (i != 0 && i != 1) throw std::invalid_argument("invalid bool");
    return i != 0;
}
const char* name(Risk x) {
    constexpr std::array names{"low", "medium", "high", "critical"};
    return names.at(static_cast<int>(x));
}
const char* name(SecurityDecision x) {
    constexpr std::array names{"proceed", "warn", "block"};
    return names.at(static_cast<int>(x));
}
}
int main(int argc, char** argv) {
    // Program + `security` + 11 normalized, numeric operands.
    if (argc != 13 || std::string_view(argv[1]) != "security") {
        std::cerr << "usage: watchcore_shadow security h_score h_risk h_signals "
                     "h_decisive m_score m_risk m_fallback m_rc warn block corroboration\n";
        return 2;
    }
    try {
        const int nsignals = integer(argv[4]);
        if (nsignals < 0 || nsignals > 100000) throw std::invalid_argument("invalid signal count");
        HeuristicVerdict h{integer(argv[2]), risk(argv[3]), {}, boolean(argv[5])};
        h.signals.resize(static_cast<size_t>(nsignals));
        ModelVerdict m{integer(argv[6]), risk(argv[7]), true, boolean(argv[8]), integer(argv[9])};
        SecurityPolicy policy{risk(argv[10]), risk(argv[11]), boolean(argv[12])};
        const auto d = decide_security(h, m, policy);
        std::cout << "{\"score\":" << d.score << ",\"risk\":\"" << name(d.risk)
                  << "\",\"decision\":\"" << name(d.decision)
                  << "\",\"block_downgraded_to_warn\":"
                  << (d.block_downgraded_to_warn ? "true" : "false")
                  << ",\"fallback_upgraded_to_warn\":"
                  << (d.fallback_upgraded_to_warn ? "true" : "false") << "}\n";
        return 0;
    } catch (const std::exception& exc) {
        std::cerr << "invalid shadow input: " << exc.what() << '\n';
        return 2;
    }
}

#include "watchcore/watchcore.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace watchcore {
namespace {

std::int64_t saturated_add(std::int64_t x, std::int64_t y) {
    if (y > 0 && x > std::numeric_limits<std::int64_t>::max() - y)
        return std::numeric_limits<std::int64_t>::max();
    if (y < 0 && x < std::numeric_limits<std::int64_t>::min() - y)
        return std::numeric_limits<std::int64_t>::min();
    return x + y;
}

// Exact exponential backoff until the cap; saturated above it.
std::int64_t capped_backoff(std::int64_t base, std::int64_t cap, std::size_t failures) {
    if (base < 0 || cap < 0) throw std::invalid_argument("negative backoff");
    if (failures == 0 || base == 0) return 0;
    std::int64_t x = std::min(base, cap);
    for (std::size_t i = 1; i < failures && x < cap; ++i) {
        if (x > cap / 2) return cap;
        x *= 2;
    }
    return x;
}

bool matches(const LedgerRow& row, const ItemKey& item,
             const std::string& default_repo) {
    const std::string& effective_repo = row.repo.empty() ? default_repo : row.repo;
    return effective_repo == item.repo && row.kind == item.kind &&
           row.number == item.number && row.revision == item.revision;
}

int rank(Risk risk) { return static_cast<int>(risk); }

} // namespace

std::string issue_revision(std::size_t comment_count) {
    return "comments:" + std::to_string(comment_count);
}

LedgerDecision decide_item(const ItemKey& item, const std::vector<LedgerRow>& rows,
                           const std::string& default_repo, std::int64_t now,
                           const RetryPolicy& policy) {
    if (policy.base_seconds < 0 || policy.max_seconds < 0)
        throw std::invalid_argument("negative retry setting");
    LedgerDecision out;
    std::optional<std::int64_t> last_failure;
    bool terminal = false;
    for (const auto& row : rows) {
        if (!matches(row, item, default_repo)) continue;
        if (row.status == Status::Done || row.status == Status::SecurityBlocked)
            terminal = true;
        if (row.status == Status::Failed && !row.failure_was_limit) {
            ++out.failure_count;
            if (row.timestamp_utc_seconds &&
                (!last_failure || *row.timestamp_utc_seconds > *last_failure))
                last_failure = row.timestamp_utc_seconds;
        }
    }
    // Python process() checks terminal -> exhausted -> backoff, in this order.
    if (terminal) {
        out.eligibility = Eligibility::AlreadyTerminal;
        return out;
    }
    if (out.failure_count >= policy.max_retries) {
        out.eligibility = Eligibility::RetriesExhausted;
        return out;
    }
    if (last_failure) {
        auto delay = capped_backoff(policy.base_seconds, policy.max_seconds, out.failure_count);
        out.next_retry_utc_seconds = saturated_add(*last_failure, delay);
        if (now < *out.next_retry_utc_seconds) out.eligibility = Eligibility::Backoff;
    }
    return out;
}

std::optional<CooldownState> active_cooldown(
    const Cooldowns& states, const std::string& vendor, double now) {
    for (const auto& key : {vendor, std::string("*")}) {
        auto it = states.find(key);
        if (it != states.end() && it->second.until_utc_seconds > now)
            return it->second;
    }
    return std::nullopt;
}

CooldownState record_cooldown(const Cooldowns& states, const std::string& vendor,
                              const LimitInfo& limit, double now,
                              const CooldownPolicy& policy) {
    if (policy.base_seconds < 0 || policy.max_seconds < 0 ||
        policy.strong_reset_max_seconds < 0)
        throw std::invalid_argument("negative cooldown setting");
    std::size_t n = 1;
    auto it = states.find(vendor);
    if (it != states.end() &&
        now - it->second.set_at_utc_seconds < policy.max_seconds) {
        n = static_cast<std::size_t>(it->second.consecutive) + 1;
    }
    const auto fallback = capped_backoff(policy.base_seconds, policy.max_seconds, n);
    double until = now + static_cast<double>(fallback);
    if (limit.reset_utc_seconds && *limit.reset_utc_seconds > now) {
        const auto ceiling = limit.strong ? policy.strong_reset_max_seconds : policy.max_seconds;
        until = std::min(*limit.reset_utc_seconds, now + static_cast<double>(ceiling));
    }
    CooldownState state;
    state.until_utc_seconds = until;
    state.set_at_utc_seconds = now;
    state.consecutive = static_cast<std::uint32_t>(std::min<std::size_t>(n, std::numeric_limits<std::uint32_t>::max()));
    state.reason = limit.reason;
    return state;
}

PickDecision pick_agent(const std::vector<Agent>& ordered,
                        const Cooldowns& cooldowns, double now,
                        const std::set<std::string>& exclude,
                        const std::set<std::string>& seen,
                        bool require_unseen) {
    std::vector<const Agent*> available;
    bool cooling = false;
    for (const auto& a : ordered) {
        if (require_unseen && seen.contains(a.vendor)) continue;
        if (!a.binary_available) continue;
        const bool is_cooling = active_cooldown(cooldowns, a.vendor, now).has_value();
        if (is_cooling) cooling = true; // even when excluded (Python rule)
        if (exclude.contains(a.name) || is_cooling) continue;
        available.push_back(&a);
    }
    if (available.empty()) return {std::nullopt, cooling ? UnavailableReason::Cooldown : UnavailableReason::Missing};
    if (!exclude.empty() && !seen.empty() && !require_unseen) {
        for (const Agent* a : available)
            if (!seen.contains(a->vendor)) return {a->name, UnavailableReason::None};
    }
    return {available.front()->name, UnavailableReason::None};
}

std::optional<double> roles_ready_at(
    const std::vector<std::vector<Agent>>& roles,
    const Cooldowns& cooldowns, double now) {
    std::optional<double> result;
    for (const auto& candidates : roles) {
        std::optional<double> earliest_for_role;
        for (const auto& a : candidates) {
            if (!a.binary_available) continue;
            auto state = active_cooldown(cooldowns, a.vendor, now);
            if (!state) continue;
            if (!earliest_for_role || state->until_utc_seconds < *earliest_for_role)
                earliest_for_role = state->until_utc_seconds;
        }
        if (earliest_for_role && (!result || *earliest_for_role > *result))
            result = earliest_for_role;
    }
    return result;
}

bool should_stop_pass(const std::vector<std::vector<Agent>>& roles,
                      const Cooldowns& cooldowns, double now) {
    bool some_cooling = false;
    for (const auto& candidates : roles) {
        const auto result = pick_agent(candidates, cooldowns, now);
        if (result.agent) return false;
        if (result.reason == UnavailableReason::Cooldown) some_cooling = true;
    }
    return some_cooling;
}

bool should_skip_item(const std::vector<std::vector<Agent>>& roles,
                      const Cooldowns& cooldowns, double now) {
    for (const auto& candidates : roles) {
        if (pick_agent(candidates, cooldowns, now).reason == UnavailableReason::Cooldown)
            return true;
    }
    return false;
}

Risk score_to_risk(int score) {
    if (score >= 80) return Risk::Critical;
    if (score >= 60) return Risk::High;
    if (score >= 30) return Risk::Medium;
    return Risk::Low;
}

SecurityResult decide_security(const HeuristicVerdict& h, const ModelVerdict& m,
                               const SecurityPolicy& p) {
    SecurityResult out;
    out.score = std::max(h.score, m.score);
    out.risk = std::max({h.risk, m.risk, score_to_risk(out.score)},
                        [](Risk a, Risk b){ return rank(a) < rank(b); });
    if (rank(out.risk) >= rank(p.block_above)) out.decision = SecurityDecision::Block;
    else if (rank(out.risk) >= rank(p.warn_above)) out.decision = SecurityDecision::Warn;
    else out.decision = SecurityDecision::Proceed;

    const bool corroborated = h.decisive || h.signals.size() >= 2 ||
                             !m.risk_valid || rank(m.risk) >= rank(p.warn_above);
    if (out.decision == SecurityDecision::Block && p.require_corroboration && !corroborated) {
        out.decision = SecurityDecision::Warn;
        out.block_downgraded_to_warn = true;
    }
    const bool triage_failed = m.agent_rc != 0;
    if (out.decision == SecurityDecision::Proceed && (m.agent_fallback || triage_failed) &&
        (m.score > 0 || !h.signals.empty())) {
        out.decision = SecurityDecision::Warn;
        out.fallback_upgraded_to_warn = true;
    }
    return out;
}

} // namespace watchcore

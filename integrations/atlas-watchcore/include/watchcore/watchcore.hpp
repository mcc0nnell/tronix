#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace watchcore {

// All functions are pure. I/O, clocks, JSON parsing, process execution and
// GitHub mutation are the responsibility of boundary adapters.
enum class Kind { Pr, Issue };
enum class Status { Done, SecurityBlocked, Failed, Deferred, Other };

struct ItemKey {
    std::string repo;
    Kind kind{Kind::Pr};
    std::int64_t number{0};
    std::string revision;  // PR head SHA; issue 'comments:<n>'
    bool operator==(const ItemKey&) const = default;
};

std::string issue_revision(std::size_t comment_count);

struct LedgerRow {
    // Empty repo denotes legacy pre-multi-repo row; belongs to default_repo.
    std::string repo;
    Kind kind{Kind::Pr};
    std::int64_t number{0};
    std::string revision;
    Status status{Status::Other};
    std::optional<std::int64_t> timestamp_utc_seconds;
    // Adapted from existing transcript classifier: 'failed' row was a limit.
    bool failure_was_limit{false};
};

struct RetryPolicy {
    std::size_t max_retries{3};
    std::int64_t base_seconds{900};
    std::int64_t max_seconds{86400};
};

enum class Eligibility { Ready, AlreadyTerminal, RetriesExhausted, Backoff };

struct LedgerDecision {
    Eligibility eligibility{Eligibility::Ready};
    std::size_t failure_count{0};
    std::optional<std::int64_t> next_retry_utc_seconds;
};

LedgerDecision decide_item(const ItemKey& item, const std::vector<LedgerRow>& rows,
                           const std::string& default_repo, std::int64_t now_utc_seconds,
                           const RetryPolicy& policy);

struct CooldownState {
    double until_utc_seconds{0};
    double set_at_utc_seconds{0};
    std::uint32_t consecutive{0};
    std::string reason;
};
using Cooldowns = std::map<std::string, CooldownState>;

struct LimitInfo {
    std::string reason;
    std::optional<double> reset_utc_seconds;
    bool strong{false};
};
struct CooldownPolicy {
    std::int64_t base_seconds{3600};
    std::int64_t max_seconds{21600};
    std::int64_t strong_reset_max_seconds{691200};
};

// Preserve compatibility with legacy global cooldown under "*".
std::optional<CooldownState> active_cooldown(
    const Cooldowns& states, const std::string& vendor, double now_utc_seconds);

CooldownState record_cooldown(const Cooldowns& states, const std::string& vendor,
                              const LimitInfo& limit, double now_utc_seconds,
                              const CooldownPolicy& policy);

struct Agent {
    std::string name;
    std::string vendor;
    bool binary_available{false}; // preflighted by adapter
};

enum class UnavailableReason { None, Cooldown, Missing };
struct PickDecision {
    std::optional<std::string> agent;
    UnavailableReason reason{UnavailableReason::None};
};

PickDecision pick_agent(const std::vector<Agent>& ordered_candidates,
                        const Cooldowns& cooldowns, double now_utc_seconds,
                        const std::set<std::string>& exclude_agents = {},
                        const std::set<std::string>& seen_vendors = {},
                        bool require_unseen_vendor = false);

std::optional<double> roles_ready_at(
    const std::vector<std::vector<Agent>>& role_candidates,
    const Cooldowns& cooldowns, double now_utc_seconds);

bool should_stop_pass(const std::vector<std::vector<Agent>>& all_role_candidates,
                      const Cooldowns& cooldowns, double now_utc_seconds);

bool should_skip_item(const std::vector<std::vector<Agent>>& required_role_candidates,
                      const Cooldowns& cooldowns, double now_utc_seconds);

enum class Risk { Low, Medium, High, Critical };
enum class SecurityDecision { Proceed, Warn, Block };

struct HeuristicVerdict {
    int score{0};
    Risk risk{Risk::Low};
    std::vector<std::string> signals;
    bool decisive{false};
};
struct ModelVerdict {
    int score{0};
    Risk risk{Risk::Low};
    bool risk_valid{true}; // invalid model risk counts as corroboration
    bool agent_fallback{false};
    int agent_rc{0};
};
struct SecurityPolicy {
    Risk warn_above{Risk::Medium};
    Risk block_above{Risk::High};
    bool require_corroboration{true};
};
struct SecurityResult {
    int score{0};
    Risk risk{Risk::Low};
    SecurityDecision decision{SecurityDecision::Proceed};
    bool block_downgraded_to_warn{false};
    bool fallback_upgraded_to_warn{false};
};

Risk score_to_risk(int score);
SecurityResult decide_security(const HeuristicVerdict& heur, const ModelVerdict& model,
                               const SecurityPolicy& policy);

} // namespace watchcore

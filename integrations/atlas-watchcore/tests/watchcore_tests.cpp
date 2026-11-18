#include "watchcore/watchcore.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace watchcore;
namespace {
int assertions = 0;
#define CHECK(expr) do { ++assertions; if (!(expr)) { \
  std::cerr << __FILE__ << ':' << __LINE__ << ": failed: " #expr << '\n'; \
  std::exit(1); } } while (0)

LedgerRow row(const ItemKey& k, Status status, std::optional<std::int64_t> ts={}, bool limit=false) {
    return {k.repo,k.kind,k.number,k.revision,status,ts,limit};
}
Agent agent(std::string name, std::string vendor, bool present=true) {
    return {std::move(name),std::move(vendor),present};
}

void ledger_tests() {
    CHECK(issue_revision(0) == "comments:0");
    CHECK(issue_revision(40) == "comments:40");
    ItemKey k{"sandialabs/atlas-ui-3",Kind::Pr,42,"abc"};
    const RetryPolicy policy{};
    CHECK(decide_item(k,{},k.repo,1000,policy).eligibility == Eligibility::Ready);
    CHECK(decide_item(k,{row(k,Status::Done,1)},k.repo,1000,policy).eligibility == Eligibility::AlreadyTerminal);
    CHECK(decide_item(k,{row(k,Status::SecurityBlocked,1)},k.repo,1000,policy).eligibility == Eligibility::AlreadyTerminal);
    CHECK(decide_item(k,{row(k,Status::Deferred,1)},k.repo,1000,policy).eligibility == Eligibility::Ready);
    CHECK(decide_item(k,{row(k,Status::Failed,1,true)},k.repo,1000,policy).eligibility == Eligibility::Ready);
    auto d=decide_item(k,{row(k,Status::Failed,1000)},k.repo,1500,policy);
    CHECK(d.failure_count==1);
    CHECK(d.eligibility == Eligibility::Backoff);
    CHECK(d.next_retry_utc_seconds && *d.next_retry_utc_seconds==1900);
    CHECK(decide_item(k,{row(k,Status::Failed,1000)},k.repo,1900,policy).eligibility==Eligibility::Ready);
    d=decide_item(k,{row(k,Status::Failed,1000),row(k,Status::Failed,1200)},k.repo,1500,policy);
    CHECK(d.failure_count==2);
    CHECK(d.next_retry_utc_seconds && *d.next_retry_utc_seconds==3000);
    CHECK(d.eligibility==Eligibility::Backoff);
    d=decide_item(k,{row(k,Status::Failed,1000),row(k,Status::Failed,1200),row(k,Status::Failed,1300)},k.repo,1500,policy);
    CHECK(d.failure_count==3 && d.eligibility==Eligibility::RetriesExhausted);
    d=decide_item(k,{row(k,Status::Failed,1000),row(k,Status::Done,1100),row(k,Status::Failed,1200),row(k,Status::Failed,1300)},k.repo,1500,policy);
    CHECK(d.eligibility==Eligibility::AlreadyTerminal);
    ItemKey other_repo{"other/atlas",Kind::Pr,42,"abc"};
    CHECK(decide_item(other_repo,{row(k,Status::Done)},k.repo,1000,policy).eligibility==Eligibility::Ready);
    ItemKey new_commit{k.repo,Kind::Pr,42,"def"};
    CHECK(decide_item(new_commit,{row(k,Status::Done)},k.repo,1000,policy).eligibility==Eligibility::Ready);
    ItemKey issue{k.repo,Kind::Issue,42,issue_revision(2)};
    CHECK(decide_item(issue,{row(k,Status::Done)},k.repo,1000,policy).eligibility==Eligibility::Ready);
    LedgerRow legacy{"",Kind::Pr,42,"abc",Status::Done,{100},false};
    CHECK(decide_item(k,{legacy},k.repo,1000,policy).eligibility==Eligibility::AlreadyTerminal);
    CHECK(decide_item(other_repo,{legacy},k.repo,1000,policy).eligibility==Eligibility::Ready);
    d=decide_item(k,{row(k,Status::Failed,std::nullopt)},k.repo,1500,policy);
    CHECK(d.failure_count==1 && d.eligibility==Eligibility::Ready && !d.next_retry_utc_seconds);
    d=decide_item(k,{row(k,Status::Failed,1000),row(k,Status::Failed,1002,true)},k.repo,1000,policy);
    CHECK(d.failure_count==1);
}

void cooldown_tests() {
    Cooldowns cd;
    LimitInfo limit{"rate limited",{},true};
    auto state=record_cooldown(cd,"provider-a",limit,1000,{});
    CHECK(state.consecutive==1);
    CHECK(state.until_utc_seconds==4600);
    cd["provider-a"]=state;
    CHECK(active_cooldown(cd,"provider-a",1001).has_value());
    CHECK(!active_cooldown(cd,"provider-b",1001));
    CHECK(!active_cooldown(cd,"provider-a",4600));
    auto fractional=record_cooldown({},"provider-f",limit,1000.25,{});
    CHECK(fractional.until_utc_seconds==4600.25);
    CHECK(active_cooldown({{"provider-f",fractional}},"provider-f",4600.24).has_value());
    CHECK(!active_cooldown({{"provider-f",fractional}},"provider-f",4600.25));
    state=record_cooldown(cd,"provider-a",limit,1100,{});
    CHECK(state.consecutive==2 && state.until_utc_seconds==8300);
    cd["provider-a"]=state;
    state=record_cooldown(cd,"provider-a",LimitInfo{"strong reset",100000,true},1200,{});
    CHECK(state.until_utc_seconds==100000);
    state=record_cooldown(cd,"provider-a",LimitInfo{"weak reset",100000,false},1200,{});
    CHECK(state.until_utc_seconds==22800);
    state=record_cooldown(cd,"provider-a",LimitInfo{"past reset",1,true},1200,{});
    CHECK(state.until_utc_seconds==15600);
    cd["*"]=CooldownState{5000,1000,1,"legacy"};
    CHECK(active_cooldown(cd,"provider-b",2000).has_value());
    CHECK(!active_cooldown(cd,"provider-b",5000));
    CHECK(!active_cooldown(cd,"provider-a",9000));
}

void routing_tests() {
    const Agent a=agent("a","vendor-1"), b=agent("b","vendor-1"),c=agent("c","vendor-2");
    CHECK(pick_agent({a,b,c},{},1000).agent=="a");
    CHECK(pick_agent({agent("none","other",false),a},{},1000).agent=="a");
    Cooldowns cd{{"vendor-1",CooldownState{3000,1000,1,"limit"}}};
    CHECK(pick_agent({a,b,c},cd,1000).agent=="c");
    auto unavailable=pick_agent({a,b},cd,1000);
    CHECK(!unavailable.agent && unavailable.reason==UnavailableReason::Cooldown);
    CHECK(pick_agent({agent("n","vendor-3",false)},{},1000).reason==UnavailableReason::Missing);
    CHECK(pick_agent({a,b,c},{},1000,{"a"},{"vendor-1"}).agent=="c"); // diversity only during failover
    CHECK(pick_agent({a,b,c},{},1000,{}, {"vendor-1"}).agent=="a");
    CHECK(pick_agent({a,b,c},{},1000,{}, {"vendor-1"},true).agent=="c");
    CHECK(pick_agent({a,b,c},{},1000,{}, {"vendor-1","vendor-2"},true).reason==UnavailableReason::Missing);
    CHECK(pick_agent({a},cd,1000,{"a"}).reason==UnavailableReason::Cooldown); // excluded but cooling
    Cooldowns both{{"vendor-1",{3000,1000,1,"limit"}},{"vendor-2",{4000,1000,1,"limit"}}};
    std::vector<std::vector<Agent>> roles{{a,c},{b}};
    CHECK(roles_ready_at(roles,both,1000)==3000);
    CHECK(should_stop_pass(roles,both,1000));
    CHECK(should_skip_item(roles,both,1000));
    CHECK(!should_stop_pass({{a},{c}},cd,1000)); // one role runnable
    CHECK(!should_stop_pass({{agent("no","v",false)}},cd,1000)); // all missing is config error
    CHECK(should_stop_pass({{a},{agent("no","v",false)}},cd,1000)); // mixed unavailable with cooling
    CHECK(!should_skip_item({{agent("no","v",false)}},cd,1000));
    CHECK(roles_ready_at({{a,c},{b}},cd,1000)==3000);
}

void security_tests() {
    CHECK(score_to_risk(29)==Risk::Low);
    CHECK(score_to_risk(30)==Risk::Medium);
    CHECK(score_to_risk(60)==Risk::High);
    CHECK(score_to_risk(80)==Risk::Critical);
    SecurityPolicy policy;
    auto r=decide_security({}, {}, policy);
    CHECK(r.decision==SecurityDecision::Proceed);
    r=decide_security(HeuristicVerdict{45,Risk::Medium,{"one"},false},{},policy);
    CHECK(r.decision==SecurityDecision::Warn);
    r=decide_security(HeuristicVerdict{75,Risk::High,{"one"},false},{},policy);
    CHECK(r.decision==SecurityDecision::Warn && r.block_downgraded_to_warn);
    r=decide_security(HeuristicVerdict{75,Risk::High,{"one"},true},{},policy);
    CHECK(r.decision==SecurityDecision::Block && !r.block_downgraded_to_warn);
    r=decide_security(HeuristicVerdict{65,Risk::High,{"one","two"},false},{},policy);
    CHECK(r.decision==SecurityDecision::Block);
    r=decide_security(HeuristicVerdict{65,Risk::High,{"one"},false},ModelVerdict{30,Risk::Medium},policy);
    CHECK(r.decision==SecurityDecision::Block);
    r=decide_security(HeuristicVerdict{65,Risk::High,{"one"},false},ModelVerdict{0,Risk::Low,false},policy);
    CHECK(r.decision==SecurityDecision::Block); // invalid model verdict corroborates
    r=decide_security({},ModelVerdict{5,Risk::Low,true,true,0},policy);
    CHECK(r.decision==SecurityDecision::Warn && r.fallback_upgraded_to_warn);
    r=decide_security({},ModelVerdict{0,Risk::Low,true,true,0},policy);
    CHECK(r.decision==SecurityDecision::Proceed);
    r=decide_security(HeuristicVerdict{5,Risk::Low,{"signal"},false},ModelVerdict{0,Risk::Low,true,false,1},policy);
    CHECK(r.decision==SecurityDecision::Warn);
    r=decide_security({},ModelVerdict{0,Risk::Low,true,false,1},policy);
    CHECK(r.decision==SecurityDecision::Proceed);
    r=decide_security(HeuristicVerdict{65,Risk::High,{"one"},false},ModelVerdict{0,Risk::Low,true,true,0},policy);
    CHECK(r.decision==SecurityDecision::Warn && !r.fallback_upgraded_to_warn); // never manufacture block
    policy.require_corroboration=false;
    CHECK(decide_security(HeuristicVerdict{65,Risk::High,{"one"},false},{},policy).decision==SecurityDecision::Block);
}
}

int main() {
    ledger_tests();
    cooldown_tests();
    routing_tests();
    security_tests();
    std::cout << "PASS: " << assertions << " assertions\n";
}

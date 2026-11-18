// SPDX-License-Identifier: Apache-2.0
// Genuine Celix bundle: Watchcore decision service has no I/O or posting authority.
#include "tronix_watchcore_security.h"
#include "tronix_watchcore_ledger.h"
#include "tronix_watchcore_routing.h"
#include "watchcore/watchcore.hpp"
#include <celix_bundle_activator.h>
#include <celix_bundle_context.h>
#include <exception>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

namespace {
struct State {
    long service_id{-1};
    tronix_watchcore_security_service_t service{};
    long ledger_service_id{-1};
    tronix_watchcore_ledger_service_t ledger_service{};
    long cooldown_service_id{-1};
    tronix_watchcore_cooldown_service_t cooldown_service{};
    long routing_service_id{-1};
    tronix_watchcore_routing_service_t routing_service{};
};
bool risk_ok(int n) { return n >= 0 && n <= 3; }
bool bit_ok(int n) { return n == 0 || n == 1; }

int decide(void *, const tronix_watchcore_security_input_t *i,
           tronix_watchcore_security_output_t *out) noexcept {
    if (!i || !out || !risk_ok(i->heur_risk) || !risk_ok(i->model_risk) ||
        !risk_ok(i->policy_warn_risk) || !risk_ok(i->policy_block_risk) ||
        i->heur_signal_count < 0 || i->heur_signal_count > 100000 ||
        !bit_ok(i->heur_decisive) || !bit_ok(i->model_fallback) ||
        !bit_ok(i->policy_require_corroboration)) {
        return -1;
    }
    try {
        using namespace watchcore;
        HeuristicVerdict heur{i->heur_score, static_cast<Risk>(i->heur_risk),
                              {}, i->heur_decisive != 0};
        heur.signals.resize(static_cast<std::size_t>(i->heur_signal_count));
        ModelVerdict model{i->model_score, static_cast<Risk>(i->model_risk),
                           true, i->model_fallback != 0, i->model_rc};
        SecurityPolicy policy{static_cast<Risk>(i->policy_warn_risk),
                              static_cast<Risk>(i->policy_block_risk),
                              i->policy_require_corroboration != 0};
        const SecurityResult result = decide_security(heur, model, policy);
        *out = {result.score, static_cast<int>(result.risk),
                static_cast<int>(result.decision),
                result.block_downgraded_to_warn ? 1 : 0,
                result.fallback_upgraded_to_warn ? 1 : 0};
        return 0;
    } catch (const std::exception &) {
        return -1;
    }
}
// All pointers are caller-owned for the duration of this synchronous call.
// No data is persisted and no process/external code is invoked here.
bool text_ok(const char* p, bool allow_empty) {
    if (!p) return false;
    size_t n = strnlen(p, TRONIX_WATCHCORE_LEDGER_MAX_STRING + 1);
    if (n > TRONIX_WATCHCORE_LEDGER_MAX_STRING || (n == 0 && !allow_empty)) return false;
    for (size_t j=0;j<n;++j) {
        const unsigned char c=static_cast<unsigned char>(p[j]);
        if (c<32 || c>=127) return false;
    }
    return true;
}
bool kind_ok(int n) { return n == 0 || n == 1; }
bool status_ok(int n) { return n >= 0 && n <= 4; }

int decide_ledger(void*, const tronix_watchcore_ledger_request_t* req,
                  tronix_watchcore_ledger_result_t* out) noexcept {
    if (!req || !out || !text_ok(req->repo, false) ||
        !text_ok(req->revision, false) || !text_ok(req->default_repo, true) ||
        !kind_ok(req->kind) || req->number < 1 ||
        req->row_count > TRONIX_WATCHCORE_LEDGER_MAX_ROWS ||
        (req->row_count != 0 && !req->rows) ||
        req->max_retries > 10000 ||
        req->backoff_base_seconds < 0 || req->backoff_max_seconds < 0)
        return -1;
    try {
        using namespace watchcore;
        std::vector<LedgerRow> rows;
        rows.reserve(req->row_count);
        for (size_t j=0;j<req->row_count;++j) {
            const auto &r=req->rows[j];
            if (!text_ok(r.repo, true) || !text_ok(r.revision, false) ||
                !kind_ok(r.kind) || r.number < 1 || !status_ok(r.status) ||
                !bit_ok(r.has_timestamp) || !bit_ok(r.failure_was_limit))
                return -1;
            LedgerRow converted{
                r.repo, static_cast<Kind>(r.kind), r.number,
                r.revision, static_cast<Status>(r.status), std::nullopt,
                r.failure_was_limit != 0
            };
            if (r.has_timestamp) converted.timestamp_utc_seconds=r.timestamp_utc_seconds;
            rows.push_back(std::move(converted));
        }
        const ItemKey item{req->repo,static_cast<Kind>(req->kind),req->number,req->revision};
        const RetryPolicy policy{req->max_retries,req->backoff_base_seconds,
                                 req->backoff_max_seconds};
        const auto answer=decide_item(item,rows,req->default_repo,req->now_utc_seconds,policy);
        *out={
            static_cast<int>(answer.eligibility),
            answer.failure_count,
            answer.next_retry_utc_seconds.has_value() ? 1 : 0,
            answer.next_retry_utc_seconds.value_or(0)
        };
        return 0;
    } catch (...) {
        return -1;
    }
}

bool label_ok(const char* value) {
    if (!value) return false;
    const size_t n=strnlen(value,TRONIX_WATCHCORE_MAX_LABEL+1);
    if(n==0 || n>TRONIX_WATCHCORE_MAX_LABEL) return false;
    for(size_t i=0;i<n;++i) {
        const auto ch=static_cast<unsigned char>(value[i]);
        if (ch<33 || ch>126) return false;
    }
    return true;
}
bool finite(double value) { return std::isfinite(value); }
bool states_ok(const tronix_watchcore_cooldown_state_t* data,size_t count) {
    if (count>TRONIX_WATCHCORE_MAX_STATES || (count && !data)) return false;
    std::set<std::string> seen;
    for(size_t i=0;i<count;++i) {
        const auto& state=data[i];
        if(!label_ok(state.vendor) || !finite(state.until_utc_seconds) ||
           !finite(state.set_at_utc_seconds) ||
           !seen.insert(state.vendor).second) return false;
    }
    return true;
}
watchcore::Cooldowns convert_states(const tronix_watchcore_cooldown_state_t* states,
                                    size_t count) {
    watchcore::Cooldowns converted;
    for(size_t i=0;i<count;++i) {
        const auto& state=states[i];
        converted.emplace(state.vendor,watchcore::CooldownState{
            state.until_utc_seconds,state.set_at_utc_seconds,state.consecutive,
            "normalized"
        });
    }
    return converted;
}
int decide_cooldown(void*,const tronix_watchcore_cooldown_request_t* in,
                    tronix_watchcore_cooldown_result_t* out) noexcept {
    if(!in || !out || in->operation<0 || in->operation>1 ||
       !label_ok(in->vendor) || !finite(in->now_utc_seconds) ||
       !states_ok(in->states,in->state_count) ||
       !bit_ok(in->has_reset) || !bit_ok(in->strong_reset) ||
       !finite(in->reset_utc_seconds) || in->base_seconds<0 ||
       in->max_seconds<0 || in->strong_reset_max_seconds<0) return -1;
    try {
        const auto states=convert_states(in->states,in->state_count);
        std::optional<watchcore::CooldownState> state;
        if(in->operation==0) {
            state=watchcore::active_cooldown(states,in->vendor,in->now_utc_seconds);
        } else {
            state=watchcore::record_cooldown(
                states,in->vendor,
                watchcore::LimitInfo{"normalized",
                    in->has_reset ? std::optional<double>{in->reset_utc_seconds}
                                  : std::nullopt,
                    in->strong_reset!=0},
                in->now_utc_seconds,
                watchcore::CooldownPolicy{
                    in->base_seconds,in->max_seconds,in->strong_reset_max_seconds
                });
        }
        *out={state.has_value()?1:0,0,0,0};
        if (state) {
            if(!finite(state->until_utc_seconds) || !finite(state->set_at_utc_seconds))
                return -1;
            out->until_utc_seconds=state->until_utc_seconds;
            out->set_at_utc_seconds=state->set_at_utc_seconds;
            out->consecutive=state->consecutive;
        }
        return 0;
    } catch(...) {return -1;}
}
int decide_routing(void*,const tronix_watchcore_routing_request_t* in,
                   tronix_watchcore_routing_result_t* out) noexcept {
    if(!in || !out || in->operation<0 || in->operation>1 ||
       !finite(in->now_utc_seconds) ||
       in->agent_count>TRONIX_WATCHCORE_MAX_AGENTS ||
       (in->agent_count && !in->agents) ||
       in->role_count>TRONIX_WATCHCORE_MAX_ROLES ||
       (in->role_count && !in->roles) ||
       in->exclude_count>TRONIX_WATCHCORE_MAX_AGENTS ||
       (in->exclude_count && !in->exclude_agents) ||
       in->seen_count>TRONIX_WATCHCORE_MAX_AGENTS ||
       (in->seen_count && !in->seen_vendors) ||
       !bit_ok(in->require_unseen_vendor) ||
       !states_ok(in->states,in->state_count)) return -1;
    try {
        std::vector<watchcore::Agent> agents;
        agents.reserve(in->agent_count);
        for(size_t i=0;i<in->agent_count;++i) {
            const auto& a=in->agents[i];
            if (!label_ok(a.name) || !label_ok(a.vendor) ||
                !bit_ok(a.binary_available)) return -1;
            agents.push_back({a.name,a.vendor,a.binary_available!=0});
        }
        std::set<std::string> exclude,seen;
        for(size_t i=0;i<in->exclude_count;++i) {
            if(!label_ok(in->exclude_agents[i])) return -1;
            exclude.insert(in->exclude_agents[i]);
        }
        for(size_t i=0;i<in->seen_count;++i) {
            if(!label_ok(in->seen_vendors[i])) return -1;
            seen.insert(in->seen_vendors[i]);
        }
        const auto states=convert_states(in->states,in->state_count);
        *out={-1,0,0,0,0,0};
        if(in->operation==0) {
            if(in->role_count!=0) return -1;
            const auto result=watchcore::pick_agent(
                agents,states,in->now_utc_seconds,exclude,seen,
                in->require_unseen_vendor!=0);
            out->unavailable_reason=static_cast<int>(result.reason);
            if(result.agent) {
                for(size_t i=0;i<agents.size();++i) {
                    if(agents[i].name==*result.agent) {
                        out->selected_index=static_cast<int>(i);
                        break;
                    }
                }
                if(out->selected_index<0) return -1;
            }
        } else {
            if(in->exclude_count || in->seen_count ||
               in->require_unseen_vendor) return -1;
            std::vector<std::vector<watchcore::Agent>> roles;
            roles.reserve(in->role_count);
            for(size_t i=0;i<in->role_count;++i) {
                const auto& r=in->roles[i];
                if(r.offset>in->agent_count ||
                   r.length>in->agent_count-r.offset) return -1;
                roles.emplace_back(agents.begin()+static_cast<std::ptrdiff_t>(r.offset),
                                   agents.begin()+static_cast<std::ptrdiff_t>(r.offset+r.length));
            }
            const auto ready=watchcore::roles_ready_at(roles,states,in->now_utc_seconds);
            if(ready) {
                if(!finite(*ready)) return -1;
                out->has_ready_at=1;
                out->ready_at_utc_seconds=*ready;
            }
            out->stop_pass=watchcore::should_stop_pass(
                roles,states,in->now_utc_seconds)?1:0;
            out->skip_item=watchcore::should_skip_item(
                roles,states,in->now_utc_seconds)?1:0;
        }
        return 0;
    } catch(...) {return -1;}
}
celix_status_t start(State *state, celix_bundle_context_t *ctx) {
    state->service.handle = state;
    state->service.decide = decide;
    celix_service_registration_options_t options{};
    options.svc = &state->service;
    options.serviceName = TRONIX_WATCHCORE_SECURITY_NAME;
    options.serviceVersion = TRONIX_WATCHCORE_SECURITY_VERSION;
    state->service_id = celix_bundleContext_registerServiceWithOptions(ctx, &options);
    if (state->service_id < 0) return CELIX_SERVICE_EXCEPTION;
    state->ledger_service.handle = state;
    state->ledger_service.decide = decide_ledger;
    celix_service_registration_options_t ledger_opts{};
    ledger_opts.svc = &state->ledger_service;
    ledger_opts.serviceName = TRONIX_WATCHCORE_LEDGER_NAME;
    ledger_opts.serviceVersion = TRONIX_WATCHCORE_LEDGER_VERSION;
    state->ledger_service_id = celix_bundleContext_registerServiceWithOptions(ctx, &ledger_opts);
    if (state->ledger_service_id < 0) {
        celix_bundleContext_unregisterService(ctx, state->service_id);
        state->service_id = -1;
        return CELIX_SERVICE_EXCEPTION;
    }
    state->cooldown_service.handle=state;
    state->cooldown_service.decide=decide_cooldown;
    celix_service_registration_options_t cooldown_opts{};
    cooldown_opts.svc=&state->cooldown_service;
    cooldown_opts.serviceName=TRONIX_WATCHCORE_COOLDOWN_NAME;
    cooldown_opts.serviceVersion=TRONIX_WATCHCORE_COOLDOWN_VERSION;
    state->cooldown_service_id=
        celix_bundleContext_registerServiceWithOptions(ctx,&cooldown_opts);
    if(state->cooldown_service_id<0) {
        celix_bundleContext_unregisterService(ctx,state->ledger_service_id);
        celix_bundleContext_unregisterService(ctx,state->service_id);
        state->ledger_service_id=state->service_id=-1;
        return CELIX_SERVICE_EXCEPTION;
    }
    state->routing_service.handle=state;
    state->routing_service.decide=decide_routing;
    celix_service_registration_options_t routing_opts{};
    routing_opts.svc=&state->routing_service;
    routing_opts.serviceName=TRONIX_WATCHCORE_ROUTING_NAME;
    routing_opts.serviceVersion=TRONIX_WATCHCORE_ROUTING_VERSION;
    state->routing_service_id=
        celix_bundleContext_registerServiceWithOptions(ctx,&routing_opts);
    if(state->routing_service_id<0) {
        celix_bundleContext_unregisterService(ctx,state->cooldown_service_id);
        celix_bundleContext_unregisterService(ctx,state->ledger_service_id);
        celix_bundleContext_unregisterService(ctx,state->service_id);
        state->routing_service_id=state->cooldown_service_id=
            state->ledger_service_id=state->service_id=-1;
        return CELIX_SERVICE_EXCEPTION;
    }
    return CELIX_SUCCESS;
}
celix_status_t stop(State *state, celix_bundle_context_t *ctx) {
    if (state->routing_service_id >= 0) {
        celix_bundleContext_unregisterService(ctx,state->routing_service_id);
        state->routing_service_id=-1;
    }
    if (state->cooldown_service_id >= 0) {
        celix_bundleContext_unregisterService(ctx,state->cooldown_service_id);
        state->cooldown_service_id=-1;
    }
    if (state->ledger_service_id >= 0) {
        celix_bundleContext_unregisterService(ctx, state->ledger_service_id);
        state->ledger_service_id = -1;
    }
    if (state->service_id >= 0) {
        celix_bundleContext_unregisterService(ctx, state->service_id);
        state->service_id = -1;
    }
    return CELIX_SUCCESS;
}
} // namespace
CELIX_GEN_BUNDLE_ACTIVATOR(State, start, stop)

// SPDX-License-Identifier: Apache-2.0
// Genuine Celix bundle: Watchcore decision service has no I/O or posting authority.
#include "tronix_watchcore_security.h"
#include "tronix_watchcore_ledger.h"
#include "watchcore/watchcore.hpp"
#include <celix_bundle_activator.h>
#include <celix_bundle_context.h>
#include <exception>
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
    return CELIX_SUCCESS;
}
celix_status_t stop(State *state, celix_bundle_context_t *ctx) {
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

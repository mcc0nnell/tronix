// SPDX-License-Identifier: Apache-2.0
// Genuine Celix bundle: Watchcore decision service has no I/O or posting authority.
#include "tronix_watchcore_security.h"
#include "watchcore/watchcore.hpp"
#include <celix_bundle_activator.h>
#include <celix_bundle_context.h>
#include <exception>
#include <vector>

namespace {
struct State {
    long service_id{-1};
    tronix_watchcore_security_service_t service{};
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
celix_status_t start(State *state, celix_bundle_context_t *ctx) {
    state->service.handle = state;
    state->service.decide = decide;
    celix_service_registration_options_t options{};
    options.svc = &state->service;
    options.serviceName = TRONIX_WATCHCORE_SECURITY_NAME;
    options.serviceVersion = TRONIX_WATCHCORE_SECURITY_VERSION;
    state->service_id = celix_bundleContext_registerServiceWithOptions(ctx, &options);
    return state->service_id >= 0 ? CELIX_SUCCESS : CELIX_SERVICE_EXCEPTION;
}
celix_status_t stop(State *state, celix_bundle_context_t *ctx) {
    if (state->service_id >= 0) {
        celix_bundleContext_unregisterService(ctx, state->service_id);
        state->service_id = -1;
    }
    return CELIX_SUCCESS;
}
} // namespace
CELIX_GEN_BUNDLE_ACTIVATOR(State, start, stop)

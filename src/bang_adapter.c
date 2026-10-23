// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Celix operation adapter for synthetic 6lowBang research only.
// The numeric Tronix job ABI NEVER transports caption text.
#include "tronix_service.h"
#include "tronix_bang_fixture.h"
#include <celix_bundle_activator.h>
#include <celix_bundle_context.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    long service_id;
    tronix_operation_service_t operation;
} tronix_bang_adapter_t;

static int allowed(const char* op) {
    return op && (!strcmp(op, "bang.synthetic_delivery") ||
                  !strcmp(op, "bang.synthetic_gap"));
}
static int execute(void* handle, const char* op, int64_t* value) {
    (void)handle;
    return allowed(op) ? tronix_bang_fixture_run(op, value) : 0;
}
static int lane_for(void* handle, const char* op) {
    (void)handle;
    return allowed(op) ? 0 : -1; // background; never priority-injected
}
static uint32_t budget_for(void* handle, const char* op) {
    (void)handle;
    return allowed(op) ? 2000 : 0;
}
static const char* operations(void* handle) {
    (void)handle;
    return "bang.synthetic_delivery,bang.synthetic_gap";
}
static celix_status_t start(tronix_bang_adapter_t* a, celix_bundle_context_t* ctx) {
    memset(a, 0, sizeof(*a));
    a->service_id = -1;
    const char* enabled = getenv("TRONIX_BANG_ENABLE_FIXTURE");
    if (!enabled || strcmp(enabled, "1") != 0) return CELIX_SERVICE_EXCEPTION;
    a->operation.handle = a;
    a->operation.execute = execute;
    a->operation.lane_for = lane_for;
    a->operation.budget_for = budget_for;
    a->operation.operations = operations;
    celix_service_registration_options_t opts = CELIX_EMPTY_SERVICE_REGISTRATION_OPTIONS;
    opts.svc = &a->operation;
    opts.serviceName = TRONIX_OPERATION_SERVICE;
    opts.serviceVersion = TRONIX_ABI_VERSION;
    a->service_id = celix_bundleContext_registerServiceWithOptions(ctx, &opts);
    return a->service_id >= 0 ? CELIX_SUCCESS : CELIX_SERVICE_EXCEPTION;
}
static celix_status_t stop(tronix_bang_adapter_t* a, celix_bundle_context_t* ctx) {
    if (a->service_id >= 0) celix_bundleContext_unregisterService(ctx, a->service_id);
    return CELIX_SUCCESS;
}
CELIX_GEN_BUNDLE_ACTIVATOR(tronix_bang_adapter_t, start, stop)

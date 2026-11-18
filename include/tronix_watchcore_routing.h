/* SPDX-License-Identifier: Apache-2.0 */
/* Pure, numeric, observation-only Celix ABIs for vendor cooldown and agent routing. */
#ifndef TRONIX_WATCHCORE_ROUTING_H
#define TRONIX_WATCHCORE_ROUTING_H
#include <stddef.h>
#include <stdint.h>
#define TRONIX_WATCHCORE_COOLDOWN_NAME "org.tronix.watchcore.cooldown"
#define TRONIX_WATCHCORE_COOLDOWN_VERSION "1.0.0"
#define TRONIX_WATCHCORE_ROUTING_NAME "org.tronix.watchcore.routing"
#define TRONIX_WATCHCORE_ROUTING_VERSION "1.0.0"
#define TRONIX_WATCHCORE_MAX_AGENTS 128u
#define TRONIX_WATCHCORE_MAX_STATES 128u
#define TRONIX_WATCHCORE_MAX_ROLES 32u
#define TRONIX_WATCHCORE_MAX_LABEL 128u
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* vendor; /* "*" is legacy global cooldown */
    double until_utc_seconds;
    double set_at_utc_seconds;
    uint32_t consecutive;
} tronix_watchcore_cooldown_state_t;

typedef struct {
    int operation;             /* 0=active,1=record */
    const char* vendor;
    double now_utc_seconds;
    const tronix_watchcore_cooldown_state_t* states;
    size_t state_count;
    int has_reset;
    double reset_utc_seconds;
    int strong_reset;
    int64_t base_seconds;
    int64_t max_seconds;
    int64_t strong_reset_max_seconds;
} tronix_watchcore_cooldown_request_t;
typedef struct {
    int has_state;
    double until_utc_seconds;
    double set_at_utc_seconds;
    uint32_t consecutive;
} tronix_watchcore_cooldown_result_t;
typedef struct {
    void* handle;
    int (*decide)(void* handle, const tronix_watchcore_cooldown_request_t* in,
                  tronix_watchcore_cooldown_result_t* out);
} tronix_watchcore_cooldown_service_t;

typedef struct {
    const char* name;
    const char* vendor;
    int binary_available;
} tronix_watchcore_agent_t;
typedef struct { size_t offset; size_t length; } tronix_watchcore_role_t;

typedef struct {
    int operation;             /* 0=pick,1=roles (readiness/stop/skip) */
    double now_utc_seconds;
    const tronix_watchcore_agent_t* agents;
    size_t agent_count;
    const tronix_watchcore_role_t* roles;
    size_t role_count;
    const tronix_watchcore_cooldown_state_t* states;
    size_t state_count;
    const char* const* exclude_agents;
    size_t exclude_count;
    const char* const* seen_vendors;
    size_t seen_count;
    int require_unseen_vendor;
} tronix_watchcore_routing_request_t;
typedef struct {
    int selected_index;        /* -1 means no usable agent */
    int unavailable_reason;    /* 0=none,1=cooldown,2=missing */
    int has_ready_at;
    double ready_at_utc_seconds;
    int stop_pass;
    int skip_item;
} tronix_watchcore_routing_result_t;
typedef struct {
    void* handle;
    int (*decide)(void* handle, const tronix_watchcore_routing_request_t* in,
                  tronix_watchcore_routing_result_t* out);
} tronix_watchcore_routing_service_t;

#ifdef __cplusplus
}
#endif
#endif

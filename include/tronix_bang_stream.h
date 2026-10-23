// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "lowbang.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define TRONIX_BANG_STREAM_SERVICE "org.tronix.bang.stream"
#define TRONIX_BANG_STREAM_ABI_VERSION "1.0.0"
#define TRONIX_BANG_MAX_SUBSCRIBERS 8
#define TRONIX_BANG_QUEUE_CAPACITY 64
// Application-level handling only. No assertion of radio or human delivery.
typedef struct {
    size_t queued_per_subscriber;
    uint64_t waiting_for;
    bool gap_reported;
    uint64_t missing_from;
    uint64_t missing_through;
} tronix_bang_admission_t;
typedef struct tronix_bang_hub tronix_bang_hub;
tronix_bang_hub* tronix_bang_hub_new(void);
void tronix_bang_hub_free(tronix_bang_hub* hub);
bang_status tronix_bang_hub_pin(tronix_bang_hub* hub, const char* channel,
    const uint8_t ed25519_public_key[32]);
bang_status tronix_bang_hub_enable(tronix_bang_hub* hub);
void tronix_bang_hub_disable(tronix_bang_hub* hub);
bang_status tronix_bang_hub_subscribe(tronix_bang_hub* hub,
    const char* channel, uint32_t* subscription_id);
bang_status tronix_bang_hub_unsubscribe(tronix_bang_hub* hub, uint32_t subscription_id);
bang_status tronix_bang_hub_accept(tronix_bang_hub* hub,
    const bang_event* signed_event, uint64_t now_ms, tronix_bang_admission_t* result);
bang_status tronix_bang_hub_flush(tronix_bang_hub* hub, const char* channel,
    const char* session, uint64_t now_ms, uint32_t hold_ms,
    tronix_bang_admission_t* result);
bang_status tronix_bang_hub_poll(tronix_bang_hub* hub,
    uint32_t subscription_id, bang_event* out);
// CELIX-only typed service. The existing integer-returning operation ABI
// must NEVER be used to transport caption contents or subscription keys.
typedef struct {
    void* handle;
    bang_status (*pin)(void*, const char*, const uint8_t[32]);
    bang_status (*enable)(void*);
    void (*disable)(void*);
    bang_status (*subscribe)(void*, const char*, uint32_t*);
    bang_status (*unsubscribe)(void*, uint32_t);
    bang_status (*accept)(void*, const bang_event*, uint64_t, tronix_bang_admission_t*);
    bang_status (*flush)(void*, const char*, const char*, uint64_t, uint32_t,
                        tronix_bang_admission_t*);
    bang_status (*poll)(void*, uint32_t, bang_event*);
} tronix_bang_stream_service_t;
#ifdef __cplusplus
}
#endif

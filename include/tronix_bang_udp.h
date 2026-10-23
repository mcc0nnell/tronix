// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "tronix_bang_stream.h"
#include <stdint.h>
// C-only, loopback IPv6 research transport; not a production Thread socket.
// All messages must traverse the pinned-key Celix stream accept callback.
#define TRONIX_BANG_UDP_SERVICE "org.tronix.bang.udp"
#define TRONIX_BANG_UDP_ABI_VERSION "1.0.0"
typedef struct {
    void* handle;
    uint16_t (*local_port)(void*);
    int (*send_event)(void*,const bang_event*,uint16_t target_port);
    bang_status (*receive_to_stream)(void*,uint32_t timeout_ms,
                                    tronix_bang_admission_t* receipt);
} tronix_bang_udp_service_t;

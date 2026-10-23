// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Dedicated, opt-in in-process Celix caption service.
// This does not extend Tronix's numeric job protocol or expose a radio socket.
#include "tronix_bang_stream.h"
#include <celix_bundle_activator.h>
#include <celix_bundle_context.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    long service_id;
    tronix_bang_hub* hub;
    tronix_bang_stream_service_t service;
} bang_stream_adapter;
static bang_status pin(void* handle,const char* channel,const uint8_t key[32]) {
    return tronix_bang_hub_pin(handle,channel,key);
}
static bang_status enable(void* handle) {return tronix_bang_hub_enable(handle);}
static void disable(void* handle) {tronix_bang_hub_disable(handle);}
static bang_status subscribe(void* handle,const char* channel,uint32_t* id) {
    return tronix_bang_hub_subscribe(handle,channel,id);
}
static bang_status unsubscribe(void* handle,uint32_t id) {
    return tronix_bang_hub_unsubscribe(handle,id);
}
static bang_status accept(void* handle,const bang_event* event,uint64_t now,
                          tronix_bang_admission_t* result) {
    return tronix_bang_hub_accept(handle,event,now,result);
}
static bang_status flush(void* handle,const char* channel,const char* session,
                         uint64_t now,uint32_t hold,tronix_bang_admission_t* result) {
    return tronix_bang_hub_flush(handle,channel,session,now,hold,result);
}
static bang_status poll(void* handle,uint32_t id,bang_event* out) {
    return tronix_bang_hub_poll(handle,id,out);
}
static celix_status_t start(bang_stream_adapter* adapter,celix_bundle_context_t* ctx) {
    memset(adapter,0,sizeof(*adapter));
    adapter->service_id=-1;
    // Opt-in because publisher identity and subscriber authorization remain
    // trusted, in-process-only research fixtures (NOT production admission).
    const char* enabled=getenv("TRONIX_BANG_STREAM_ENABLE_FIXTURE");
    if (!enabled || strcmp(enabled,"1")!=0) return CELIX_SERVICE_EXCEPTION;
    adapter->hub=tronix_bang_hub_new();
    if (!adapter->hub) return CELIX_SERVICE_EXCEPTION;
    adapter->service.handle=adapter->hub;
    adapter->service.pin=pin;
    adapter->service.enable=enable;
    adapter->service.disable=disable;
    adapter->service.subscribe=subscribe;
    adapter->service.unsubscribe=unsubscribe;
    adapter->service.accept=accept;
    adapter->service.flush=flush;
    adapter->service.poll=poll;
    celix_service_registration_options_t opts=CELIX_EMPTY_SERVICE_REGISTRATION_OPTIONS;
    opts.svc=&adapter->service;
    opts.serviceName=TRONIX_BANG_STREAM_SERVICE;
    opts.serviceVersion=TRONIX_BANG_STREAM_ABI_VERSION;
    adapter->service_id=celix_bundleContext_registerServiceWithOptions(ctx,&opts);
    if (adapter->service_id>=0) return CELIX_SUCCESS;
    tronix_bang_hub_free(adapter->hub);
    adapter->hub=NULL;
    return CELIX_SERVICE_EXCEPTION;
}
static celix_status_t stop(bang_stream_adapter* adapter,celix_bundle_context_t* ctx) {
    if (adapter->service_id>=0)
        celix_bundleContext_unregisterService(ctx,adapter->service_id);
    tronix_bang_hub_free(adapter->hub);
    adapter->hub=NULL;
    return CELIX_SUCCESS;
}
CELIX_GEN_BUNDLE_ACTIVATOR(bang_stream_adapter,start,stop)

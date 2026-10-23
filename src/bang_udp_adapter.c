// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Thin Celix adapter. The actual binary codec and bounded UDP socket belong
// exclusively to mcc0nnell/6lowstick native/bang (lowbang_udp).
#define _POSIX_C_SOURCE 200809L
#include "tronix_bang_udp.h"
#include "lowbang_udp.h"
#include <celix_bundle_activator.h>
#include <celix_bundle_context.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    long registration_id;
    lowbang_udp* endpoint;
    celix_bundle_context_t* ctx;
    tronix_bang_udp_service_t service;
} bang_adapter;
static uint16_t local_port(void* handle) {
    bang_adapter* a=handle;
    return a && a->endpoint ? lowbang_udp_local_port(a->endpoint) : 0;
}
static int send_event(void* handle,const bang_event* f,uint16_t target) {
    bang_adapter* a=handle;
    return a && a->endpoint &&
      lowbang_udp_send_loopback(a->endpoint,target,f)==BANG_OK;
}
typedef struct {
    const bang_event* frame;
    uint64_t now_ms;
    tronix_bang_admission_t* result;
    bang_status status;
    bool invoked;
} accept_state;
static void invoke_stream(void* handle,void* raw) {
    accept_state* a=handle;
    tronix_bang_stream_service_t* service=raw;
    if(!service || !service->accept)return;
    a->invoked=true;
    a->status=service->accept(service->handle,a->frame,a->now_ms,a->result);
}
static bang_status receive_to_stream(void* handle,uint32_t timeout,
                                     tronix_bang_admission_t* out) {
    bang_adapter* a=handle;
    if(!a||!a->endpoint||!out)return BANG_INVALID;
    memset(out,0,sizeof *out);
    // This only decodes an UNTRUSTED candidate. The pinned publisher key,
    // TTL and replay checks must run in the signed Celix stream service.
    bang_event candidate={0};
    bang_status st=lowbang_udp_recv_untrusted(a->endpoint,timeout,&candidate);
    if(st!=BANG_OK)return st;
    struct timespec now={0};
    if(clock_gettime(CLOCK_REALTIME,&now)!=0)return BANG_INVALID;
    uint64_t observed=(uint64_t)now.tv_sec*1000+now.tv_nsec/1000000;
    accept_state state={.frame=&candidate,.now_ms=observed,
                        .result=out,.status=BANG_UNAUTHORIZED,.invoked=false};
    celix_service_use_options_t use={0};
    use.filter.serviceName=TRONIX_BANG_STREAM_SERVICE;
    use.filter.versionRange="[1.0.0,2.0.0)";
    use.callbackHandle=&state;
    use.use=invoke_stream;
    (void)celix_bundleContext_useServiceWithOptions(a->ctx,&use);
    return state.invoked ? state.status:BANG_UNAUTHORIZED;
}
static celix_status_t start(bang_adapter* a,celix_bundle_context_t* ctx) {
    memset(a,0,sizeof *a);
    a->registration_id=-1;
    const char* enabled=getenv("TRONIX_BANG_UDP_ENABLE_FIXTURE");
    const char* requested=getenv("TRONIX_BANG_UDP_BIND_PORT");
    if(!enabled || strcmp(enabled,"1") || !requested || !requested[0])
        return CELIX_SERVICE_EXCEPTION;
    errno=0;
    char* end=NULL;
    unsigned long port=strtoul(requested,&end,10);
    if(errno || !end || *end || port>65535 || (port && port<1024))
        return CELIX_SERVICE_EXCEPTION;
    a->endpoint=lowbang_udp_open_loopback((uint16_t)port);
    if(!a->endpoint)return CELIX_SERVICE_EXCEPTION;
    a->ctx=ctx;
    a->service.handle=a;
    a->service.local_port=local_port;
    a->service.send_event=send_event;
    a->service.receive_to_stream=receive_to_stream;
    celix_service_registration_options_t opt=CELIX_EMPTY_SERVICE_REGISTRATION_OPTIONS;
    opt.svc=&a->service;
    opt.serviceName=TRONIX_BANG_UDP_SERVICE;
    opt.serviceVersion=TRONIX_BANG_UDP_ABI_VERSION;
    a->registration_id=celix_bundleContext_registerServiceWithOptions(ctx,&opt);
    if(a->registration_id>=0)return CELIX_SUCCESS;
    lowbang_udp_close(a->endpoint);a->endpoint=NULL;
    return CELIX_SERVICE_EXCEPTION;
}
static celix_status_t stop(bang_adapter* a,celix_bundle_context_t* ctx) {
    if(a->registration_id>=0)celix_bundleContext_unregisterService(ctx,a->registration_id);
    lowbang_udp_close(a->endpoint);
    a->endpoint=NULL;
    return CELIX_SUCCESS;
}
CELIX_GEN_BUNDLE_ACTIVATOR(bang_adapter,start,stop)

// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Explicitly opt-in, IPv6 loopback-only UDP lab service. No RF, gateways,
// background threads, remote hosts, or old integer job ABI.
#define _GNU_SOURCE
#include "tronix_bang_udp.h"
#include "tronix_bang_wire.h"
#include <celix_bundle_activator.h>
#include <celix_bundle_context.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    long registration_id;
    int fd;
    uint16_t port;
    celix_bundle_context_t* context;
    tronix_bang_udp_service_t service;
} bang_udp_adapter;

static uint16_t own_port(void* handle) {
    return ((bang_udp_adapter*)handle)->port;
}
static int send_event(void* handle,const bang_event* event,uint16_t target) {
    bang_udp_adapter* adapter=handle;
    if(!adapter||!event||target<1024||adapter->fd<0)return 0;
    uint8_t bytes[TRONIX_BANG_WIRE_MAX]={0};
    size_t n=0;
    if(!tronix_bang_wire_encode(event,bytes,sizeof bytes,&n))return 0;
    struct sockaddr_in6 dst={0};
    dst.sin6_family=AF_INET6;
    dst.sin6_addr=in6addr_loopback; // Destination is not caller-configurable.
    dst.sin6_port=htons(target);
    ssize_t sent=sendto(adapter->fd,bytes,n,MSG_NOSIGNAL,
                         (const struct sockaddr*)&dst,sizeof dst);
    return sent==(ssize_t)n;
}
typedef struct {
    const bang_event* event;
    uint64_t now_ms;
    tronix_bang_admission_t* admission;
    bang_status result;
    bool invoked;
} accept_state;
static void admit_signed_frame(void* handle,void* raw_service) {
    accept_state* state=handle;
    tronix_bang_stream_service_t* stream=raw_service;
    if(!stream||!stream->accept)return;
    state->invoked=true;
    state->result=stream->accept(stream->handle,state->event,
                                  state->now_ms,state->admission);
}
static bang_status receive_to_stream(void* handle,uint32_t timeout,
                                     tronix_bang_admission_t* admission) {
    bang_udp_adapter* adapter=handle;
    if(!adapter||adapter->fd<0||!admission||timeout>1500)return BANG_INVALID;
    memset(admission,0,sizeof *admission);
    struct pollfd pfd={.fd=adapter->fd,.events=POLLIN};
    int ready=poll(&pfd,1,(int)timeout);
    if(ready==0)return BANG_NOT_READY;
    if(ready<0 || (pfd.revents & (POLLERR|POLLHUP|POLLNVAL)) ||
       !(pfd.revents & POLLIN))return BANG_INVALID;
    uint8_t raw[TRONIX_BANG_WIRE_MAX]={0};
    struct sockaddr_in6 remote={0};
    socklen_t remote_len=sizeof remote;
    // MSG_TRUNC makes malformed oversized datagrams fail closed rather than
    // validating an attacker-controlled, silently truncated prefix.
    ssize_t n=recvfrom(adapter->fd,raw,sizeof raw,MSG_TRUNC,
                       (struct sockaddr*)&remote,&remote_len);
    if(n<=0 || n>(ssize_t)sizeof raw || remote_len!=sizeof remote ||
       remote.sin6_family!=AF_INET6 ||
       !IN6_IS_ADDR_LOOPBACK(&remote.sin6_addr))return BANG_UNAUTHORIZED;
    bang_event event={0};
    if(!tronix_bang_wire_decode(raw,(size_t)n,&event))return BANG_INVALID;
    // Use a clock read at actual datagram reception, not a potentially
    // stale caller-provided timestamp from before the bounded poll.
    struct timespec clock_now={0};
    if(clock_gettime(CLOCK_REALTIME,&clock_now)!=0)return BANG_INVALID;
    uint64_t receipt_ms=(uint64_t)clock_now.tv_sec*1000 +
                       (uint64_t)clock_now.tv_nsec/1000000;
    accept_state state={.event=&event,.now_ms=receipt_ms,
                        .admission=admission,.result=BANG_UNAUTHORIZED,
                        .invoked=false};
    celix_service_use_options_t use={0};
    use.filter.serviceName=TRONIX_BANG_STREAM_SERVICE;
    use.filter.versionRange="[1.0.0,2.0.0)";
    use.callbackHandle=&state;
    use.use=admit_signed_frame;
    (void)celix_bundleContext_useServiceWithOptions(adapter->context,&use);
    return state.invoked ? state.result : BANG_UNAUTHORIZED;
}
static celix_status_t start(bang_udp_adapter* adapter,celix_bundle_context_t* ctx) {
    memset(adapter,0,sizeof(*adapter));
    adapter->registration_id=-1;
    adapter->fd=-1;
    const char* enabled=getenv("TRONIX_BANG_UDP_ENABLE_FIXTURE");
    const char* porttext=getenv("TRONIX_BANG_UDP_BIND_PORT");
    if(!enabled || strcmp(enabled,"1")!=0 || !porttext || !porttext[0])
        return CELIX_SERVICE_EXCEPTION;
    errno=0;
    char* end=NULL;
    unsigned long requested=strtoul(porttext,&end,10);
    if(errno || !end || *end || requested>65535 ||
       (requested!=0 && requested<1024))return CELIX_SERVICE_EXCEPTION;
    int fd=socket(AF_INET6,SOCK_DGRAM|SOCK_CLOEXEC,0);
    if(fd<0)return CELIX_SERVICE_EXCEPTION;
    int v6only=1;
    if(setsockopt(fd,IPPROTO_IPV6,IPV6_V6ONLY,&v6only,sizeof v6only)!=0) {
        close(fd);return CELIX_SERVICE_EXCEPTION;
    }
    struct sockaddr_in6 addr={0};
    addr.sin6_family=AF_INET6;
    addr.sin6_addr=in6addr_loopback;
    addr.sin6_port=htons((uint16_t)requested);
    if(bind(fd,(const struct sockaddr*)&addr,sizeof addr)!=0) {
        close(fd);return CELIX_SERVICE_EXCEPTION;
    }
    socklen_t slen=sizeof addr;
    if(getsockname(fd,(struct sockaddr*)&addr,&slen)!=0 ||
       slen!=sizeof addr || ntohs(addr.sin6_port)==0) {
        close(fd);return CELIX_SERVICE_EXCEPTION;
    }
    adapter->fd=fd;
    adapter->port=ntohs(addr.sin6_port);
    adapter->context=ctx;
    adapter->service.handle=adapter;
    adapter->service.local_port=own_port;
    adapter->service.send_event=send_event;
    adapter->service.receive_to_stream=receive_to_stream;
    celix_service_registration_options_t opt=CELIX_EMPTY_SERVICE_REGISTRATION_OPTIONS;
    opt.svc=&adapter->service;
    opt.serviceName=TRONIX_BANG_UDP_SERVICE;
    opt.serviceVersion=TRONIX_BANG_UDP_ABI_VERSION;
    adapter->registration_id=celix_bundleContext_registerServiceWithOptions(ctx,&opt);
    if(adapter->registration_id<0) {
        close(fd);adapter->fd=-1;
        return CELIX_SERVICE_EXCEPTION;
    }
    return CELIX_SUCCESS;
}
static celix_status_t stop(bang_udp_adapter* adapter,celix_bundle_context_t* ctx) {
    if(adapter->registration_id>=0)
        celix_bundleContext_unregisterService(ctx,adapter->registration_id);
    if(adapter->fd>=0)close(adapter->fd);
    adapter->fd=-1;
    return CELIX_SUCCESS;
}
CELIX_GEN_BUNDLE_ACTIVATOR(bang_udp_adapter,start,stop)

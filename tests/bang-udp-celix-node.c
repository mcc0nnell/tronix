// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Two independently launched C/Celix nodes, carrying real binary signed events
// over IPv6 loopback UDP. Never production keys or an actual Thread/RF claim.
#define _GNU_SOURCE
#include "tronix_bang_udp.h"
#include "lowbang_wire.h"
#include <celix_framework_factory.h>
#include <celix_framework.h>
#include <celix_bundle_context.h>
#include <celix_properties.h>
#include <celix_constants.h>
#include <openssl/evp.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define SESSION "abababababababababababababababab"
static const uint8_t fixture_secret[32]={1};
static uint64_t clock_ms(void) {
    struct timespec ts={0};
    if(clock_gettime(CLOCK_REALTIME,&ts)!=0)return 0;
    return (uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000;
}
static int public_key(uint8_t out[32]) {
    EVP_PKEY* p=EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519,NULL,
                                      fixture_secret,sizeof fixture_secret);
    if(!p)return 0;
    size_t n=32;
    int yes=EVP_PKEY_get_raw_public_key(p,out,&n)==1 && n==32;
    EVP_PKEY_free(p);
    return yes;
}
typedef struct {
    int action;
    uint16_t target,port;
    const bang_event* event;
    uint32_t id1,id2;
    uint64_t now;
    bang_status status;
    tronix_bang_admission_t admission;
    bang_event read1,read2;
    bool invoked;
} call_state;
static void stream_use(void* raw,void* service) {
    call_state* st=raw;
    tronix_bang_stream_service_t* s=service;
    if(!s)return;
    st->invoked=true;
    switch(st->action) {
      case 1: {
        uint8_t pub[32];
        st->status=public_key(pub) ?
            s->pin(s->handle,"main-stage",pub) : BANG_CRYPTO_ERROR;
        break;
      }
      case 2:
        st->status=s->enable(s->handle);
        break;
      case 3:
        st->status=s->subscribe(s->handle,"main-stage",&st->id1);
        break;
      case 4:
        st->status=s->poll(s->handle,st->id1,&st->read1);
        break;
      case 5:
        st->status=s->subscribe(s->handle,"main-stage",&st->id2);
        break;
      case 6:
        st->status=s->poll(s->handle,st->id2,&st->read2);
        break;
      default:
        st->status=BANG_INVALID;
    }
}
static void udp_use(void* raw,void* service) {
    call_state* st=raw;
    tronix_bang_udp_service_t* s=service;
    if(!s)return;
    st->invoked=true;
    if(st->action==10) {
      st->port=s->local_port(s->handle);
      st->status=st->port ? BANG_OK : BANG_INVALID;
    }else if(st->action==11) {
      st->status=s->send_event(s->handle,st->event,st->target) ?
                       BANG_OK : BANG_INVALID;
    }else if(st->action==12) {
      st->status=s->receive_to_stream(s->handle,1000,&st->admission);
    }else st->status=BANG_INVALID;
}
static bang_status run(celix_bundle_context_t* ctx,bool udp,call_state* st) {
    st->invoked=false;
    celix_service_use_options_t opts={0};
    opts.filter.serviceName=udp?TRONIX_BANG_UDP_SERVICE:
                              TRONIX_BANG_STREAM_SERVICE;
    opts.filter.versionRange="[1.0.0,2.0.0)";
    opts.callbackHandle=st;
    opts.use=udp?udp_use:stream_use;
    (void)celix_bundleContext_useServiceWithOptions(ctx,&opts);
    return st->invoked?st->status:BANG_UNAUTHORIZED;
}
static int receive_events(celix_bundle_context_t* ctx,call_state* st) {
    st->action=1;
    if(run(ctx,false,st)!=BANG_OK)return 1;
    st->action=2;
    if(run(ctx,false,st)!=BANG_OK)return 1;
    st->action=3;
    if(run(ctx,false,st)!=BANG_OK)return 1;
    const uint32_t id1=st->id1;
    st->action=5;
    if(run(ctx,false,st)!=BANG_OK)return 1;
    const uint32_t id2=st->id2;
    st->action=10;
    if(run(ctx,true,st)!=BANG_OK)return 1;
    printf("READY %u\n",(unsigned)st->port);
    fflush(stdout);
    const bang_status expected[]={BANG_OK,BANG_OK,BANG_REPLAY,BANG_UNAUTHORIZED,BANG_INVALID,BANG_INVALID};
    for(int i=0;i<6;i++) {
        bang_status got=BANG_NOT_READY;
        for(int attempt=0;attempt<12 && got==BANG_NOT_READY;attempt++) {
            st->action=12;
            got=run(ctx,true,st);
        }
        if(got!=expected[i]) {
            fprintf(stderr,"frame=%d expected=%s got=%s\n",i,
                    bang_status_string(expected[i]),bang_status_string(got));
            return 1;
        }
        if(i==0 && (st->admission.queued_per_subscriber!=0 ||
                    st->admission.waiting_for!=1))return 1;
        if(i==1 && (st->admission.queued_per_subscriber!=2 ||
                    st->admission.waiting_for!=0))return 1;
    }
    for(int seq=1;seq<=2;seq++) {
        st->id1=id1;st->action=4;
        if(run(ctx,false,st)!=BANG_OK||st->read1.seq!=(uint64_t)seq)return 1;
        st->id2=id2;st->action=6;
        if(run(ctx,false,st)!=BANG_OK||st->read2.seq!=(uint64_t)seq)return 1;
        if(strcmp(st->read1.text,st->read2.text)!=0 ||
           strcmp(st->read1.text,seq==1?"Gates open":"[APPLAUSE]")!=0)return 1;
    }
    printf("PASS: two native Celix processes tested 6 binary UDP datagrams; authenticated, ordered, fanned out, rejected replay, forgery, oversized and malformed\n");
    fflush(stdout);
    return 0;
}
static int publish_events(celix_bundle_context_t* ctx,uint16_t target,call_state* st) {
    bang_source* source=bang_source_new("main-stage","en",SESSION,fixture_secret);
    if(!source)return 1;
    bang_event first={0},second={0},tampered={0};
    uint64_t now=clock_ms();
    int good=now>0 &&
        bang_source_emit(source,BANG_CAPTION,"Gates open",now,15000,&first)==BANG_OK &&
        bang_source_emit(source,BANG_SOUND,"[APPLAUSE]",now+1,15000,&second)==BANG_OK;
    if(good) {
        tampered=first;tampered.text[0]='X';
        const bang_event* frames[]={&second,&first,&first,&tampered};
        for(int i=0;i<4;i++) {
            st->action=11;st->target=target;st->event=frames[i];
            if(run(ctx,true,st)!=BANG_OK) {good=0;break;}
            usleep(25000);
        }
    }
    if(good) {
        // Explicit test-only hostile send: never exposed through the
        // constrained typed Celix UDP send_event API.
        int fd=socket(AF_INET6,SOCK_DGRAM|SOCK_CLOEXEC,0);
        if(fd<0)good=0;
        else {
            struct sockaddr_in6 dst={0};
            dst.sin6_family=AF_INET6;dst.sin6_addr=in6addr_loopback;
            dst.sin6_port=htons(target);
            uint8_t oversized[BANG_WIRE_MAX+1]={0};
            uint8_t invalid_magic[BANG_WIRE_HEADER]={0};
            good=sendto(fd,oversized,sizeof oversized,0,
                   (const struct sockaddr*)&dst,sizeof dst)==(ssize_t)sizeof oversized;
            usleep(25000);
            if(good)good=sendto(fd,invalid_magic,sizeof invalid_magic,0,
                   (const struct sockaddr*)&dst,sizeof dst)==(ssize_t)sizeof invalid_magic;
            close(fd);
        }
    }
    bang_source_free(source);
    if(good)puts("PASS: Celix UDP publisher sent signed frames; hostile test-only oversized and malformed frames were injected");
    return good?0:1;
}
int main(int argc,char** argv) {
    if(argc!=5 || (strcmp(argv[1],"rx")&&strcmp(argv[1],"tx")))return 2;
    char* end=NULL;
    unsigned long target=strtoul(argv[2],&end,10);
    if(!end||*end||target>65535 ||
       (!strcmp(argv[1],"tx")&&target<1024))return 2;
    if(argv[3][0]!='/'||argv[4][0]!='/')return 2;
    const char* cache=getenv("BANG_CELIX_CACHE");
    if(!cache||cache[0]!='/')return 2;
    setenv("TRONIX_BANG_STREAM_ENABLE_FIXTURE","1",1);
    if(!getenv("BANG_UDP_SKIP_ENABLE"))
        setenv("TRONIX_BANG_UDP_ENABLE_FIXTURE","1",1);
    setenv("TRONIX_BANG_UDP_BIND_PORT","0",1);
    celix_properties_t* props=celix_properties_create();
    celix_properties_set(props,CELIX_FRAMEWORK_CACHE_DIR,cache);
    celix_properties_setBool(props,CELIX_FRAMEWORK_CLEAN_CACHE_DIR_ON_CREATE,true);
    celix_framework_t* fw=celix_frameworkFactory_createFramework(props);
    if(!fw)return 3;
    celix_bundle_context_t* ctx=celix_framework_getFrameworkContext(fw);
    int result=0;
    if(celix_bundleContext_installBundle(ctx,argv[3],true)<0 ||
       celix_bundleContext_installBundle(ctx,argv[4],true)<0)result=3;
    if(result==0) {
        call_state st={0};
        result=strcmp(argv[1],"rx")==0?receive_events(ctx,&st):
                  publish_events(ctx,(uint16_t)target,&st);
    }
    celix_frameworkFactory_destroyFramework(fw);
    return result;
}

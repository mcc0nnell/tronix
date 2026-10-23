// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Local, synthetic fixture: generates signed events and independently verifies
// received OpenThread CLI hex-line payloads with the real C11 lib6lowBang.
#include "tronix_bang_wire.h"
#include "tronix_bang_stream.h"
#include <openssl/evp.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define SESSION "abababababababababababababababab"
static const uint8_t fixture_secret[32]={1}; // NEVER production credentials.
static uint64_t now_ms(void) {
    struct timespec ts={0};
    if(clock_gettime(CLOCK_REALTIME,&ts)!=0) return 0;
    return (uint64_t)ts.tv_sec*1000 + (uint64_t)ts.tv_nsec/1000000;
}
static void print_hex(const char *tag,const uint8_t *bytes,size_t n) {
    printf("%s=",tag);
    for(size_t i=0;i<n;i++)printf("%02x",bytes[i]);
    puts("");
}
static int from_hex(const char *text,uint8_t *out,size_t *count) {
    if(!text) return 0;
    size_t n=strlen(text);
    if(n==0 || n&1 || n>TRONIX_BANG_WIRE_MAX*2)return 0;
    for(size_t i=0;i<n/2;i++){
        int digits[2]={-1,-1};
        for(int j=0;j<2;j++){
            char c=text[2*i+j];
            if(c>='0'&&c<='9')digits[j]=c-'0';
            else if(c>='a'&&c<='f')digits[j]=c-'a'+10;
        }
        if(digits[0]<0||digits[1]<0)return 0;
        out[i]=(uint8_t)(digits[0]*16+digits[1]);
    }
    *count=n/2;return 1;
}
static int make_publisher_key(uint8_t output[32]) {
    EVP_PKEY *key=EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519,NULL,
        fixture_secret,sizeof fixture_secret);
    if(!key)return 0;
    size_t n=32;
    int ok=EVP_PKEY_get_raw_public_key(key,output,&n)==1 && n==32;
    EVP_PKEY_free(key);
    return ok;
}
static int emit(void) {
    bang_source *source=bang_source_new("main-stage","en",SESSION,fixture_secret);
    if(!source)return 1;
    bang_event first={0},second={0};
    uint64_t now=now_ms();
    int ok=now &&
        bang_source_emit(source,BANG_CAPTION,"Gates open",now,15000,&first)==BANG_OK &&
        bang_source_emit(source,BANG_SOUND,"[APPLAUSE]",now+1,15000,&second)==BANG_OK;
    if(ok) {
        uint8_t a[TRONIX_BANG_WIRE_MAX],b[TRONIX_BANG_WIRE_MAX];
        size_t na=0,nb=0;
        ok=tronix_bang_wire_encode(&first,a,sizeof a,&na) &&
           tronix_bang_wire_encode(&second,b,sizeof b,&nb);
        if(ok) {
            print_hex("FRAME1",a,na);
            print_hex("FRAME2",b,nb);
            printf("WIRE_BYTES=%zu,%zu\n",na,nb);
        }
    }
    bang_source_free(source);return ok?0:1;
}
static int verify_on_receiver(const char *arrived2,const char *arrived1) {
    uint8_t raw2[TRONIX_BANG_WIRE_MAX],raw1[TRONIX_BANG_WIRE_MAX];
    size_t n2=0,n1=0;bang_event frame2={0},frame1={0};
    if(!from_hex(arrived2,raw2,&n2)||!from_hex(arrived1,raw1,&n1) ||
       !tronix_bang_wire_decode(raw2,n2,&frame2) ||
       !tronix_bang_wire_decode(raw1,n1,&frame1))return 1;
    uint8_t pub[32]={0};
    if(!make_publisher_key(pub))return 1;
    tronix_bang_hub *hub=tronix_bang_hub_new();
    if(!hub)return 1;
    int ok=tronix_bang_hub_pin(hub,"main-stage",pub)==BANG_OK &&
           tronix_bang_hub_enable(hub)==BANG_OK;
    uint32_t a=0,b=0;
    if(ok)ok=tronix_bang_hub_subscribe(hub,"main-stage",&a)==BANG_OK &&
             tronix_bang_hub_subscribe(hub,"main-stage",&b)==BANG_OK;
    tronix_bang_admission_t admission={0};
    uint64_t now=now_ms();
    if(ok)ok=tronix_bang_hub_accept(hub,&frame2,now,&admission)==BANG_OK &&
        admission.queued_per_subscriber==0 && admission.waiting_for==1;
    now=now_ms();
    if(ok)ok=tronix_bang_hub_accept(hub,&frame1,now,&admission)==BANG_OK &&
        admission.queued_per_subscriber==2 && admission.waiting_for==0;
    if(ok) {
        for(int i=0;i<2;i++) {
            bang_event ea={0},eb={0};
            ok=tronix_bang_hub_poll(hub,a,&ea)==BANG_OK &&
               tronix_bang_hub_poll(hub,b,&eb)==BANG_OK &&
               ea.seq==(uint64_t)(i+1) && eb.seq==ea.seq &&
               !strcmp(ea.text,eb.text) &&
               !strcmp(ea.text,i==0?"Gates open":"[APPLAUSE]");
            if(!ok)break;
        }
    }
    if(ok)ok=tronix_bang_hub_accept(hub,&frame1,now_ms(),&admission)==BANG_REPLAY;
    if(ok) {
        bang_event tampered=frame2;
        tampered.text[0]='X';
        ok=tronix_bang_hub_accept(hub,&tampered,now_ms(),&admission)==BANG_UNAUTHORIZED;
    }
    tronix_bang_hub_free(hub);
    if(ok)puts("PASS: signed OpenThread UDP events admitted through C stream hub; two subscribers, ordering, replay and forgery checks");
    return ok?0:1;
}
static int verify_gap(const char *arrived2) {
    uint8_t raw[TRONIX_BANG_WIRE_MAX];
    size_t n=0;bang_event frame={0};
    if(!from_hex(arrived2,raw,&n)||!tronix_bang_wire_decode(raw,n,&frame))return 1;
    uint8_t pub[32]={0};
    if(!make_publisher_key(pub))return 1;
    tronix_bang_hub *hub=tronix_bang_hub_new();
    if(!hub)return 1;
    int ok=tronix_bang_hub_pin(hub,"main-stage",pub)==BANG_OK &&
           tronix_bang_hub_enable(hub)==BANG_OK;
    uint32_t id=0;
    if(ok)ok=tronix_bang_hub_subscribe(hub,"main-stage",&id)==BANG_OK;
    tronix_bang_admission_t admission={0};
    uint64_t observed=now_ms();
    if(ok)ok=tronix_bang_hub_accept(hub,&frame,observed,&admission)==BANG_OK &&
             admission.queued_per_subscriber==0 && admission.waiting_for==1;
    if(ok) {
        // Caller-driven simulated jitter expiry, not a measured radio outage.
        ok=tronix_bang_hub_flush(hub,"main-stage",SESSION,observed+250,250,&admission)==BANG_OK &&
           admission.gap_reported && admission.missing_from==1 &&
           admission.missing_through==1 && admission.queued_per_subscriber==1;
    }
    if(ok) {
        bang_event released={0};
        ok=tronix_bang_hub_poll(hub,id,&released)==BANG_OK &&
           released.seq==2 && !strcmp(released.text,"[APPLAUSE]");
    }
    tronix_bang_hub_free(hub);
    if(ok)puts("PASS: withheld signed caption #1 explicitly reported missing; no invented words");
    return ok?0:1;
}
static int selftest(void) {
    bang_source *s=bang_source_new("main-stage","en",SESSION,fixture_secret);
    if(!s)return 1;
    bang_event f={0},decoded={0};uint8_t wire[TRONIX_BANG_WIRE_MAX];
    size_t n=0;int ok=bang_source_emit(s,BANG_CAPTION,"Test",600000,15000,&f)==BANG_OK &&
        tronix_bang_wire_encode(&f,wire,sizeof wire,&n) &&
        tronix_bang_wire_decode(wire,n,&decoded) &&
        decoded.seq==f.seq && decoded.text_len==f.text_len &&
        !memcmp(decoded.signature,f.signature,64);
    if(ok)ok=!tronix_bang_wire_decode(wire,n-1,&decoded);
    if(ok) {wire[0]^=1;ok=!tronix_bang_wire_decode(wire,n,&decoded);}
    bang_source_free(s);
    if(ok)puts("PASS: bounded C wire codec encode/decode/truncation and magic checks");
    return ok?0:1;
}
int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"emit"))return emit();
    if(argc==4 && !strcmp(argv[1],"verify"))return verify_on_receiver(argv[2],argv[3]);
    if(argc==3 && !strcmp(argv[1],"verify-gap"))return verify_gap(argv[2]);
    if(argc==2 && !strcmp(argv[1],"selftest"))return selftest();
    fputs("Usage: bang-thread-codec {emit|selftest|verify RECEIVED2 RECEIVED1}\n",stderr);
    return 2;
}

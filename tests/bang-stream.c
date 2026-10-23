// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Native signed C11 caption stream tests; no Celix framework or hardware required.
#include "tronix_bang_stream.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#define REQUIRE(expr) do { if (!(expr)) { \
 fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#expr); return 1; \
} } while (0)
#define SESSION "abababababababababababababababab"
#define NOW UINT64_C(600000)
static uint8_t secret[32]={1}, publisher[32];
static int keys(void) {
 EVP_PKEY* p=EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519,NULL,secret,sizeof secret);
 REQUIRE(p); size_t len=sizeof publisher;
 int good=EVP_PKEY_get_raw_public_key(p,publisher,&len);
 EVP_PKEY_free(p);REQUIRE(good==1 && len==sizeof publisher);return 0;
}
static tronix_bang_hub* setup(void) {
 tronix_bang_hub* hub=tronix_bang_hub_new();
 if (!hub) return NULL;
 if (tronix_bang_hub_pin(hub,"stage-a",publisher)!=BANG_OK) {
  tronix_bang_hub_free(hub);return NULL;
 }
 return hub;
}
static bang_source* new_source(void) {
 return bang_source_new("stage-a","en",SESSION,secret);
}
static int disabled_and_pins(void) {
 tronix_bang_hub* hub=setup(); REQUIRE(hub);
 uint32_t id=0;
 REQUIRE(tronix_bang_hub_subscribe(hub,"stage-a",&id)==BANG_DISABLED);
 REQUIRE(tronix_bang_hub_enable(hub)==BANG_OK);
 REQUIRE(tronix_bang_hub_pin(hub,"stage-b",publisher)==BANG_UNAUTHORIZED);
 REQUIRE(tronix_bang_hub_subscribe(hub,"untrusted",&id)==BANG_UNAUTHORIZED);
 REQUIRE(tronix_bang_hub_subscribe(hub,"stage-a",&id)==BANG_OK && id!=0);
 tronix_bang_hub_disable(hub);
 bang_event f={0};
 REQUIRE(tronix_bang_hub_poll(hub,id,&f)==BANG_DISABLED);
 tronix_bang_hub_free(hub);return 0;
}
static int fanout_and_validation(void) {
 tronix_bang_hub* hub=setup();bang_source* src=new_source();
 REQUIRE(hub && src && tronix_bang_hub_enable(hub)==BANG_OK);
 uint32_t a=0,b=0; REQUIRE(tronix_bang_hub_subscribe(hub,"stage-a",&a)==BANG_OK);
 REQUIRE(tronix_bang_hub_subscribe(hub,"stage-a",&b)==BANG_OK);
 bang_event first,second,event;tronix_bang_admission_t result;
 REQUIRE(bang_source_emit(src,BANG_CAPTION,"Gates open",NOW,15000,&first)==BANG_OK);
 REQUIRE(bang_source_emit(src,BANG_SOUND,"[APPLAUSE]",NOW+1,15000,&second)==BANG_OK);
 REQUIRE(tronix_bang_hub_accept(hub,&second,NOW+10,&result)==BANG_OK);
 REQUIRE(result.waiting_for==1 && result.queued_per_subscriber==0);
 REQUIRE(tronix_bang_hub_poll(hub,a,&event)==BANG_NOT_READY);
 bang_event tamper=first;tamper.text[0]='X';
 REQUIRE(tronix_bang_hub_accept(hub,&tamper,NOW+11,&result)==BANG_UNAUTHORIZED);
 REQUIRE(tronix_bang_hub_accept(hub,&first,NOW+12,&result)==BANG_OK);
 REQUIRE(result.waiting_for==0 && result.queued_per_subscriber==2);
 REQUIRE(tronix_bang_hub_accept(hub,&first,NOW+13,&result)==BANG_REPLAY);
 for (int i=0;i<2;i++) {
  REQUIRE(tronix_bang_hub_poll(hub,a,&event)==BANG_OK);
  REQUIRE(event.seq==(uint64_t)(i+1));
  REQUIRE(tronix_bang_hub_poll(hub,b,&event)==BANG_OK);
  REQUIRE(event.seq==(uint64_t)(i+1));
 }
 REQUIRE(tronix_bang_hub_poll(hub,b,&event)==BANG_NOT_READY);
 REQUIRE(tronix_bang_hub_unsubscribe(hub,a)==BANG_OK);
 REQUIRE(tronix_bang_hub_unsubscribe(hub,a)==BANG_UNAUTHORIZED);
 // Subscribing one channel twice does not revoke the surviving participant.
 bang_event third;
 REQUIRE(bang_source_emit(src,BANG_CAPTION,"Next",NOW+3,15000,&third)==BANG_OK);
 REQUIRE(tronix_bang_hub_accept(hub,&third,NOW+14,&result)==BANG_OK);
 REQUIRE(tronix_bang_hub_poll(hub,b,&event)==BANG_OK && event.seq==3);
 REQUIRE(tronix_bang_hub_unsubscribe(hub,b)==BANG_OK);
 REQUIRE(tronix_bang_hub_accept(hub,&third,NOW+15,&result)==BANG_UNAUTHORIZED);
 tronix_bang_hub_free(hub);bang_source_free(src);return 0;
}
static int gap_behavior(void) {
 tronix_bang_hub* hub=setup();bang_source* src=new_source();REQUIRE(hub && src);
 REQUIRE(tronix_bang_hub_enable(hub)==BANG_OK);
 uint32_t id=0;REQUIRE(tronix_bang_hub_subscribe(hub,"stage-a",&id)==BANG_OK);
 bang_event first,second,out;tronix_bang_admission_t r;
 REQUIRE(bang_source_emit(src,BANG_CAPTION,"missing",NOW,15000,&first)==BANG_OK);
 REQUIRE(bang_source_emit(src,BANG_CAPTION,"delivered",NOW+1,15000,&second)==BANG_OK);
 REQUIRE(tronix_bang_hub_accept(hub,&second,NOW+20,&r)==BANG_OK);
 REQUIRE(tronix_bang_hub_flush(hub,"stage-a",SESSION,NOW+269,250,&r)==BANG_NOT_READY);
 REQUIRE(tronix_bang_hub_flush(hub,"stage-a",SESSION,NOW+270,250,&r)==BANG_OK);
 REQUIRE(r.gap_reported && r.missing_from==1 && r.missing_through==1 &&
         r.queued_per_subscriber==1);
 REQUIRE(tronix_bang_hub_poll(hub,id,&out)==BANG_OK);
 REQUIRE(out.seq==2 && strcmp(out.text,"delivered")==0);
 REQUIRE(tronix_bang_hub_accept(hub,&first,NOW+271,&r)==BANG_REPLAY);
 tronix_bang_hub_free(hub);bang_source_free(src);return 0;
}
static int bounded_backpressure(void) {
 tronix_bang_hub* hub=setup();bang_source* src=new_source();REQUIRE(hub && src);
 REQUIRE(tronix_bang_hub_enable(hub)==BANG_OK);
 uint32_t id=0;REQUIRE(tronix_bang_hub_subscribe(hub,"stage-a",&id)==BANG_OK);
 bang_event frame,out;tronix_bang_admission_t receipt;
 for (unsigned i=0;i<33;i++) {
  REQUIRE(bang_source_emit(src,BANG_CAPTION,"synthetic",NOW+i,15000,&frame)==BANG_OK);
  REQUIRE(tronix_bang_hub_accept(hub,&frame,NOW+i,&receipt)==BANG_OK);
  REQUIRE(receipt.queued_per_subscriber==1);
 }
 REQUIRE(bang_source_emit(src,BANG_CAPTION,"blocked",NOW+34,15000,&frame)==BANG_OK);
 REQUIRE(tronix_bang_hub_accept(hub,&frame,NOW+34,&receipt)==BANG_CAPACITY);
 REQUIRE(tronix_bang_hub_poll(hub,id,&out)==BANG_OK && out.seq==1);
 REQUIRE(tronix_bang_hub_accept(hub,&frame,NOW+35,&receipt)==BANG_OK);
 REQUIRE(tronix_bang_hub_poll(hub,id,&out)==BANG_OK && out.seq==2);
 tronix_bang_hub_free(hub);bang_source_free(src);return 0;
}
static int subscriber_bounds(void) {
 tronix_bang_hub* hub=setup(); REQUIRE(hub);
 REQUIRE(tronix_bang_hub_enable(hub)==BANG_OK);
 uint32_t ids[TRONIX_BANG_MAX_SUBSCRIBERS];
 for (size_t i=0;i<TRONIX_BANG_MAX_SUBSCRIBERS;i++)
  REQUIRE(tronix_bang_hub_subscribe(hub,"stage-a",&ids[i])==BANG_OK);
 uint32_t extra=0;
 REQUIRE(tronix_bang_hub_subscribe(hub,"stage-a",&extra)==BANG_CAPACITY);
 REQUIRE(tronix_bang_hub_unsubscribe(hub,ids[0])==BANG_OK);
 REQUIRE(tronix_bang_hub_subscribe(hub,"stage-a",&extra)==BANG_OK);
 REQUIRE(extra!=ids[0]); // Prevent stale subscription capability reuse.
 tronix_bang_hub_free(hub);return 0;
}
static int rejected_malformed_channel(void) {
 tronix_bang_hub* hub=setup();bang_source* src=new_source();REQUIRE(hub&&src);
 REQUIRE(tronix_bang_hub_enable(hub)==BANG_OK);
 uint32_t id=0;REQUIRE(tronix_bang_hub_subscribe(hub,"stage-a",&id)==BANG_OK);
 bang_event frame;tronix_bang_admission_t r;
 REQUIRE(bang_source_emit(src,BANG_CAPTION,"valid",NOW,1000,&frame)==BANG_OK);
 memset(frame.channel,'x',sizeof frame.channel); // No terminating NUL.
 REQUIRE(tronix_bang_hub_accept(hub,&frame,NOW,&r)==BANG_UNAUTHORIZED);
 tronix_bang_hub_free(hub);bang_source_free(src);return 0;
}
int main(void) {
 REQUIRE(keys()==0);
 struct {const char* name;int (*fn)(void);} cases[]={
  {"disabled and pinned keys",disabled_and_pins},
  {"authenticity and fanout",fanout_and_validation},
  {"gap reporting",gap_behavior},
  {"explicit backpressure",bounded_backpressure},
  {"bounded subscribers",subscriber_bounds},
  {"malformed channel length",rejected_malformed_channel}
 };
 for (size_t i=0;i<sizeof cases/sizeof cases[0];i++) {
  REQUIRE(cases[i].fn()==0);
  printf("PASS: %s\n",cases[i].name);
 }
 puts("6 native C service test groups passed. No Celix runtime or RF tested.");
 return 0;
}

// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Fixed, bounded demo codec, not a single 802.15.4 MAC frame.
#include "tronix_bang_wire.h"
#include <string.h>
static size_t bounded_len(const char *s, size_t max) {
    size_t n=0;
    while(n<max && s[n]) n++;
    return n;
}
static int hex(char c) {
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    return -1;
}
static void put_u64(uint8_t *b,uint64_t v) {
    for (int i=7;i>=0;i--) {b[i]=(uint8_t)(v & 255);v >>= 8;}
}
static uint64_t get_u64(const uint8_t *b) {
    uint64_t v=0;
    for(int i=0;i<8;i++) v=(v<<8)|b[i];
    return v;
}
int tronix_bang_wire_encode(const bang_event *e,uint8_t *out,size_t cap,size_t *length) {
    if(!e||!out||!length) return 0;
    size_t ch=bounded_len(e->channel,BANG_MAX_CHANNEL+1);
    size_t lg=bounded_len(e->language,BANG_MAX_LANGUAGE+1);
    size_t tx=bounded_len(e->text,BANG_MAX_TEXT+1);
    if(e->v!=1||(e->kind!=BANG_CAPTION&&e->kind!=BANG_SOUND)||
       ch==0||ch>BANG_MAX_CHANNEL||lg<2||lg>BANG_MAX_LANGUAGE||
       tx==0||tx>BANG_MAX_TEXT||e->text_len!=tx ||
       bounded_len(e->session,33)!=32) return 0;
    const size_t size=TRONIX_BANG_WIRE_HEADER+ch+lg+tx;
    if(size>cap || size>TRONIX_BANG_WIRE_MAX)return 0;
    uint8_t b[TRONIX_BANG_WIRE_MAX]={0};
    b[0]='B';b[1]='G';b[2]=1;b[3]=(uint8_t)e->kind;
    b[4]=(uint8_t)ch;b[5]=(uint8_t)lg;b[6]=(uint8_t)tx;
    for(size_t i=0;i<16;i++) {
        int hi=hex(e->session[2*i]),lo=hex(e->session[2*i+1]);
        if(hi<0||lo<0)return 0;
        b[7+i]=(uint8_t)((hi<<4)|lo);
    }
    put_u64(b+23,e->seq);
    put_u64(b+31,e->captured_at_ms);
    put_u64(b+39,e->expires_at_ms);
    memcpy(b+47,e->signature,64);
    memcpy(b+TRONIX_BANG_WIRE_HEADER,e->channel,ch);
    memcpy(b+TRONIX_BANG_WIRE_HEADER+ch,e->language,lg);
    memcpy(b+TRONIX_BANG_WIRE_HEADER+ch+lg,e->text,tx);
    memcpy(out,b,size);
    *length=size;
    return 1;
}
int tronix_bang_wire_decode(const uint8_t *bytes,size_t len,bang_event *e) {
    if(!bytes||!e||len<TRONIX_BANG_WIRE_HEADER||len>TRONIX_BANG_WIRE_MAX||
       bytes[0]!='B'||bytes[1]!='G'||bytes[2]!=1||
       (bytes[3]!=BANG_CAPTION&&bytes[3]!=BANG_SOUND))return 0;
    const size_t ch=bytes[4],lg=bytes[5],tx=bytes[6];
    if(ch==0||ch>BANG_MAX_CHANNEL||lg<2||lg>BANG_MAX_LANGUAGE||
       tx==0||tx>BANG_MAX_TEXT||
       len!=TRONIX_BANG_WIRE_HEADER+ch+lg+tx)return 0;
    bang_event f={0};
    f.v=1;f.kind=(bang_kind)bytes[3];
    const char digits[]="0123456789abcdef";
    for(size_t i=0;i<16;i++) {
        f.session[2*i]=digits[bytes[7+i]>>4];
        f.session[2*i+1]=digits[bytes[7+i]&15];
    }
    f.seq=get_u64(bytes+23);
    f.captured_at_ms=get_u64(bytes+31);
    f.expires_at_ms=get_u64(bytes+39);
    memcpy(f.signature,bytes+47,64);
    memcpy(f.channel,bytes+TRONIX_BANG_WIRE_HEADER,ch);
    memcpy(f.language,bytes+TRONIX_BANG_WIRE_HEADER+ch,lg);
    memcpy(f.text,bytes+TRONIX_BANG_WIRE_HEADER+ch+lg,tx);
    f.text_len=tx;
    // No embedded NUL may mutate canonical signed fields on decode.
    if(memchr(f.channel,0,ch)||memchr(f.language,0,lg)||memchr(f.text,0,tx))return 0;
    *e=f;
    return 1; // Cryptographic authentication occurs ONLY in lib6lowBang.
}

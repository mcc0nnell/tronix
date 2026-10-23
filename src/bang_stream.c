// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Bounded, process-local publisher-authenticated caption fanout.
// No sockets, queues outside this process, radio or untrusted plugin sandbox.
#include "tronix_bang_stream.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
typedef struct {
    bool active;
    uint32_t id;
    char channel[BANG_MAX_CHANNEL+1];
    size_t head, count;
    bang_event queue[TRONIX_BANG_QUEUE_CAPACITY];
} bang_subscription;
struct tronix_bang_hub {
    pthread_mutex_t lock;
    bang_receiver* receiver;
    bool enabled;
    uint32_t next_id;
    bang_subscription subs[TRONIX_BANG_MAX_SUBSCRIBERS];
};
static bang_subscription* find_sub(tronix_bang_hub* hub, uint32_t id) {
    for (size_t i=0; i<TRONIX_BANG_MAX_SUBSCRIBERS; ++i)
        if (hub->subs[i].active && hub->subs[i].id == id) return &hub->subs[i];
    return NULL;
}
static bool channel_subscribed(tronix_bang_hub* hub, const char* channel) {
    for (size_t i=0; i<TRONIX_BANG_MAX_SUBSCRIBERS; ++i)
        if (hub->subs[i].active && strncmp(hub->subs[i].channel,channel,BANG_MAX_CHANNEL+1)==0)
            return true;
    return false;
}
static bool has_headroom(tronix_bang_hub* hub,const char* channel) {
    bool found=false;
    for (size_t i=0; i<TRONIX_BANG_MAX_SUBSCRIBERS; ++i) {
        const bang_subscription* sub=&hub->subs[i];
        if (!sub->active || strncmp(sub->channel,channel,BANG_MAX_CHANNEL+1)!=0) continue;
        found=true;
        // Reserve a full 32-frame reorder burst BEFORE admission into the
        // 6lowBang receiver. Otherwise verification may consume a frame
        // that cannot be fanned out to all subscribers.
        if (sub->count > TRONIX_BANG_QUEUE_CAPACITY-BANG_WINDOW) return false;
    }
    return found;
}
static void result_from_delivery(tronix_bang_admission_t* result,
                                  const bang_delivery* delivery) {
    result->queued_per_subscriber=delivery->ready_count;
    result->waiting_for=delivery->waiting_for;
    result->gap_reported=delivery->gap_reported;
    result->missing_from=delivery->missing_from;
    result->missing_through=delivery->missing_through;
}
static void distribute(tronix_bang_hub* hub,const char* channel,const bang_delivery* d) {
    // Caller holds lock and has reserved BANG_WINDOW capacity per subscriber.
    for (size_t i=0; i<TRONIX_BANG_MAX_SUBSCRIBERS; ++i) {
        bang_subscription* sub=&hub->subs[i];
        if (!sub->active || strncmp(sub->channel,channel,BANG_MAX_CHANNEL+1)!=0) continue;
        for (size_t j=0; j<d->ready_count; ++j) {
            const size_t at=(sub->head+sub->count)%TRONIX_BANG_QUEUE_CAPACITY;
            sub->queue[at]=d->ready[j];
            sub->count++;
        }
    }
}
tronix_bang_hub* tronix_bang_hub_new(void) {
    tronix_bang_hub* hub=calloc(1,sizeof(*hub));
    if (!hub) return NULL;
    if (pthread_mutex_init(&hub->lock,NULL)!=0) {free(hub); return NULL;}
    hub->receiver=bang_receiver_new();
    if (!hub->receiver) {pthread_mutex_destroy(&hub->lock);free(hub);return NULL;}
    hub->next_id=1;
    return hub;
}
void tronix_bang_hub_free(tronix_bang_hub* hub) {
    if (!hub) return;
    pthread_mutex_lock(&hub->lock);
    bang_receiver_disable(hub->receiver);
    bang_receiver_free(hub->receiver);
    memset(hub->subs,0,sizeof hub->subs);
    pthread_mutex_unlock(&hub->lock);
    pthread_mutex_destroy(&hub->lock);
    free(hub);
}
bang_status tronix_bang_hub_pin(tronix_bang_hub* hub,const char* channel,
                                 const uint8_t ed25519_public_key[32]) {
    if (!hub || !channel || !ed25519_public_key) return BANG_INVALID;
    pthread_mutex_lock(&hub->lock);
    // Publisher roots are fixed before enabling, not replaceable by subscribers.
    const bang_status status=hub->enabled ? BANG_UNAUTHORIZED :
        bang_receiver_provision(hub->receiver,channel,ed25519_public_key);
    pthread_mutex_unlock(&hub->lock);
    return status;
}
bang_status tronix_bang_hub_enable(tronix_bang_hub* hub) {
    if (!hub) return BANG_INVALID;
    pthread_mutex_lock(&hub->lock);
    bang_receiver_enable(hub->receiver);
    hub->enabled=true;
    pthread_mutex_unlock(&hub->lock);
    return BANG_OK;
}
void tronix_bang_hub_disable(tronix_bang_hub* hub) {
    if (!hub) return;
    pthread_mutex_lock(&hub->lock);
    hub->enabled=false;
    bang_receiver_disable(hub->receiver);
    memset(hub->subs,0,sizeof hub->subs);
    pthread_mutex_unlock(&hub->lock);
}
bang_status tronix_bang_hub_subscribe(tronix_bang_hub* hub,
                                      const char* channel,uint32_t* id_out) {
    if (!hub || !channel || !id_out) return BANG_INVALID;
    pthread_mutex_lock(&hub->lock);
    if (!hub->enabled) {pthread_mutex_unlock(&hub->lock);return BANG_DISABLED;}
    if (hub->next_id==0 || hub->next_id==UINT32_MAX) {
        pthread_mutex_unlock(&hub->lock);return BANG_CAPACITY;
    }
    bang_subscription* slot=NULL;
    for (size_t i=0;i<TRONIX_BANG_MAX_SUBSCRIBERS;i++) {
        if (!hub->subs[i].active) {slot=&hub->subs[i]; break;}
    }
    if (!slot) {pthread_mutex_unlock(&hub->lock);return BANG_CAPACITY;}
    // lowbang checks that the channel was explicitly pinned out of band.
    const bang_status st=bang_receiver_subscribe(hub->receiver,channel);
    if (st==BANG_OK) {
        memset(slot,0,sizeof(*slot));
        slot->active=true;slot->id=hub->next_id++;
        strcpy(slot->channel,channel);
        *id_out=slot->id;
    }
    pthread_mutex_unlock(&hub->lock);
    return st;
}
bang_status tronix_bang_hub_unsubscribe(tronix_bang_hub* hub,uint32_t id) {
    if (!hub || id==0) return BANG_INVALID;
    pthread_mutex_lock(&hub->lock);
    bang_subscription* slot=find_sub(hub,id);
    if (!slot) {pthread_mutex_unlock(&hub->lock);return BANG_UNAUTHORIZED;}
    char channel[BANG_MAX_CHANNEL+1];
    strcpy(channel,slot->channel);
    memset(slot,0,sizeof(*slot));
    if (!channel_subscribed(hub,channel))
        bang_receiver_unsubscribe(hub->receiver,channel);
    pthread_mutex_unlock(&hub->lock);
    return BANG_OK;
}
bang_status tronix_bang_hub_accept(tronix_bang_hub* hub,const bang_event* frame,
                                    uint64_t now,tronix_bang_admission_t* result) {
    if (!hub || !frame || !result) return BANG_INVALID;
    memset(result,0,sizeof(*result));
    pthread_mutex_lock(&hub->lock);
    if (!hub->enabled) {pthread_mutex_unlock(&hub->lock);return BANG_DISABLED;}
    if (!channel_subscribed(hub,frame->channel)) {
        pthread_mutex_unlock(&hub->lock);return BANG_UNAUTHORIZED;
    }
    if (!has_headroom(hub,frame->channel)) {
        pthread_mutex_unlock(&hub->lock);return BANG_CAPACITY;
    }
    bang_delivery delivery={0};
    const bang_status st=bang_receiver_receive(hub->receiver,frame,now,&delivery);
    if (st==BANG_OK) {
        distribute(hub,frame->channel,&delivery);
        result_from_delivery(result,&delivery);
    }
    pthread_mutex_unlock(&hub->lock);
    return st;
}
bang_status tronix_bang_hub_flush(tronix_bang_hub* hub,const char* channel,
                                   const char* session,uint64_t now,
                                   uint32_t hold,tronix_bang_admission_t* result) {
    if (!hub || !channel || !session || !result) return BANG_INVALID;
    memset(result,0,sizeof(*result));
    pthread_mutex_lock(&hub->lock);
    if (!hub->enabled) {pthread_mutex_unlock(&hub->lock);return BANG_DISABLED;}
    if (!channel_subscribed(hub,channel)) {
        pthread_mutex_unlock(&hub->lock);return BANG_UNAUTHORIZED;
    }
    if (!has_headroom(hub,channel)) {
        pthread_mutex_unlock(&hub->lock);return BANG_CAPACITY;
    }
    bang_delivery delivery={0};
    const bang_status st=bang_receiver_flush(hub->receiver,channel,session,now,hold,&delivery);
    if (st==BANG_OK) {
        distribute(hub,channel,&delivery);
        result_from_delivery(result,&delivery);
    }
    pthread_mutex_unlock(&hub->lock);
    return st;
}
bang_status tronix_bang_hub_poll(tronix_bang_hub* hub,uint32_t id,
                                 bang_event* event) {
    if (!hub || id==0 || !event) return BANG_INVALID;
    pthread_mutex_lock(&hub->lock);
    if (!hub->enabled) {pthread_mutex_unlock(&hub->lock);return BANG_DISABLED;}
    bang_subscription* sub=find_sub(hub,id);
    if (!sub) {pthread_mutex_unlock(&hub->lock);return BANG_UNAUTHORIZED;}
    if (sub->count==0) {pthread_mutex_unlock(&hub->lock);return BANG_NOT_READY;}
    *event=sub->queue[sub->head];
    memset(&sub->queue[sub->head],0,sizeof(bang_event));
    sub->head=(sub->head+1)%TRONIX_BANG_QUEUE_CAPACITY;
    --sub->count;
    pthread_mutex_unlock(&hub->lock);
    return BANG_OK;
}

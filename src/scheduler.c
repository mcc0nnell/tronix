// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
/* TRONIX experimental cooperative scheduler: fixed-capacity, monotonic deadlines. */
#define _POSIX_C_SOURCE 200809L
#include "tronix_service.h"
#include <celix_bundle_activator.h>
#include <celix_bundle_context.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_JOBS 256
#define MAX_PENDING 128
#define MAX_PER_AGENT 32
#define INTERACTIVE_BURST 4
#define MAX_AGENT_SLOTS MAX_JOBS

typedef struct {
    char name[TRONIX_AGENT_MAX+1];
    uint64_t dispatched[2];
} agent_t;
typedef struct {
    pthread_mutex_t lock;
    tronix_job_t jobs[MAX_JOBS];
    agent_t agents[MAX_AGENT_SLOTS];
    tronix_stats_t stats;
    uint64_t enqueued_sequence;
    int interactive_streak;
    long service_id;
    tronix_scheduler_service_t service;
} runtime_t;

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
/* Caller holds r->lock. Expiry must progress even if the worker is busy. */
static void expire_pending_locked(runtime_t* r, uint64_t now) {
    for (int i=0;i<MAX_JOBS;i++) {
        tronix_job_t* j=&r->jobs[i];
        if (j->state==TRONIX_QUEUED && j->deadline_ns<=now) {
            j->state=TRONIX_EXPIRED;
            r->stats.pending--;
            r->stats.expired++;
        }
    }
}
static int valid(const char* s, size_t max) {
    if (!s) return 0;
    size_t n=strnlen(s,max+1);
    if (n==0 || n>max) return 0;
    for(size_t i=0;i<n;i++) {
        unsigned char ch=(unsigned char)s[i];
        if (ch<33 || ch>126) return 0;
    }
    return 1;
}
static int agent_slot(runtime_t* r, const char* agent, int allocate) {
    int empty=-1;
    for(int i=0;i<MAX_AGENT_SLOTS;i++) {
        if (strcmp(r->agents[i].name,agent)==0) return i;
        if(empty<0 && r->agents[i].name[0]=='\0') empty=i;
    }
    if (allocate && empty<0) {
        /* Recycle only an inactive agent; a running/queued agent keeps its fair-share history. */
        for(int i=0;i<MAX_AGENT_SLOTS;i++) {
            int busy=0;
            for(int j=0;j<MAX_JOBS;j++) {
                const tronix_job_t* job=&r->jobs[j];
                if((job->state==TRONIX_QUEUED || job->state==TRONIX_RUNNING) &&
                   strcmp(job->agent,r->agents[i].name)==0) {busy=1;break;}
            }
            if(!busy){empty=i;break;}
        }
    }
    if (allocate && empty>=0) {
        memset(&r->agents[empty],0,sizeof(r->agents[empty]));
        strncpy(r->agents[empty].name, agent, TRONIX_AGENT_MAX);
        return empty;
    }
    return -1;
}
static int admit(void* ptr,const char* id,const char* agent,const char* op,int lane,uint32_t deadline_ms,uint32_t budget_ms) {
    runtime_t* r=(runtime_t*)ptr;
    if(!valid(id,TRONIX_ID_MAX)||!valid(agent,TRONIX_AGENT_MAX)||
       !valid(op,TRONIX_OP_MAX)||(lane!=0&&lane!=1)||
       deadline_ms==0||deadline_ms>30000||budget_ms==0||budget_ms>30000) return -1;
    pthread_mutex_lock(&r->lock);
    expire_pending_locked(r,mono_ns());
    int slot=-1, own=0;
    for(int i=0;i<MAX_JOBS;i++) {
        tronix_job_t* job=&r->jobs[i];
        if(job->state && strcmp(job->id,id)==0) {
            r->stats.rejected++; pthread_mutex_unlock(&r->lock); return -3;
        }
        if(job->state==0 && slot<0) slot=i;
        if(job->state==TRONIX_QUEUED && strcmp(job->agent,agent)==0) own++;
    }
    if(slot<0 || r->stats.pending>=MAX_PENDING || own>=MAX_PER_AGENT ||
       agent_slot(r,agent,1)<0) {
        r->stats.rejected++; pthread_mutex_unlock(&r->lock); return -2;
    }
    tronix_job_t* job=&r->jobs[slot];
    memset(job,0,sizeof(*job));
    strcpy(job->id,id); strcpy(job->agent,agent); strcpy(job->operation,op);
    job->lane=lane; job->state=TRONIX_QUEUED;
    job->enqueued_seq=++r->enqueued_sequence;
    job->enqueued_ns=mono_ns();
    job->deadline_ns=job->enqueued_ns+(uint64_t)deadline_ms*1000000ULL;
    job->budget_ms=budget_ms;
    r->stats.pending++; r->stats.admitted++;
    if(r->stats.pending>r->stats.peak_pending) r->stats.peak_pending=r->stats.pending;
    pthread_mutex_unlock(&r->lock); return 0;
}
static int claim(void* ptr,tronix_job_t* out) {
    runtime_t* r=(runtime_t*)ptr;
    if(!out) return 0;
    pthread_mutex_lock(&r->lock);
    expire_pending_locked(r,mono_ns());
    if(r->stats.active) {pthread_mutex_unlock(&r->lock);return 0;}
    int count[2]={0,0};
    for(int i=0;i<MAX_JOBS;i++) {
        tronix_job_t* j=&r->jobs[i];
        if(j->state==TRONIX_QUEUED)count[j->lane]++;
    }
    if(count[0]+count[1]==0) {pthread_mutex_unlock(&r->lock);return 0;}
    int lane=(count[1] && (!count[0] || r->interactive_streak<INTERACTIVE_BURST))?1:0;
    if(lane) r->interactive_streak++; else r->interactive_streak=0;
    int selected=-1; uint64_t best_count=UINT64_MAX; uint64_t oldest=UINT64_MAX;
    for(int i=0;i<MAX_JOBS;i++) {
        tronix_job_t* j=&r->jobs[i];
        if(j->state!=TRONIX_QUEUED||j->lane!=lane) continue;
        int a=agent_slot(r,j->agent,0);
        uint64_t times=a<0?0:r->agents[a].dispatched[lane];
        /* Smallest dispatch count per agent; oldest job breaks ties. */
        if(times<best_count || (times==best_count && j->enqueued_seq<oldest)) {
            best_count=times;oldest=j->enqueued_seq;selected=i;
        }
    }
    if(selected<0) {pthread_mutex_unlock(&r->lock);return 0;}
    tronix_job_t* job=&r->jobs[selected];
    job->state=TRONIX_RUNNING;job->dispatch_seq=++r->stats.sequence;
    job->queue_wait_ns=mono_ns()-job->enqueued_ns;
    r->stats.total_queue_wait_ns+=job->queue_wait_ns;
    if(job->queue_wait_ns>r->stats.max_queue_wait_ns)
        r->stats.max_queue_wait_ns=job->queue_wait_ns;
    r->stats.pending--;r->stats.active=1;r->stats.dispatched++;
    int a=agent_slot(r,job->agent,0);
    if(a>=0)r->agents[a].dispatched[lane]++;
    *out=*job;
    pthread_mutex_unlock(&r->lock);return 1;
}
static int complete(void* ptr,const char* id,int ok,int64_t result,uint64_t execution_ns) {
    runtime_t* r=(runtime_t*)ptr;
    pthread_mutex_lock(&r->lock);
    for(int i=0;i<MAX_JOBS;i++) {
        tronix_job_t* job=&r->jobs[i];
        if(job->state==TRONIX_RUNNING && strcmp(job->id,id)==0) {
            const int overrun=execution_ns>(uint64_t)job->budget_ms*1000000ULL;
            job->execution_ns=execution_ns;
            job->state=overrun?TRONIX_OVERRUN:(ok?TRONIX_DONE:TRONIX_FAILED);
            job->value=overrun?0:result;
            r->stats.total_execution_ns+=execution_ns;
            if(execution_ns>r->stats.max_execution_ns)
                r->stats.max_execution_ns=execution_ns;
            r->stats.active=0;
            if(overrun)r->stats.overrun++;
            else if(ok)r->stats.completed++;
            else r->stats.failed++;
            pthread_mutex_unlock(&r->lock);return 1;
        }
    }
    pthread_mutex_unlock(&r->lock);return 0;
}
static int lookup(void* ptr,const char* id,tronix_job_t* out) {
    runtime_t* r=(runtime_t*)ptr;
    if(!id || !out)return 0;
    pthread_mutex_lock(&r->lock);
    expire_pending_locked(r,mono_ns());
    for(int i=0;i<MAX_JOBS;i++) if(r->jobs[i].state && strcmp(r->jobs[i].id,id)==0) {
        *out=r->jobs[i];pthread_mutex_unlock(&r->lock);return 1;
    }
    pthread_mutex_unlock(&r->lock);return 0;
}
static int ack(void* ptr,const char* id) {
    runtime_t* r=(runtime_t*)ptr;
    if(!id)return 0;
    pthread_mutex_lock(&r->lock);
    expire_pending_locked(r,mono_ns());
    for(int i=0;i<MAX_JOBS;i++) {
        tronix_job_t* j=&r->jobs[i];
        if(j->state>=TRONIX_DONE && strcmp(j->id,id)==0) {
            memset(j,0,sizeof(*j));
            pthread_mutex_unlock(&r->lock);return 1;
        }
    }
    pthread_mutex_unlock(&r->lock);return 0;
}
static void snapshot(void* ptr,tronix_stats_t* out) {
    runtime_t* r=(runtime_t*)ptr;
    if(!out)return;
    pthread_mutex_lock(&r->lock);
    expire_pending_locked(r,mono_ns());
    *out=r->stats;
    pthread_mutex_unlock(&r->lock);
}
static celix_status_t start(runtime_t* r,celix_bundle_context_t* ctx) {
    memset(r,0,sizeof(*r));r->service_id=-1;
    if(pthread_mutex_init(&r->lock,NULL)!=0)return CELIX_SERVICE_EXCEPTION;
    r->service.handle=r;
    r->service.admit=admit;r->service.claim=claim;r->service.complete=complete;
    r->service.lookup=lookup;r->service.ack=ack;r->service.snapshot=snapshot;
    celix_service_registration_options_t opts=CELIX_EMPTY_SERVICE_REGISTRATION_OPTIONS;
    opts.svc=&r->service;opts.serviceName=TRONIX_SCHEDULER_SERVICE;
    opts.serviceVersion=TRONIX_ABI_VERSION;
    r->service_id=celix_bundleContext_registerServiceWithOptions(ctx,&opts);
    return r->service_id>=0?CELIX_SUCCESS:CELIX_SERVICE_EXCEPTION;
}
static celix_status_t stop(runtime_t* r,celix_bundle_context_t* ctx) {
    if(r->service_id>=0)celix_bundleContext_unregisterService(ctx,r->service_id);
    pthread_mutex_destroy(&r->lock);return CELIX_SUCCESS;
}
CELIX_GEN_BUNDLE_ACTIVATOR(runtime_t,start,stop)

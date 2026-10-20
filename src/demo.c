/* Deliberately restricted demo adapter; no user-provided commands or scripts. */
#define _POSIX_C_SOURCE 200809L
#include "tronix_service.h"
#include <celix_bundle_activator.h>
#include <celix_bundle_context.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
typedef struct {
    long service_id;
    tronix_operation_service_t service;
} demo_t;
static int execute(void* handle,const char* op,int64_t* value) {
    (void)handle;
    if(!op || !value)return 0;
    if(strcmp(op,"demo.constant")==0){*value=42;return 1;}
    if(getenv("TRONIX_ISOLATION_FIXTURES")) {
        if(strcmp(op,"demo.pid")==0){*value=getpid();return 1;}
        if(strcmp(op,"demo.hang")==0){sleep(10);*value=123;return 1;}
        if(strcmp(op,"demo.crash")==0){abort();}
    }
    if(strcmp(op,"demo.pause")==0 || strcmp(op,"demo.slow")==0 || strcmp(op,"demo.overrun")==0) {
        struct timespec ts={.tv_sec=0,
            .tv_nsec=(strcmp(op,"demo.slow")==0 || strcmp(op,"demo.overrun")==0) ? 150000000L : 15000000L};
        nanosleep(&ts,NULL);*value=1;return 1;
    }
    return 0;
}
static int lane_for(void* handle,const char* op) {
    (void)handle;
    if(!op)return -1;
    if(strcmp(op,"demo.constant")==0)return 1;
    if(getenv("TRONIX_ISOLATION_FIXTURES") &&
       (strcmp(op,"demo.hang")==0 || strcmp(op,"demo.crash")==0 ||
        strcmp(op,"demo.pid")==0))return 0;
    if(strcmp(op,"demo.pause")==0 || strcmp(op,"demo.slow")==0 || strcmp(op,"demo.overrun")==0)return 0;
    return -1;
}
static uint32_t budget_for(void* handle,const char* op){
    (void)handle;
    if(!op)return 0;
    if(strcmp(op,"demo.constant")==0){
        // Fixture only: emulate policy changing after startup inspection.
        if(getenv("TRONIX_ISOLATION_FIXTURES") && getenv("TRONIX_POLICY_DRIFT_FILE") &&
           access(getenv("TRONIX_POLICY_DRIFT_FILE"),F_OK)==0)return 1100;
        return 1000;
    }
    if(getenv("TRONIX_ISOLATION_FIXTURES") &&
       strcmp(op,"demo.hang")==0)return 50;
    if(getenv("TRONIX_ISOLATION_FIXTURES") && strcmp(op,"demo.crash")==0)return 1000;
    if(getenv("TRONIX_ISOLATION_FIXTURES") && strcmp(op,"demo.pid")==0)return 1000;
    if(strcmp(op,"demo.pause")==0)return 50;
    if(strcmp(op,"demo.slow")==0)return 500;
    if(strcmp(op,"demo.overrun")==0)return 20;
    return 0;
}
static const char* operations(void* handle){(void)handle;return getenv("TRONIX_ISOLATION_FIXTURES")?
        "demo.constant,demo.pause,demo.slow,demo.overrun,demo.hang,demo.crash,demo.pid":
        "demo.constant,demo.pause,demo.slow,demo.overrun";}
static celix_status_t start(demo_t* d,celix_bundle_context_t* ctx) {
    memset(d,0,sizeof(*d));d->service_id=-1;
    if(getenv("TRONIX_ISOLATION_FIXTURES") && getenv("TRONIX_ACTIVATOR_MARKER")){
        const char* path=getenv("TRONIX_ACTIVATOR_MARKER");
        int fd=open(path,O_WRONLY|O_APPEND|O_CLOEXEC|O_NOFOLLOW);
        if(fd>=0){
            char line[32];const int len=snprintf(line,sizeof(line),"%ld\n",(long)getpid());
            if(len>0 && len<(int)sizeof(line)){
                const ssize_t written=write(fd,line,(size_t)len);
                (void)written;
            }
            close(fd);
        }
    }
    d->service.handle=d;d->service.execute=execute;
    d->service.lane_for=lane_for;d->service.budget_for=budget_for;
    d->service.operations=operations;
    celix_service_registration_options_t opts=CELIX_EMPTY_SERVICE_REGISTRATION_OPTIONS;
    opts.svc=&d->service;opts.serviceName=TRONIX_OPERATION_SERVICE;
    opts.serviceVersion=TRONIX_ABI_VERSION;
    d->service_id=celix_bundleContext_registerServiceWithOptions(ctx,&opts);
    return d->service_id>=0?CELIX_SUCCESS:CELIX_SERVICE_EXCEPTION;
}
static celix_status_t stop(demo_t* d,celix_bundle_context_t* ctx) {
    if(d->service_id>=0)celix_bundleContext_unregisterService(ctx,d->service_id);
    return CELIX_SUCCESS;
}
CELIX_GEN_BUNDLE_ACTIVATOR(demo_t,start,stop)

/* Deliberately restricted demo adapter; no user-provided commands or scripts. */
#define _POSIX_C_SOURCE 200809L
#include "tronix_service.h"
#include <celix_bundle_activator.h>
#include <celix_bundle_context.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
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
        if(strcmp(op,"demo.read_probe")==0) {
            const char* path=getenv("TRONIX_READ_PROBE");
            if(!path)return 0;
            int fd=open(path,O_RDONLY|O_CLOEXEC);
            if(fd>=0){close(fd);*value=0;return 1;}
            *value=errno;return 1;
        }
        if(strcmp(op,"demo.write_probe")==0) {
            const char* path=getenv("TRONIX_WRITE_PROBE");
            if(!path)return 0;
            int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
            if(fd>=0){close(fd);*value=0;return 1;}
            *value=errno;return 1;
        }
        if(strcmp(op,"demo.net_probe")==0) {
            const char* raw=getenv("TRONIX_CONNECT_PORT");
            if(!raw)return 0;
            unsigned long port=strtoul(raw,NULL,10);
            if(port<1||port>65535)return 0;
            int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
            if(fd<0){*value=errno;return 1;}
            struct sockaddr_in addr={0};
            addr.sin_family=AF_INET;
            addr.sin_port=htons((uint16_t)port);
            addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            int rc=connect(fd,(struct sockaddr*)&addr,sizeof(addr));
            int error=rc==0?0:errno;
            close(fd);*value=error;return 1;
        }

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
        strcmp(op,"demo.pid")==0 ||
        strcmp(op,"demo.read_probe")==0 ||
        strcmp(op,"demo.write_probe")==0 ||
        strcmp(op,"demo.net_probe")==0))return 0;
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
    if(getenv("TRONIX_ISOLATION_FIXTURES") &&
       (strcmp(op,"demo.pid")==0 || strcmp(op,"demo.read_probe")==0 ||
        strcmp(op,"demo.write_probe")==0 || strcmp(op,"demo.net_probe")==0))
        return 1000;
    if(strcmp(op,"demo.pause")==0)return 50;
    if(strcmp(op,"demo.slow")==0)return 500;
    if(strcmp(op,"demo.overrun")==0)return 20;
    return 0;
}
static const char* operations(void* handle){(void)handle;return getenv("TRONIX_ISOLATION_FIXTURES")?
        "demo.constant,demo.pause,demo.slow,demo.overrun,demo.hang,demo.crash,demo.pid,demo.read_probe,demo.write_probe,demo.net_probe":
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

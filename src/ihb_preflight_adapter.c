/* Opt-in IHB read-only synthetic preflight.  No Docker, shell, or target execution. */
#define _GNU_SOURCE
#include "tronix_service.h"
#include <celix_bundle_activator.h>
#include <celix_bundle_context.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef TRONIX_IHB_PREFLIGHT_SOCKET
#error "Compile-time operator-selected private IHB socket is required"
#endif
#ifndef TRONIX_IHB_PREFLIGHT_UID
#error "Compile-time broker UID is required"
#endif

typedef struct {
    long service_id;
    tronix_operation_service_t service;
} ihb_preflight_t;

static int known(const char* op) {
    return op && (strcmp(op, "fuzz.preflight.safe") == 0 ||
                  strcmp(op, "fuzz.preflight.buggy") == 0);
}

static int write_exact(int fd, const char* text, size_t n) {
    size_t sent = 0;
    while(sent < n) {
        ssize_t count = send(fd, text + sent, n - sent, MSG_NOSIGNAL);
        if(count <= 0) return 0;
        sent += (size_t) count;
    }
    return 1;
}

static int preflight_execute(void* handle, const char* op, int64_t* value) {
    (void)handle;
    if(!known(op) || !value) return 0;
    const char* path = TRONIX_IHB_PREFLIGHT_SOCKET;
    if(strlen(path) >= sizeof(((struct sockaddr_un*)0)->sun_path)) return 0;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if(fd < 0) return 0;
    struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
    struct sockaddr_un peer = {.sun_family = AF_UNIX};
    strcpy(peer.sun_path, path);
    int ok = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0 &&
             setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
             connect(fd, (struct sockaddr*)&peer, sizeof(peer)) == 0;
    if(ok) {
        struct ucred creds = {0};
        socklen_t length = sizeof(creds);
        ok = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &creds, &length) == 0 &&
             length == sizeof(creds) && creds.uid == (uid_t)TRONIX_IHB_PREFLIGHT_UID;
    }
    if(ok) {
        const size_t n = strlen(op);
        ok = write_exact(fd, op, n) && write_exact(fd, "\n", 1);
    }
    char response[24] = {0};
    size_t got = 0;
    while(ok && got < sizeof(response)-1) {
        ssize_t n = recv(fd, response+got, sizeof(response)-1-got, 0);
        if(n <= 0) { ok = 0; break; }
        got += (size_t)n;
        if(response[got-1] == '\n') break;
    }
    close(fd);
    if(!ok || got != 5 || memcmp(response,"OK 1\n",5) != 0) return 0;
    *value = 1;  /* Verified profile only; not an executed campaign. */
    return 1;
}

static int preflight_lane(void* handle, const char* op) {
    (void)handle;
    return known(op) ? 0 : -1;
}
static uint32_t preflight_budget(void* handle, const char* op) {
    (void)handle;
    return known(op) ? 3000 : 0;
}
static const char* preflight_ops(void* handle) {
    (void)handle;
    return "fuzz.preflight.safe,fuzz.preflight.buggy";
}
static celix_status_t start(ihb_preflight_t* ctx, celix_bundle_context_t* bundle) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->service_id = -1;
    ctx->service.handle = ctx;
    ctx->service.execute = preflight_execute;
    ctx->service.lane_for = preflight_lane;
    ctx->service.budget_for = preflight_budget;
    ctx->service.operations = preflight_ops;
    celix_service_registration_options_t opts = CELIX_EMPTY_SERVICE_REGISTRATION_OPTIONS;
    opts.svc = &ctx->service;
    opts.serviceName = TRONIX_OPERATION_SERVICE;
    opts.serviceVersion = TRONIX_ABI_VERSION;
    ctx->service_id = celix_bundleContext_registerServiceWithOptions(bundle, &opts);
    return ctx->service_id >= 0 ? CELIX_SUCCESS : CELIX_SERVICE_EXCEPTION;
}
static celix_status_t stop(ihb_preflight_t* ctx, celix_bundle_context_t* bundle) {
    if(ctx->service_id >= 0) celix_bundleContext_unregisterService(bundle,ctx->service_id);
    return CELIX_SUCCESS;
}
CELIX_GEN_BUNDLE_ACTIVATOR(ihb_preflight_t, start, stop)

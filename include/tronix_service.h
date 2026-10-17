#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define TRONIX_SCHEDULER_SERVICE "org.tronix.scheduler"
#define TRONIX_OPERATION_SERVICE "org.tronix.operation"
#define TRONIX_ABI_VERSION "1.0.0"
#define TRONIX_ID_MAX 64
#define TRONIX_AGENT_MAX 64
#define TRONIX_OP_MAX 48

enum { TRONIX_QUEUED=1, TRONIX_RUNNING=2, TRONIX_DONE=3,
       TRONIX_FAILED=4, TRONIX_EXPIRED=5 };
typedef struct {
    char id[TRONIX_ID_MAX+1];
    char agent[TRONIX_AGENT_MAX+1];
    char operation[TRONIX_OP_MAX+1];
    int state;
    int lane; /* 1 interactive, 0 background; server policy selects this */
    uint64_t deadline_ns;
    uint64_t enqueued_seq;
    uint64_t dispatch_seq;
    int64_t value;
} tronix_job_t;
typedef struct {
    uint64_t admitted, rejected, expired, dispatched, completed, failed;
    uint64_t pending, active, peak_pending, sequence;
} tronix_stats_t;
typedef struct {
    void* handle;
    /* 0 accepted; -1 invalid, -2 queue full, -3 duplicate */
    int (*admit)(void*, const char*, const char*, const char*, int, uint32_t);
    /* 1 claimed; 0 empty/busy */
    int (*claim)(void*, tronix_job_t*);
    int (*complete)(void*, const char*, int, int64_t);
    int (*lookup)(void*, const char*, tronix_job_t*);
    /* Removes only terminal results. Allows bounded memory without silent loss. */
    int (*ack)(void*, const char*);
    void (*snapshot)(void*, tronix_stats_t*);
} tronix_scheduler_service_t;
typedef struct {
    void* handle;
    int (*execute)(void*, const char* operation, int64_t* result);
} tronix_operation_service_t;
#ifdef __cplusplus
}
#endif

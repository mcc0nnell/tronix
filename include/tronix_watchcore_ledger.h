/* SPDX-License-Identifier: Apache-2.0 */
/* Observation-only Celix ABI for Atlas Watchcore ledger eligibility. */
#ifndef TRONIX_WATCHCORE_LEDGER_H
#define TRONIX_WATCHCORE_LEDGER_H
#include <stddef.h>
#include <stdint.h>
#define TRONIX_WATCHCORE_LEDGER_NAME "org.tronix.watchcore.ledger"
#define TRONIX_WATCHCORE_LEDGER_VERSION "1.0.0"
#define TRONIX_WATCHCORE_LEDGER_MAX_ROWS 512u
#define TRONIX_WATCHCORE_LEDGER_MAX_STRING 256u
/* kind: 0=PR,1=issue; status: 0=done,1=security_blocked,
 * 2=failed,3=deferred,4=other. */
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *repo;
    int kind;
    int64_t number;
    const char *revision;
    int status;
    int has_timestamp;
    int64_t timestamp_utc_seconds;
    int failure_was_limit;
} tronix_watchcore_ledger_row_t;

typedef struct {
    const char *repo;
    int kind;
    int64_t number;
    const char *revision;
    const char *default_repo;
    int64_t now_utc_seconds;
    uint32_t max_retries;
    int64_t backoff_base_seconds;
    int64_t backoff_max_seconds;
    const tronix_watchcore_ledger_row_t *rows;
    size_t row_count;
} tronix_watchcore_ledger_request_t;

typedef struct {
    int eligibility; /* 0=ready, 1=already_terminal, 2=exhausted, 3=backoff */
    size_t failure_count;
    int has_next_retry;
    int64_t next_retry_utc_seconds;
} tronix_watchcore_ledger_result_t;

typedef struct {
    void *handle;
    /* 0=valid; nonzero=no decision, never promote to an authority. */
    int (*decide)(void *handle, const tronix_watchcore_ledger_request_t *request,
                  tronix_watchcore_ledger_result_t *out);
} tronix_watchcore_ledger_service_t;
#ifdef __cplusplus
}
#endif
#endif

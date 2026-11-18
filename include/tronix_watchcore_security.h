/* SPDX-License-Identifier: Apache-2.0 */
/* Numeric-only C ABI for an observation-only Apache Celix Watchcore service. */
#ifndef TRONIX_WATCHCORE_SECURITY_H
#define TRONIX_WATCHCORE_SECURITY_H

#define TRONIX_WATCHCORE_SECURITY_NAME "org.tronix.watchcore.security"
#define TRONIX_WATCHCORE_SECURITY_VERSION "1.0.0"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int heur_score;
    int heur_risk;               /* 0=low, 1=medium, 2=high, 3=critical */
    int heur_signal_count;       /* 0..100000 */
    int heur_decisive;           /* 0 or 1 */
    int model_score;
    int model_risk;
    int model_fallback;          /* 0 or 1 */
    int model_rc;
    int policy_warn_risk;
    int policy_block_risk;
    int policy_require_corroboration;
} tronix_watchcore_security_input_t;

typedef struct {
    int score;
    int risk;                    /* 0..3 */
    int decision;                /* 0=proceed, 1=warn, 2=block */
    int block_downgraded_to_warn;
    int fallback_upgraded_to_warn;
} tronix_watchcore_security_output_t;

typedef struct {
    void *handle;
    /* Return 0 for a valid decision. Nonzero means no decision is available. */
    int (*decide)(void *handle, const tronix_watchcore_security_input_t *input,
                  tronix_watchcore_security_output_t *output);
} tronix_watchcore_security_service_t;

#ifdef __cplusplus
}
#endif
#endif

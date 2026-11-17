/* SPDX-License-Identifier: Apache-2.0 */
/* Optional deterministic byte-equality service; not a truth oracle. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define TRONIX_ANSWER_VERIFY_SERVICE "org.tronix.answer.verify"
#define TRONIX_ANSWER_VERIFY_ABI "1.0.0"
#define TRONIX_ANSWER_MAX_BYTES (64u * 1024u)
enum {
    TRONIX_ANSWER_UNVERIFIABLE = -1,
    TRONIX_ANSWER_REJECTED = 0,
    TRONIX_ANSWER_VERIFIED = 1
};
typedef struct {
    void* handle;
    /* VERIFIED means bytes match the expected digest, not objective truth. */
    int (*verify)(void* handle, const uint8_t* data, size_t len,
                  const uint8_t expected_sha256[32],
                  uint8_t actual_sha256[32]);
} tronix_answer_verify_service_t;
#ifdef __cplusplus
}
#endif

/* SPDX-License-Identifier: Apache-2.0 */
#include "tronix_answer_verify.h"
#include <celix_bundle_activator.h>
#include <celix_bundle_context.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <string.h>
typedef struct {
    long service_id;
    tronix_answer_verify_service_t service;
} answer_verifier_t;
static int verify(void* handle, const uint8_t* data, size_t len,
                  const uint8_t expected[32], uint8_t actual[32]) {
    (void)handle;
    if (!data || !expected || !actual || len > TRONIX_ANSWER_MAX_BYTES)
        return TRONIX_ANSWER_UNVERIFIABLE;
    unsigned int n = 0;
    if (!EVP_Digest(data, len, actual, &n, EVP_sha256(), NULL) || n != 32)
        return TRONIX_ANSWER_UNVERIFIABLE;
    return CRYPTO_memcmp(expected, actual, 32) == 0 ?
           TRONIX_ANSWER_VERIFIED : TRONIX_ANSWER_REJECTED;
}
static celix_status_t start(answer_verifier_t* v, celix_bundle_context_t* ctx) {
    memset(v, 0, sizeof(*v));
    v->service_id = -1;
    v->service.handle = v;
    v->service.verify = verify;
    celix_service_registration_options_t opts = CELIX_EMPTY_SERVICE_REGISTRATION_OPTIONS;
    opts.svc = &v->service;
    opts.serviceName = TRONIX_ANSWER_VERIFY_SERVICE;
    opts.serviceVersion = TRONIX_ANSWER_VERIFY_ABI;
    v->service_id = celix_bundleContext_registerServiceWithOptions(ctx, &opts);
    return v->service_id >= 0 ? CELIX_SUCCESS : CELIX_SERVICE_EXCEPTION;
}
static celix_status_t stop(answer_verifier_t* v, celix_bundle_context_t* ctx) {
    if (v->service_id >= 0) celix_bundleContext_unregisterService(ctx, v->service_id);
    return CELIX_SUCCESS;
}
CELIX_GEN_BUNDLE_ACTIVATOR(answer_verifier_t, start, stop)

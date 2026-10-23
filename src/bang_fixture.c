// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Explicitly synthetic protocol exercise: no audio, network, radio, dispatch,
// streaming caption service, or human receipt.
#include "tronix_bang_fixture.h"
#include "lowbang.h"
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <stdint.h>
#include <string.h>

#define SESSION "abababababababababababababababab"
#define NOW UINT64_C(600000)
#define DELIVERY "bang.synthetic_delivery"
#define GAP "bang.synthetic_gap"

int tronix_bang_fixture_run(const char* operation, int64_t* result) {
    if (!operation || !result ||
        (strcmp(operation, DELIVERY) != 0 && strcmp(operation, GAP) != 0))
        return 0; // Reject unknown operations before generating any keys.
    *result = 0;
    uint8_t seed[32] = {0}, public_key[32] = {0};
    if (RAND_bytes(seed, sizeof seed) != 1) return 0;

    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL,
                                                seed, sizeof seed);
    size_t public_len = sizeof public_key;
    int pub_ok = key && EVP_PKEY_get_raw_public_key(key, public_key, &public_len) == 1 &&
                 public_len == sizeof public_key;
    EVP_PKEY_free(key);
    bang_source* source = pub_ok ? bang_source_new("main-stage", "en", SESSION, seed) : NULL;
    OPENSSL_cleanse(seed, sizeof seed);
    if (!source) return 0;

    bang_receiver* receiver = bang_receiver_new();
    int good = receiver != NULL;
    if (good) good = bang_receiver_provision(receiver, "main-stage", public_key) == BANG_OK;
    OPENSSL_cleanse(public_key, sizeof public_key);
    if (good) {
        bang_receiver_enable(receiver);
        good = bang_receiver_subscribe(receiver, "main-stage") == BANG_OK;
    }

    bang_event first = {0}, second = {0};
    bang_delivery delivery = {0};
    if (good) good = bang_source_emit(source, BANG_CAPTION, "Welcome to main stage",
                                      NOW, 15000, &first) == BANG_OK;
    if (good) good = bang_source_emit(source, BANG_SOUND, "[APPLAUSE]",
                                      NOW + 1, 15000, &second) == BANG_OK;
    if (good) {
        good = bang_receiver_receive(receiver, &second, NOW + 20, &delivery) == BANG_OK &&
               delivery.application_received && delivery.ready_count == 0 &&
               delivery.waiting_for == 1;
    }
    if (good && strcmp(operation, DELIVERY) == 0) {
        good = bang_receiver_receive(receiver, &first, NOW + 21, &delivery) == BANG_OK &&
               delivery.ready_count == 2 &&
               delivery.ready[0].seq == 1 && delivery.ready[1].seq == 2 &&
               delivery.ready[0].kind == BANG_CAPTION &&
               delivery.ready[1].kind == BANG_SOUND &&
               strcmp(delivery.ready[0].text, "Welcome to main stage") == 0 &&
               strcmp(delivery.ready[1].text, "[APPLAUSE]") == 0;
        if (good) *result = 2;
    } else if (good && strcmp(operation, GAP) == 0) {
        good = bang_receiver_flush(receiver, "main-stage", SESSION, NOW + 270,
                                   250, &delivery) == BANG_OK &&
               delivery.gap_reported && delivery.missing_from == 1 &&
               delivery.missing_through == 1 && delivery.ready_count == 1 &&
               delivery.ready[0].seq == 2 &&
               strcmp(delivery.ready[0].text, "[APPLAUSE]") == 0;
        if (good) *result = 1;
    }

    bang_receiver_free(receiver);
    bang_source_free(source);
    return good;
}

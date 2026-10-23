// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
#ifndef TRONIX_BANG_FIXTURE_H
#define TRONIX_BANG_FIXTURE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Synthetic-only operations. 1 success, 0 denied/failure. No radio/ASR. */
int tronix_bang_fixture_run(const char* operation, int64_t* result);
#ifdef __cplusplus
}
#endif
#endif

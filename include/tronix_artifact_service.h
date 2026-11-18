// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Optional artifact digest service; does not change the frozen TRONIX v2 ABI.
// Only registered by the pinned, explicitly granted offline Rizzer adapter.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#define TRONIX_ARTIFACT_SERVICE "org.tronix.artifact.digest"
#define TRONIX_ARTIFACT_ABI_VERSION "1.0.0"
typedef struct {
    void* handle;
    // A successful one-shot worker operation returns its full native-parity
    // SHA-256 as a 64-byte lowercase hex string plus terminating NUL.
    // No digest is produced on failure, timeout, or before execution.
    int (*take_digest)(void* handle, char out[65]);
} tronix_artifact_service_t;
#ifdef __cplusplus
}
#endif

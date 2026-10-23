// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "lowbang.h"
#include <stddef.h>
#include <stdint.h>
// Narrow lab codec for signed 6lowBang events. Never an RF frame-size claim.
// Header: magic BG, version, kind, channel/lang/text lengths, binary session,
// seq/captured/expiry (big-endian uint64), Ed25519 signature, then UTF-8 fields.
#define TRONIX_BANG_WIRE_HEADER 111
#define TRONIX_BANG_WIRE_MAX (TRONIX_BANG_WIRE_HEADER + BANG_MAX_CHANNEL + BANG_MAX_LANGUAGE + BANG_MAX_TEXT)
int tronix_bang_wire_encode(const bang_event *event, uint8_t *out, size_t cap, size_t *length);
int tronix_bang_wire_decode(const uint8_t *input, size_t length, bang_event *event);

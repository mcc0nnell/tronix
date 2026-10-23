// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Dependency-independent driver for the native lib6lowBang fixture.
#include "tronix_bang_fixture.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)
int main(void) {
    int64_t value = -1;
    CHECK(tronix_bang_fixture_run("bang.synthetic_delivery", &value) == 1);
    CHECK(value == 2);
    puts("PASS: signed synthetic caption + sound order");
    value = -1;
    CHECK(tronix_bang_fixture_run("bang.synthetic_gap", &value) == 1);
    CHECK(value == 1);
    puts("PASS: signed text gap reported; no invented words");
    value = -1;
    CHECK(tronix_bang_fixture_run("bang.raw_cli", &value) == 0);
    CHECK(value == -1); // Unknown operation has no effects on output.
    CHECK(tronix_bang_fixture_run("bang.synthetic_delivery", NULL) == 0);
    CHECK(tronix_bang_fixture_run(NULL, &value) == 0);
    puts("PASS: unapproved actions and invalid pointers rejected");
    return 0;
}

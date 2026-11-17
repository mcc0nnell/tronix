# TRONIX persistent Celix verifier and selective Watcher handoff

Status: experimental, review-only, not deployed. This extends the
one-shot TRONIX Celix verifier from PR #19 on a separate feature branch.
It does not change the existing TRONIX int64 scheduler ABI or v1 wire.

## Native runtime and controls

- tronix_verify_daemon: one persistent C++17 host process with real
  Apache Celix framework and the registered org.tronix.answer.verify service.
- No provider execution, no model access, no shell tools, no dynamic
  untrusted bundles, no network listeners or automatic publication.
- Bundle archive is fixed by an operator-supplied SHA-256; staged bytes
  are independently checked before the Celix service is installed.
- Owner-authored 0600 oracle manifest: up to 128 lines, each
  "taskId lowercase-sha256". No duplicate task IDs or unknown syntax.
  The complete source manifest is snapshotted in memory at startup.
  A model provider cannot select the correct answer over the socket.
- Owner-private 0700 directory contains the new Unix socket (0600).
  SO_PEERCRED restricts local IPC to the daemon's effective UID.
  This is not a malicious same-UID process boundary or sandbox.
- MessagePack v2 uses a big-endian 4-byte frame length, max 65,536 bytes plus 1,024 bytes of framing allowance.
  Verify requests have exactly v, action, taskId, answer (BIN max 64 KiB).
  Stats requests have exactly v and action.
  Invalid fields/types, duplicate keys, trailing objects and v1 requests
  are rejected. Request handling is serial with a two-second I/O timeout.
- Receipts carry oracle manifest digest, actual and expected answer digests,
  status, route, verification nanoseconds, byte count and pending human
  review. They are operator-local, unsigned and not remotely attested.

## Routing

VERIFIED -> no Watcher call. Exact equality to a trusted golden hash only.
REJECTED -> no Watcher call. An incorrect known answer stays rejected.
UNVERIFIABLE -> atlas-review-required, but execution remains unauthorized.

The evidence-only bridge in src/verify_handoff.mjs independently hashes the
local answer and checks the native result. For UNVERIFIABLE only, it creates
a new owner-private directory with answer.bin and handoff.json; it never
replaces old evidence. The file explicitly says humanReview=pending,
published=false, executionAuthorized=false. This is NOT a Rizzer
atlas-operator-job and it does not invoke, approve, pay or post anything.
A separately reviewed, authenticated Rizzer adapter is still necessary
before the existing Atlas Watcher broker can consume this handoff.

## Build and test

Use the pinned Celix and msgpack-c source revisions from TRONIX, then run:

  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCELIX_SOURCE=/absolute/path/to/pinned/celix \
    -DMSGPACK_SOURCE=/absolute/path/to/pinned/msgpack-c
  cmake --build build --target tronix_verify_daemon \
    tronix_answer_verifier_bundle tronix_demo_bundle -j 4
  npm ci --ignore-scripts
  npm run test:verification

For local use, supply all four required options to tronix_verify_daemon:
--socket PRIVATE_DIRECTORY/verify.sock
--oracle PRIVATE_0600_ORACLE_FILE
--bundle ABSOLUTE_TRUSTED_CELIX_ZIP
--bundle-sha256 THE_OPERATOR_VERIFIED_ARCHIVE_SHA256

Use verifyRequest and verifyStats in src/verify_client.mjs, or the
review-only CLI in src/verify_handoff.mjs with --socket --task --input --out.

## Security and cost caveats

This is a separate persistent daemon, NOT a new mode of the original
tronixd scheduler. It reuses Celix lifecycle, owner-only admission, and
operator-pinned artifact identity principles, but not TRONIX's existing
worker process supervisor or HMAC grant evidence. Native bundle code
runs under the operator's UID. Staging reduces but does not eliminate
same-UID filesystem and process threats. No arbitrary model answers can
be objectively proven by a golden SHA-256 equality check.

A later phase must bind independently authenticated Rizzer Scan/Forge
receipts and oracle authority, enforce safe broker egress and budgets,
and preserve pending human approval while routing selected cases to the
real Atlas Watcher. HMAC or signature verification, external checkpoints,
full hostile-process isolation and actual review costs remain out of scope.

Measure total inference + verification + escalation + retry cost per
objectively successful output, and track false acceptance and latency.
The local persistent round-trip benchmark alone does not establish
economic savings.

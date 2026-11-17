# TRONIX / Rizzer deterministic verification pilot

Experimental native Apache Celix correctness *primitive*, not an AI truth oracle.
No live Rizzer/Atlas Watcher integration, model calls, marketplace transactions,
automated patching, or production privileges are included.

## What is implemented

- Celix service org.tronix.answer.verify, service ABI 1.0.0.
- One-shot CLI accepts a bounded regular-file answer (up to 64 KiB), an
  independent, operator-supplied lowercase SHA-256 expected digest, a task ID,
  and a trusted Celix bundle with its exact SHA-256 pin.
- The CLI stages the pinned bundle bytes in a private directory, starts a real
  Celix framework, discovers the registered service, and executes its digest
  comparison. It refuses absent services, invalid inputs and bundle mismatch.
- Status is VERIFIED for matching digest, REJECTED for mismatch and
  UNVERIFIABLE for missing or invalid evidence/service.
- Exit codes are 0 for VERIFIED, 1 for REJECTED, 3 for UNVERIFIABLE.
- JSON receipts expose actual and expected digests, byte count, wall time in
  nanoseconds, task ID and decision. The receipt explicitly states that
  objective truth is not established, and publication is disabled.
- Input symlinks, parent symlinks, path traversal components, missing paths,
  nonregular files, oversized data, unknown CLI flags and duplicate CLI flags
  fail closed. Paths are operator controlled, and this is not a hardened
  hostile-filesystem/concurrent-writer boundary.

## Reproduce

Use this repository's existing Celix and msgpack-c source pins. From repo root:

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCELIX_SOURCE=/absolute/path/to/pinned/apache-celix \
      -DMSGPACK_SOURCE=/absolute/path/to/pinned/msgpack-c
    cmake --build build --target tronix_answer_verifier_bundle tronix_verify_once tronix_demo_bundle -j 4
    bash tests/answer-verifier.sh

A manual operation with a golden expected digest:

    ./build/tronix_verify_once --task-id task42 \
      --input /absolute/path/to/answer \
      --expected-sha256 PRECOMMITTED_64_CHARACTER_HEX \
      --bundle /absolute/path/to/build/tronix_answer_verifier.zip \
      --bundle-sha256 VERIFIED_BUNDLE_64_CHARACTER_HEX

The expected digest must come from independent reviewed ground truth. Generating
the expected digest from the answer itself establishes only self-consistency.

## Boundaries

SHA-256 equality proves only that the bytes match a precommitted digest. It is
not proof of factual accuracy, semantic equivalence, security impact, or model
truthfulness. The expected digest must not be selected by the inference provider.
The Celix bundle is native code running as the caller, not sandboxed untrusted
code. The local receipt is unsigned and has no external authenticity claim.
The staged bundle reduces path-replacement risk but does not protect against
same-UID compromise. This does not establish performance or cost savings.

TRONIX's existing v1 scheduler and int64 operation API are untouched. They do
not currently transport arbitrary per-answer data. The new service is a
standalone, optional typed capability and does not change any existing jobs.

## Follow-up contract (proposed, not implemented)

1. Rizzer supplies an independently verified evidence artifact and a frozen
   correctness oracle, without granting Celix additional scan/forge authority.
2. A versioned typed work/result interface carries bounded answer bytes to
   the Celix verifier and a signed or independently retained receipt from an
   appropriate trust boundary.
3. Only UNVERIFIABLE or explicitly disputed cases escalate to Atlas Watcher's
   existing broker, with model budgets and independent reviewer policies.
4. REJECTED remains a rejection; reviewer consensus cannot override deterministic
   evidence without a separately authorized oracle change.
5. Compare the original and selective paths using total cost per objectively
   accepted answer, false-accept rates, total latency, and model calls.

Do not merge this pilot into Rizzer's ongoing MR !40 without independent review
and the existing human approval/security gates.

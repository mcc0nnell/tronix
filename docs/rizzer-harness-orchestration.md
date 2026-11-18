# TRONIX ↔ Rizzer corpus harness orchestration (experimental)

> **Revision note, 2026-11-18:** This page documents the initial
> single-operation v1 bridge. The stacked catalog/digest upgrade in
> [harness-catalog-signed-receipts.md](harness-catalog-signed-receipts.md)
> replaces the fixed `--casebook` argument with a pinned multi-cartridge
> catalog and adds HMAC-protected v2 full-report SHA-256 receipts. The
> limitations below describe the original v1 increment only.

## Delivered boundary

This optional TRONIX v2 operation executes a **single operator-selected,
offline-reference harness** using the already-tested native Apache Celix
evaluator from Rizzer MR !83, and returns a **bounded result count** through
TRONIX's existing scheduler, supervised Celix worker and exact-archive
capability grant system. The original TRONIX scheduler, worker, wire protocol
and grants-evidence implementation are **unchanged**.

```text
TRONIX client -> native scheduler -> allowlisted/granted "rizzer.harness.reference"
                           |
                           v
                TRONIX one-shot worker
                Celix adapter (C++17)
                           |
                 UID-private socket + token
                           |
                           v
             Node local fixed-scope sidecar
             (not in Celix's process)
                           |
             fixed operator-pinned CLI args
                           |
                           v
          Rizzer scan harness-native (Python)
                 -> pinned Celix C++ oracle
                 -> reference parity check
                 -> provenance-bound local receipt
                           |
                           v
     sidecar local dispatch evidence + count (3..32)
                           |
                           v
    TRONIX signed grant-evidence job result (existing)
```

Only `rizzer.harness.reference` is supported. An untrusted client cannot
supply paths, scripts, URLs, target names, token material, or arbitrary
operations. TRONIX's own `tronix.grants.v1` checks are applied before
admission, and the adapter does not instantiate an independent scheduler.

## Design constraint discovered during integration

`tronix_worker` deliberately strips the daemon's inherited environment.
The first prototype attempted to configure the adapter through environment
variables and correctly failed worker policy inspection. We **did not**
modify that isolation policy.

Instead, the optional adapter derives a fixed, UID-scoped local path:

```text
/tmp/tronix-rizzer-harness-<effective-UID>/harness.sock
/tmp/tronix-rizzer-harness-<effective-UID>/bridge.token
```

The directory must be owned by the daemon/worker's effective UID with mode
0700, the token an owner-private regular file mode 0600 containing exactly
64 lowercase hex characters, and the socket owner-private mode 0600. The
bridge verifies these conditions independently. This prototype supports
**one bridge instance per UID**, and refuses occupied directories.

The static token limits direct socket access but does **not** establish
independent same-UID process isolation: other code executing as that UID
could access that UID's token. This is a trusted same-UID local research
boundary, not a multi-tenant security sandbox.

## Build

Uses the existing pinned Apache Celix and msgpack-c source trees; no
additional vendored runtime. This adapter requires the same local build
prerequisites as TRONIX, and the Node dependency already in package.json.

```bash
cmake -S . -B build -G Ninja \
  -DCELIX_SOURCE=/absolute/reviewed/apache-celix \
  -DMSGPACK_SOURCE=/absolute/reviewed/msgpack-c
cmake --build build --target \
  tronix_rizzer_harness_bundle tronix_scheduler_bundle tronix_worker tronixd
```

The external Rizzer checkout must include **both**
`forge harness` (MR !78) and `scan harness-native` (MR !83).
Neither merge request is an assumed production installation; use a reviewed
checkout and exact artifact pins.

Prepare an owner-private bridge root and separate evidence root. Use
`umask 077`. An operator must create a fresh private root instead of
overwriting an existing instance.

```bash
umask 077
ROOT="/tmp/tronix-rizzer-harness-$(id -u)"
mkdir -m 700 "$ROOT"             # abort if already exists
python3 -c 'import secrets; print(secrets.token_hex(32),end="")' \
  > "$ROOT/bridge.token"
chmod 600 "$ROOT/bridge.token"

EVIDENCE=/absolute/private/new-evidence-directory
mkdir -m 700 "$EVIDENCE"         # must not already exist
```

Run the fixed-scope bridge in a separate process. All paths must be
operator-selected and absolute. It verifies CLI/host/bundle SHA-256 pins,
harness-source digests and native/Python parity; it does not take target
parameters from the Unix socket.

```bash
node scripts/rizzer_harness_sidecar.mjs \
  --socket "$ROOT/harness.sock" --token-file "$ROOT/bridge.token" \
  --rizzer-cli /absolute/reviewed/rizzer/rizzer_cli.py \
  --rizzer-cli-sha256 SHA256_OF_RIZZER_CLI \
  --casebook /absolute/reviewed/compiled-corpus-harness \
  --host /absolute/reviewed/rizzer_native_harness_once \
  --host-sha256 SHA256_OF_REVIEWED_NATIVE_HOST \
  --bundle /absolute/reviewed/rizzer_harness_oracle.zip \
  --bundle-sha256 SHA256_OF_REVIEWED_CELIX_BUNDLE \
  --evidence-dir "$EVIDENCE"
```

Create an owner-private `tronix.grants.v1` file matching the *actual*
TRONIX adapter archive's SHA-256:

```text
tronix.grants.v1
bundle org.tronix.adapter.rizzer_harness
version 0.1.0
artifact <exact SHA256 of build/tronix_rizzer_harness.zip>
allow rizzer.harness.reference
```

Launch `tronixd` with `--scheduler build/tronix_scheduler.zip`,
`--adapter build/tronix_rizzer_harness.zip`, `--worker
build/tronix_worker`, `--adapter-sha256`, `--grants`, `--evidence`,
`--evidence-key` and the usual owner-private socket path. The grants,
HMAC evidence key and evidence-log requirements are the **existing** TRONIX
contract; follow its grant and archive-identity documentation.

A TRONIX client can then submit only the explicitly granted
`rizzer.harness.reference` operation. It returns a numeric count of cases
(3–32) **only** after native execution and Python reference parity succeeded.
This count is *not* a vulnerability count.

## Evidence

The bridge creates `run-NNNN/receipt.json`,
`run-NNNN/native-parity.json`, and a create-only
`run-NNNN.dispatch.json` containing all exact SHA-256 content hashes. The
existing TRONIX grant evidence chain records the signed *operation result*.
The current TRONIX v2 wire result is an `int64` count and **does not carry a
cryptographically complete Rizzer artifact digest**. The signed grant ledger
and full Rizzer receipt are separate locally verifiable artifacts; do **not**
claim that the HMAC ledger signs the full parity result. A versioned typed
receipt linking the full 256-bit evidence hash is future work.

## Verification

Native operator test on Butcher (2026-11-17 America/New_York):
- The actual Celix adapter compiled against existing TRONIX source.
- TRONIX's actual scheduler admitted and completed `rizzer.harness.reference`.
- Unallowed TRONIX operation denied; direct wrong-token and wrong-operation
  requests to the bridge denied **before Rizzer execution**.
- Existing independent `scripts/verify_grant_evidence.py` validated
  the HMAC-linked ledger.
- All **four** Rizzer corpus families executed through the full TRONIX →
  Celix adapter → sidecar → Rizzer/Celix native parity path:
  MCP authorization (4 cases), 6LoWPAN (6), Spinel (6), FullMath (5):
  **21/21 offline reference results**, no target calls.
- Each family ran 11 asserted integration checks, **44/44** across four
  separate private instances, with existing evidence retained on the
  authorized research machine.

Run the test suite with **explicit operator-selected** Rizzer artifact paths:

```bash
RIZZER_HARNESS_TEST_CLI=/absolute/reviewed/rizzer/rizzer_cli.py \
RIZZER_HARNESS_TEST_CASEBOOK=/absolute/compiled-harness \
RIZZER_HARNESS_TEST_HOST=/absolute/native-host \
RIZZER_HARNESS_TEST_BUNDLE=/absolute/native-oracle.zip \
node tests/rizzer-harness.mjs
```

Without those four paths, `npm run test:rizzer` checks the bridge's JavaScript
syntax and **explicitly skips** native integration; it never reports an
untested native flow as passing. This test refuses to disturb a preexisting
UID-private bridge root.

## Limitations and next contract

1. Only offline reference harnesses and explicit local grants. No live target
   execution, network scanner, on-demand corpus synthesis or bounty finding.
2. The sidecar is a local research agent with the operator's UID privileges,
   not an OS sandbox. No same-UID process isolation is claimed.
3. The adapter is a **fixed single-operation interface**. Versioned dynamic
   cartridge discovery and independent capability assignment remain future
   additions; do not turn this into an arbitrary subprocess runner.
4. The signed TRONIX operation result and full Rizzer native-parity report are
   not yet bound by one cryptographically complete signed artifact reference.
   This is the next evidence protocol upgrade.

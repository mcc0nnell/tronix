# Atlas Watchcore: real Apache Celix observation service

Status: experimental stacked PR; source code on
`feat/celix-watchcore-service-20261118`, based on
`feat/celix-typed-persistent-review-gate-20261117`.
This is a **security-decision shadow service**, not a complete port of the
Atlas Watcher orchestrator, not a deployed daemon, and not a production promotion.

## Provenance and authority

The C++20 `integrations/atlas-watchcore` snapshot is the preserved standalone
source package previously parity-tested against `garland3/atlas-watcher`,
Python reference commit `96829a699f6c27b3bc304cd1a033b88073d4660f`.
Original header/source and original C++ tests are not modified by this PR.
The earlier 1,690/1,690 Python/native parity report is prior evidence,
**not a claimed fresh rerun** against this Celix build. Review upstream
license provenance and distribution attribution before release.

Actual Python Watcher remains authoritative. Rizzer MR !74 is an opt-in
observation hook. The follow-on Rizzer MR !92 forwards ONLY two pinned Celix
bundle variables to an otherwise fixed child environment. Neither native
match nor mismatch changes Python scoring, posting or agent selection.

## True Celix lifecycle

1. A numeric-only C ABI `org.tronix.watchcore.security` version `1.0.0`
   registers with Celix at bundle start and unregisters on stop.
2. The host `tronix_watchcore_shadow` validates its exact 11-operand
   `security` protocol, and requires an operator-selected absolute bundle
   path and 64 lowercase hexadecimal SHA-256 pin.
3. It reads a bounded, regular, unlinked, owner-trusted bundle archive and
   hashes it; a private owner-only directory receives a staged copy that is
   hashed again before Celix loads it.
4. It starts a real Celix framework, installs/starts the pinned bundle, looks
   up the versioned service and executes its `decide` method. It shuts down
   the framework and returns only five normalized JSON fields.
5. Failures produce no decision JSON and a nonzero exit, recorded by Rizzer
   as `unavailable`. No fallback to a local direct-library decision occurs.

This is currently a **per-call, one-shot Celix lifecycle** rather than a
persistent daemon or warmed service pool. It adds startup overhead and is
unsuitable as a performance claim. The service calls the original
`watchcore::decide_security` and has no network, model, git, publishing,
scanning, or autonomous tool authority.

## Build and test

Use the project's *existing pinned* Celix and msgpack-c sources:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCELIX_SOURCE=/absolute/path/to/pinned/apache-celix \
  -DMSGPACK_SOURCE=/absolute/path/to/pinned/msgpack-c
cmake --build build --target tronix_watchcore_security_bundle \
  tronix_watchcore_shadow tronix_watchcore_reference \
  tronix_watchcore_unit_tests tronix_demo_bundle -j 4
python3 tests/watchcore_celix_integration.py \
  "$(realpath build/tronix_watchcore_shadow)" \
  "$(realpath build/tronix_watchcore_reference)" \
  "$(realpath build/tronix_watchcore_security.zip)" \
  "$(realpath build/tronix_demo.zip)"
```

For an independent Rizzer source-tree Python decision parity check
without model/provider calls:

```sh
python3 tests/watchcore_python_shadow_parity.py \
  /absolute/path/to/rizzer-mr92-checkout \
  "$(realpath build/tronix_watchcore_shadow)" \
  "$(realpath build/tronix_watchcore_security.zip)"
```

The Python test stubs external import dependencies and normalized
provider/heuristic **acquisition only**. The actual Python
`security.assess()` and its corroboration, risk, fallback and shadow
observer logic remain untouched.

## Manual operator opt-in

```sh
export WATCHCORE_CELIX_BUNDLE="$(realpath build/tronix_watchcore_security.zip)"
export WATCHCORE_CELIX_BUNDLE_SHA256="$(sha256sum "$WATCHCORE_CELIX_BUNDLE" | cut -d' ' -f1)"
export WATCHCORE_SHADOW_BIN="$(realpath build/tronix_watchcore_shadow)"
export WATCHCORE_SHADOW_SHA256="$(sha256sum "$WATCHCORE_SHADOW_BIN" | cut -d' ' -f1)"
# Run the Rizzer Atlas Watcher security assessment from the MR !92 checkout
# in an authorized local sandbox; no production enablement is automatic.
```

## Evidence and limits

Butcher, November 18, 2026:
- Original standalone CMake/CTest source suite: 1/1 passed. Also exposed
  as `tronix_watchcore_unit_tests` in the native CI build.
- 344 **real Celix framework/bundle lifecycles** produced identical decisions
  to independently built original native Watchcore.
- 12 malformed/missing/symlinked/wrong-pinned/wrong-service inputs produced no result.
- 240 synthetic normalized inputs through the **actual** Python
  `security.assess()` matched the real Celix host using Rizzer MR !92.
- One forced bad bundle digest left Python's block verdict authoritative.
- Rizzer's adapter unit tests passed 10, with one optional real-native
  skip in an unconfigured environment; the real Celix test was run separately.

These are locally executed tests; **exact-head GitHub and GitLab CI must
still pass** and receive independent review before any merge.
Not established: full Watchcore routing/cooldown/ledger Celix ABI,
Watcher scheduling, live agents, production deployment, authenticated
remote attestation, cost savings, hostile same-UID isolation, or faster
than Python decisions.

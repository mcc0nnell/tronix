# Atlas Watchcore ledger eligibility — native Celix shadow

**Status:** experimental, review-only draft stacked after TRONIX PR #25.
This is not the complete Atlas Watcher port or a deployed service.

## Architecture and authority

The existing, preserved Watchcore C++20 `decide_item()` logic now runs behind
a *second* named service in the same operator-pinned Celix bundle:

- `org.tronix.watchcore.security`, ABI `1.0.0`, unchanged.
- `org.tronix.watchcore.ledger`, ABI `1.0.0`, newly registered and
  unregistered with Celix lifecycle management.

The bundle refuses partial registration: ledger registration failure rolls
back security registration. The host starts a **real** Celix framework,
validates an operator-supplied SHA-256 bundle pin, privately stages the exact
archive, discovers the requested service by name/version, and shuts down
after the synchronous decision. No direct-library fallback exists.

The existing `tronix_watchcore_shadow security ...` Rizzer MR !92 interface
does not change. The additional `tronix_watchcore_shadow ledger` subcommand
reads one bounded UTF-8 JSON object on **stdin**, not from process arguments.
It returns a non-authoritative, three-field JSON observation:

```json
{"eligibility":"backoff","failure_count":1,"next_retry_utc_seconds":1773001900}
```

It accepts at most 64 KiB of JSON and 512 ledger rows. Strings must be
printable ASCII, length 1..256 (an empty row repo means a legacy row);
numeric fields and booleans are strictly typed. Unknown/duplicate/missing
fields, malformed inputs, incorrect bundle pins, and absent service fail
closed with a nonzero exit and **no decision JSON**.

## Required input schema

All fields are mandatory. `kind`: 0=PR, 1=issue.
`status`: 0=done, 1=security_blocked, 2=failed, 3=deferred, 4=other.

```json
{
  "repo": "example/atlas",
  "kind": 0,
  "number": 42,
  "revision": "sha123",
  "default_repo": "example/atlas",
  "now": 1773001500,
  "max_retries": 3,
  "backoff_base_seconds": 900,
  "backoff_max_seconds": 86400,
  "rows": [
    {
      "repo": "example/atlas",
      "kind": 0,
      "number": 42,
      "revision": "sha123",
      "status": 2,
      "has_timestamp": true,
      "timestamp": 1773001000,
      "failure_was_limit": false
    }
  ]
}
```

The `failure_was_limit` flag is normalized **outside** the service from
transcript evidence. The native core never reads model output or transcripts.
A row with `repo=""` is restricted to `default_repo`; it does not match
arbitrary repositories. The `has_timestamp` flag separates a missing
timestamp from Unix epoch zero. The service outputs no repo, revision, prompt,
transcript, credential or user-controlled text.

## Repeatable verification

With pinned Celix and msgpack-c sources already configured through TRONIX:

```sh
cmake --build build --target tronix_watchcore_security_bundle \
  tronix_watchcore_shadow tronix_watchcore_unit_tests tronix_demo_bundle -j 4

./build/tronix_watchcore_unit_tests

python3 tests/watchcore_celix_ledger.py \
  "$(realpath build/tronix_watchcore_shadow)" \
  "$(realpath build/tronix_watchcore_security.zip)" \
  "$(realpath build/tronix_demo.zip)"
```

An owner with an authorized checkout of Rizzer MR !92 can additionally run:

```sh
python3 tests/watchcore_celix_ledger.py \
  "$(realpath build/tronix_watchcore_shadow)" \
  "$(realpath build/tronix_watchcore_security.zip)" \
  "$(realpath build/tronix_demo.zip)" \
  /absolute/path/to/rizzer-mr92-checkout
```

That optional test imports the actual Python Atlas Watcher `ledger.py`
and compares `has_done`, `failure_count`, `retries_exhausted`, and
`next_retry_at`. Only external ledger-file and transcript-limit inputs are
provided by fixtures; Python decision functions remain unmodified. Public CI
does not require access to a private Rizzer checkout.

**Local Butcher verification, November 18, 2026:**

- 15 real Celix ledger known-answer cases passed.
- 19 invalid-schema, missing-pin or wrong-service cases failed closed.
- 260 real Python-vs-Celix ledger results matched.
- Existing security command remained functional.
- This extension remains unmerged; exact-head GitHub Actions are currently
  blocked by a GitHub account billing lock, not by a test assertion.

## Explicit non-goals

This service cannot append, reconcile, edit or publish ledger records.
It does not determine whether posted/blocked terminal evidence is authentic.
It does not dispatch scans, choose agents, execute models, read secrets,
observe a live source checkout, decide if a vulnerability is real, or promote
a native result to production authority. The complete Watcher orchestration
and persistence remain Python-owned. No performance advantage is claimed for
the per-request Celix framework startup.

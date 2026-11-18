# Native Celix cooldown and agent-routing shadow services

**Status:** unmerged draft stacked on the native Watchcore ledger PR #26.
The existing Python Atlas Watcher retains all admission, persistence, agent
execution, GitHub posting, and production routing authority.

## What runs in Celix

This revision adds two separately registered, versioned C ABI services in the
same operator-SHA-256-pinned Celix bundle as the earlier security and ledger
services. All service registrations are synchronous and rollback on failure;
stopping the bundle unregisters all services.

| Service | ABI | Original untouched Watchcore C++ functions |
| --- | --- | --- |
| `org.tronix.watchcore.cooldown` | 1.0.0 | `active_cooldown`, `record_cooldown` |
| `org.tronix.watchcore.routing` | 1.0.0 | `pick_agent`, `roles_ready_at`, `should_stop_pass`, `should_skip_item` |

The sole C++ algorithms are the original `integrations/atlas-watchcore`
snapshot, preserved byte-for-byte. No alternative routing algorithm is
implemented in the ABI adapter or host.

The host `tronix_watchcore_shadow` now accepts opt-in `cooldown` and
`routing` subcommands in addition to its existing `security` and `ledger`
commands. Both read one JSON object from stdin (not an untrusted pathname),
validate a 64-KiB input ceiling, validate all required fields, reject unknown
or duplicate fields, and cap the sizes of all arrays. Both start a **real**
Celix framework, verify and privately stage the pinned bundle, call the
named service through Celix, and destroy the framework before returning a
numeric-only observation. No direct-library fallback or default shadow
enablement exists.

## Cooldown input and output

Every field is required. `op` is `active` or `record`.
`states` is an operator-normalized snapshot of at most 128 vendor entries,
where `vendor: "*"` preserves the legacy global cooldown rule. Dates use
Unix epoch seconds, finite numeric values only; reasons are intentionally not
accepted. `limit.reset` may be null; `strong` is an actual JSON boolean.

```json
{
  "op":"record",
  "vendor":"alpha",
  "now":1100,
  "states":[{"vendor":"alpha","until":2000,"set_at":900,"consecutive":1}],
  "limit":{"reset":null,"strong":false},
  "policy":{"base_seconds":3600,"max_seconds":21600,
            "strong_reset_max_seconds":691200}
}
```

Output:

```json
{"has_state":true,"until":8300,"set_at":1100,"consecutive":2}
```

For `active`, absent or expired states return `has_state:false` and null
timestamps. For `record`, the new state is returned **without writing it**
to the cooldown file. The operator-normalized `strong` flag is sourced
upstream of this service from the Python classifier. It cannot be promoted
from untrusted text inside the bundle.

## Routing input and output

`op` is `pick` or `roles`. There may be at most 128 preflighted agents,
128 cooldown states, 32 roles, and 128 exclude/seen entries. Each role is
a bounded, half-open span into the `agents` array.

```json
{
  "op":"pick",
  "now":1000,
  "agents":[
    {"name":"a","vendor":"alpha","available":true},
    {"name":"b","vendor":"beta","available":true}
  ],
  "states":[{"vendor":"alpha","until":2000,"set_at":900,"consecutive":1}],
  "exclude":[],
  "seen":[],
  "require_unseen":false,
  "roles":[]
}
```

Output:

```json
{"selected_index":1,"reason":"none","ready_at":null,
 "stop_pass":false,"skip_item":false}
```

A missing candidate produces `selected_index:-1` and
`reason:"cooldown"` or `"missing"`. Only normalized **indices** are returned;
the host never echoes arbitrary agent names. The `roles` operation accepts
a nonempty `roles` list (empty is permitted for the equivalent native empty
role state) and requires empty `exclude`, empty `seen`, and
`require_unseen:false`; it returns `ready_at`, `stop_pass`, and
`skip_item`. Importantly, a role can have an available candidate and also
a cooling candidate; `roles_ready_at` retains the upstream Python rule
of considering cooling candidates even when the role is otherwise runnable.

The `available` flag is an operator-controlled **result of preflight**,
not a claim that the service has verified any binary or contacted an external
provider. All timing is explicit and deterministic; the service does not
read the wall clock itself.

## How to validate

Given the repository's existing pinned Celix and msgpack-c source trees:

```sh
cmake --build build --target tronix_watchcore_security_bundle \
  tronix_watchcore_shadow tronix_watchcore_reference \
  tronix_watchcore_unit_tests tronix_demo_bundle -j 4

python3 tests/watchcore_celix_routing.py \
  "$(realpath build/tronix_watchcore_shadow)" \
  "$(realpath build/tronix_watchcore_security.zip)" \
  "$(realpath build/tronix_demo.zip)"
```

Optionally, in an owner-controlled environment with the Rizzer MR !92
branch checked out:

```sh
python3 tests/watchcore_celix_routing.py \
  "$(realpath build/tronix_watchcore_shadow)" \
  "$(realpath build/tronix_watchcore_security.zip)" \
  "$(realpath build/tronix_demo.zip)" \
  /absolute/path/to/rizzer-mr92-checkout
```

That optional parity test **imports the actual Python**
`atlas_watcher.cooldown` and `atlas_watcher.agents` modules. It substitutes
only acquisition dependencies: current clock, cooldown file, binary preflight,
agent→vendor metadata, and role candidate lists. All Python decision
functions remain unchanged. Stop/skip references derive directly from the
actual Python `agents.pick` output for each normalized role.

Locally exercised on Butcher, November 18, 2026:

- 10 real Celix cooldown known-answer cases.
- 12 real Celix routing known-answer cases.
- 34 malformed-input/pin/symlink/wrong-service cases rejected.
- 240 cooldown decisions matched the real Python cooldown functions.
- 300 routing picks matched the real Python `agents.pick` function.
- 160 role-readiness matches to the actual Python `roles_ready_at` function;
  derived stop/skip flags also matched Python pick outcomes.

All figures refer to *local* execution, not passing hosted GitHub Actions.

## Non-goals and review gates

No cooldown file writes, agent launching, network calls, model calls, git
actions, posting, auditing, ledger authority transfers or binary health probes
are permitted. This is an **observation-only service** with per-request Celix
framework startup, not a warmed daemon and not a performance breakthrough.

The existing Rizzer MR !92 Python security shadow remains unchanged.
There is no automatic Python cooldown/routing shadow adapter in this revision;
only an explicit reproducible cross-project differential test. Promotion or
any use of Celix decisions to drive actual Watcher work requires independent
review, CI evidence, and explicit owner authorization.

The native CI workflow runs only the source-local test without a private
Rizzer checkout. Its GitHub Actions execution remains subject to the known
GitHub account billing lock; do not label an unstarted run as passing.

## Canonical-source follow-on (stacked draft)

The source-only NeonOxide distribution contract is described in
[docs-watchcore-neonoxide-source.md](docs-watchcore-neonoxide-source.md).
Public TRONIX retains the unchanged snapshot and independently verifies its
pinned source hashes before native CI. This does not enable a new service,
new capabilities, promotion, deployment, or source-code deletion.

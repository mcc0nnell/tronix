# TRONIX v0.1 — experimental native agent runtime

**Status:** native Celix scheduler and demo adapter built and smoke-tested on the
isolated Butcher development host. A restricted CDP observation adapter has also
been exercised against a dedicated isolated Chrome fixture; it is not deployed
as a production gateway. Not installed as a system daemon or production-ready.

TRONIX borrows specific _scheduling ideas_ from TRON / μITRON, not implementation
code, TRON branding rights, or hard-real-time guarantees. It currently consists of:

- `tronixd`: C++17 process hosting a real Apache Celix framework.
- `tronix_scheduler.zip`: native C Celix scheduler **service bundle**.
- `tronix_demo.zip`: separately packaged, registered Celix operation **service bundle**.
- v1 MessagePack over length-prefixed private AF_UNIX sockets. Chrome CDP remains JSON.
- Native agent lifecycle events and receipt lookup, with opt-in owner-private
  append-only MessagePack journal for restart inspection. **No arbitrary agent
  tools, shell execution, model loop, or prompt memory were added.**
- One event-driven dispatch worker (generation-guarded condition variable,
  no 1ms idle polling); two trusted lanes, up to 4 interactive operations
  before background progress, per-agent fair-share selection.
- Monotonic queue-wait deadlines, including expiry accounting while another operation
  is active; fixed-capacity admission (128 global pending, 32 per agent), one active
  operation, strict request shapes, request-ID collision rejection, result lookup
  and explicit acknowledgment. The Unix listener backlog is 128 connections.
- Trusted adapter-defined execution budgets. On completion, the runtime accounts
  for elapsed execution and flags over-budget work as `overrun` (without returning
  its value). This is **non-preemptive**: already-performed side effects are not
  canceled, and a hung bundle can still block the worker.
- The synthetic adapter supports `demo.constant` -> 42, `demo.pause` -> 1 after
  15ms, `demo.slow` -> 1 after 150ms, and `demo.overrun` -> a 150ms
  operation with a trusted 20ms execution budget for regression testing.
- An optional reviewed Celix CDP bundle supports only `browser.constant` -> 42
  through the existing isolated WindAnvil browser fixture. **No raw CDP or arbitrary
  shell commands are reachable from this interface.**

## Reproduce the build

Prerequisites: Linux, CMake >=3.20, Ninja, C/C++ compilers, the required Celix
build dependencies (Jansson, libzip, etc.), Node 22, npm. The tested Celix and
MessagePack source revisions were:

- Apache Celix `270c784d20dabd0b1f6418c7ee2195822ec9ef90`
- msgpack-c `e1b8bc889d0c16714fc9236af4b954cbfa182be4`
- @msgpack/msgpack v3.1.3 for the optional Node client/tests.

On Butcher (existing pinned dependencies under the WindAnvil worktree):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/home/commander/windanvil-msgpack-work/.deps/local \
  -DCELIX_SOURCE=/home/commander/windanvil-msgpack-work/.deps/apache-celix \
  -DMSGPACK_SOURCE=/home/commander/windanvil-msgpack-work/.deps/msgpack-c
cmake --build build --target tronixd tronix_scheduler_bundle tronix_demo_bundle -j 3
npm ci --offline --ignore-scripts
npm test
```

On another machine, supply your own pinned source checkouts and library
dependencies. There is no provisioning script for those dependencies yet.

Run the daemon only against a private disposable directory:

```sh
mkdir -p ./private && chmod 700 ./private
./build/tronixd --socket "$(pwd)/private/tronix.sock" \
  --scheduler "$(pwd)/build/tronix_scheduler.zip" \
  --adapter "$(pwd)/build/tronix_demo.zip" \
  --journal "$(pwd)/private/events.mp"
```

`TRONIX_READY` is emitted after Celix services are registered and the local
socket is listening. The socket is chmod 0600, must not already exist, and is
removed on orderly shutdown. **Do not expose the daemon as a public service.**

## Opt-in supervised execution

Tronix now supports an optional one-shot process supervisor for reviewed
Celix operation bundles. The worker has a wall-time termination path, can
survive a crash without bringing down the daemon, and preserves the existing
bounded job/receipt protocol. This is fault containment, NOT sandboxing
untrusted native code. See [Supervised execution](docs-supervision.md)
for startup, test coverage, timing allowances and security limitations.

## Opt-in UID-bound capability policy (Linux pilot)

Create a private owned policy file with mode 0600 in a directory that only
the daemon owner can modify.  For a demo-only identity and capability:

```sh
printf 'uid %s\nallow demo.constant\n' "$(id -u)" > ./private/peer.policy
chmod 600 ./private/peer.policy
# Add --policy "$(pwd)/private/peer.policy" to the tronixd command.
```

The grammar is one `uid N` directive followed by one or more `allow operation.name`
directives.  No wildcards or implicit grants.  A valid file is at most 4096
bytes, owner-only, regular, and not a symlink.  The UID must match the daemon
owner.  Invalid or empty policy files fail startup.

When `--policy` is provided, the daemon checks the Linux kernel's `SO_PEERCRED`
UID for every AF_UNIX client, rejects other UIDs before reading their request,
requires the submitted agent label to be exactly `uid:N`, denies unlisted
operations before scheduling, and only advertises allowlisted capabilities.
Use `node tests/peer-policy.mjs` to reproduce policy boundary tests.

**Scope:** This is one Unix principal, not independently authenticated
agents.  All processes sharing that UID share authority over jobs, events,
and receipts, and an in-process Celix bundle still executes with daemon
privileges.  The socket stays mode 0600.  No process sandboxing,
multi-tenant isolation, preemption, or external attestation is provided.
Without `--policy`, legacy lab clients remain supported and can claim
arbitrary agent labels; do not present that mode as authenticated.

## Protocol v1

A request is a strict MessagePack map. Every frame is:

```text
uint32 big-endian length (1..4096) | MessagePack map
```

Each connection carries exactly one request and one response. The request
must contain exactly the documented keys, with no caller-provided `lane`,
`command`, `url` or JavaScript.

```js
{v:1,action:"submit",id:"job-1",agent:"fixture-a",
 operation:"demo.constant",deadlineMs:1000}
// -> {v:1,ok:true,id:"job-1",state:"queued"}

{v:1,action:"result",id:"job-1"}
// -> {v:1,ok:true,id:"job-1",state:"completed",dispatchSequence:1,value:42}

{v:1,action:"ack",id:"job-1"}
// -> {v:1,ok:true,state:"acked"}
```

Other actions: `ping`, `capabilities`, `snapshot`.
Results include `queued`, `running`, `completed`, `failed`, `expired`, or `overrun`.
Terminal results consume one of 256 bounded slots until acknowledged. Duplicate
IDs are rejected while retained. Deadlines apply **only until dispatch**, not
to in-progress work. Expiry is recorded when the scheduler is next accessed,
including through `result` or `snapshot` while a job is running; it is not
an independent real-time timer. The v1 MessagePack wire format is unchanged;
results now also expose `budgetMs`, `queueWaitNs`, and `executionNs` for known
jobs. `snapshot` adds `overrun` and cumulative/max execution and queue-wait
nanoseconds. Celix service ABI **2.0.0** intentionally rejects old adapter bundles.

The optional JavaScript `TronixClient` is in `src/client.mjs`. It is an
IPC client, **not** the execution or scheduling runtime.

## Native lifecycle and receipts (first agent-kernel slice)

The optional `--journal /absolute/private/events.mp` enables native
append-only MessagePack receipts. No journal is required for existing
deployments: without it, events and receipts exist **only in memory**.

- `{v:1,action:"events",after:0,limit:16}` returns ascending events
  (`queued`, `running`, `completed`, `failed`, `overrun`, `expired`,
  `acked`, `interrupted`) and `lastSeq`, `oldestSeq`, `truncated`, `persisted`.
  `limit` is strictly 1..16, and the in-memory window is the latest 512
  events. An expired queued job is reconciled with the Celix scheduler when
  clients request events, results, or acknowledgement.
- `{v:1,action:"receipt",id:"job-1"}` retrieves the last observed status
  even after acknowledgement; completed values are retained in the receipt.
- The native event ledger writes an owner-only, non-symlink, single-writer file
  and synchronizes each appended event. It retains a hard 16 MiB journal cap
  with 1 MiB reserved for already-admitted transitions; further submissions
  receive `LEDGER_UNAVAILABLE` when the admission threshold is reached.
  Rotate/archive offline before that limit; online compaction is not implemented.
- On restart, unacknowledged `queued` or `running` records become
  `interrupted`. **Nothing is automatically replayed**, because a stopped
  operation may have performed side effects. Durable run IDs cannot be reused
  across process restarts, even after acknowledgement.
- A torn final journal frame is truncated during recovery; invalid complete
  frames and concurrent writers fail closed. The private journal is not an
  authenticity signature or external attestation.

**Evidence boundary:** the scheduler and ledger are not one transactional
system. A crash between the scheduler transition and its journal write can
leave the last recorded state incomplete; `interrupted` means the result is
unknown, not that the tool was canceled or rolled back. Journal I/O errors
are reported to stderr, but already-admitted operations may still execute.
Only deploy in a single-user, reviewed-bundle environment.

This is the first persistent **execution** layer, not OpenClaw-equivalent
agent orchestration. Persistent prompt/fact memory, model planning,
execution-process isolation, independent agent authentication, and event
subscriptions over multiple nodes are **not implemented**.

## Security and limitations

- Single local user and trust domain. Without `--policy`, caller-supplied
  `agent` labels are unauthenticated. With `--policy`, the Linux peer UID is
  verified, but all processes under that UID still share job/receipt authority.
  No independent agent identities, browser context separation, per-process
  ACLs, capability leases, or persistent credentials.
- The daemon loads the reviewed adapter bundle in-process for admission
  even if worker supervision is enabled. A supervised worker uses the
  same Unix UID, not a security sandbox. Only load reviewed bundles.
- No crash-safe queue, transactional scheduler/journal boundary, automatic
  job replay, or OS scheduler priority. Without `--journal`, a crashed daemon
  loses its results; with a journal, it retains best-effort run receipts and
  labels unresolved runs `interrupted`.
- Queue-wait deadlines end at dispatch. Without the supervisor, execution
  budgets are assessed after return and execution is non-preemptive. With
  supervision, the child has a budget-plus-startup-grace termination limit,
  not a hard real-time guarantee, rollback, or safe cancellation of prior
  side effects.
- Celix can register/unregister services, but **runtime hot-reload is not
  implemented** in this version.
- The current event loop accepts one bounded client request at a time. It is
  a lab prototype, not an optimized high-concurrency server.
- The smoke/hardening tests cover the native demo adapter, deadlines, and
  shutdown. The optional CDP integration test covers a dedicated owned Chrome
  target on Butcher; it does not establish browser isolation, multi-host
  correctness, production throughput, or real-time guarantees.

## Verification

Run `npm test` for the native smoke suite, lifecycle journal and regressions.
The agent-ledger test includes restart, forced crash, cursor pagination,
scheduler expiry reconciliation, overrun receipts and single-writer locking.
In particular,
`tests/hardening.mjs` confirms expiry during busy execution, execution-budget
accounting and client-side override rejection, and prompt idle shutdown.
Run `npm run test:bench` to measure an optional 64-job/8-agent burst with a
synthetic 150ms blocking operation. It prints observed p50/p95 timings rather
than asserting hard real-time latency guarantees.

The opt-in browser fixture can be tested with `TRONIX_GATEWAY_MODULE` pointing
at an absolute, reviewed WindAnvil CDP gateway module and `npm run test:cdp`.
It uses a disposable dedicated Chrome target on loopback; do not point it at
an ordinary signed-in browser profile. The test writes local, unsigned
receipts under `evidence/`.

## Next engineering milestone

The next milestone is credential and filesystem containment beyond the
current child process termination. Measure wakeup jitter,
large-burst fairness, and backpressure on multiple systems. Add atomic scheduling/receipt transitions, identity-bound admission, and bounded-memory provenance before any
multi-user or embedded production use.


## Thread telemetry prototype (local-only)

See [docs-thread-pilot.md](docs-thread-pilot.md) for the isolated OpenThread
POSIX simulation, native Celix adapter, and end-to-end test. Build the
optional `tronix_thread_bundle` target and run `npm run test:thread`.
It exposes read-only `thread.nodes_up` and `thread.leaders` counts over
private sockets. This is neither a LoRaWAN gateway nor a deployed radio network.

## Lightpanda fork backend (opt-in, local fixture)

Tronix can run its existing, restricted `browser.constant` Celix operation
against a **pinned Lightpanda executable from
[mcc0nnell/browser](https://github.com/mcc0nnell/browser)** instead of the
Chrome/WindAnvil fixture. The backend is
`src/lightpanda-bridge.mjs` and does not alter the native scheduler ABI,
public request schema or existing Chrome CDP bridge. It runs the browser as a
separate child process, observes an operator-owned, ephemeral HTTP fixture on
`127.0.0.1`, checks for a fixed DOM marker, and returns the existing numeric
value 42 to the Celix adapter. The child has a hard 2.2-second timeout.
The fixture server is part of `tests/lightpanda-live.mjs`.

Build the existing `tronixd`, `tronix_scheduler_bundle`, and
`tronix_cdp_bundle` targets as documented above, then install Node
dependencies. Set `TRONIX_LIGHTPANDA_BINARY` to an absolute local
Lightpanda executable and `TRONIX_LIGHTPANDA_SHA256` to **the reviewed
binary's** SHA-256 (compute with `sha256sum` after identifying its build
provenance). Run:

```sh
TRONIX_LIGHTPANDA_BINARY=/absolute/reviewed/lightpanda \
TRONIX_LIGHTPANDA_SHA256=<64-lowercase-hex-digest> \
npm run test:lightpanda
```

The test starts a new loopback-only fixture and a private `0700` bridge
directory with a `0600` Unix socket. It verifies the configured browser
hash before starting, rejects non-allowlisted operations, runs three actual
Lightpanda fetches through the native Celix adapter, checks completion
receipts and drains the queue, then shuts down. It does not touch logged-in
browser profiles or navigate arbitrary public sites.

**Security/evidence boundaries:** This is a bounded browser *execution
backend*, not general agent browser control or a proof of isolation. The
request protocol still exposes only `browser.constant`; no URLs,
JavaScript, selectors or shell commands are accepted from clients. The
admin-configured URL must be `http://127.0.0.1:PORT/`. It is intended for
a static, trusted local fixture. Run it in a single-user isolated
development environment: same-UID processes can access the private sockets,
and the Celix budget remains post-hoc even though the browser child is
timed out. A chosen digest validates the executable bytes but does **not**
attest the build origin or make the binary safe. The test does not prove
public-site compatibility, production security, Chromium parity or
accessibility conformance.

The separate
[lightpanda-agent-bridge](https://github.com/mcc0nnell/lightpanda-agent-bridge)
already supports richer CDP actions and sanitized record/replay. Connecting
those features to Tronix should require a new, explicitly approved operation
contract, destination and session policy, stronger process containment, and
structured results rather than overloading this fixed integer-valued probe.

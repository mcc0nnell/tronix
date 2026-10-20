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
  append-only MessagePack journal for restart inspection.
- Opt-in **supervised Celix execution workers**: one independent process per
  trusted operation, time-bounded and reaped after success, crash or hang.
- Opt-in durable agent key/value memory: 512 total keys, owner-private
  MessagePack append journal, bounded pagination and restart replay.
  **No arbitrary shell tools, model planner or semantic/vector memory.**
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
  rolled back. In default in-process mode a hung bundle can still block the
  worker; with `--worker`, a supervised child can be killed and reaped.
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
cmake --build build --target tronixd tronix_worker tronix_scheduler_bundle tronix_demo_bundle -j 3
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
  --journal "$(pwd)/private/events.mp" \
  --worker "$(pwd)/build/tronix_worker" \
  --memory "$(pwd)/private/agent-memory.mp"
```

`TRONIX_READY` is emitted after Celix services are registered and the local
socket is listening. The socket is chmod 0600, must not already exist, and is
removed on orderly shutdown. **Do not expose the daemon as a public service.**

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

## Native agent-kernel slices

See [docs-agent-runtime.md](docs-agent-runtime.md) for the supervised process
mode, persistent per-agent key/value memory, failure behavior, capacity limits,
and current trust boundary.

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
agent orchestration. The optional local key/value memory store and opt-in
worker subprocess mode are implemented, but prompt composition, semantic
memory, model planning, multi-user authentication, and event subscriptions
across multiple nodes are **not implemented**.

## Security and limitations

- Single local user and trust domain. The string `agent` is not authenticated.
  No multi-user isolation, capability leases, browser context separation, ACLs,
  or persistent credentials. Other processes with the same Unix UID may access
  its socket and read or acknowledge other work.
- The daemon still loads the reviewed adapter to query its authorization and
  budget policy. Default execution also runs in-process. Opt-in `--worker`
  moves `execute` calls out of process; neither mode is a sandbox for
  untrusted bundle code, and the child retains the daemon's UID and network
  privileges. Only load reviewed bundles.
- No crash-safe queue, transactional scheduler/journal boundary, automatic
  job replay, or OS scheduler priority. Without `--journal`, a crashed daemon
  loses its results; with a journal, it retains best-effort run receipts and
  labels unresolved runs `interrupted`.
- Queue deadlines apply only to time spent waiting for dispatch. Without
  `--worker`, execution is non-preemptive and budgets are assessed afterward.
  With `--worker`, the supervisor terminates and reaps children whose
  wall time exceeds the trusted budget plus 200ms startup allowance. Neither
  mode provides rollback, a syscall sandbox or hard real-time guarantees.
- Celix can register/unregister services, but **runtime hot-reload is not
  implemented** in this version.
- The current event loop accepts one bounded client request at a time. It is
  a lab prototype, not an optimized high-concurrency server.
- The smoke/hardening tests cover the native demo adapter, deadlines, and
  shutdown. The optional CDP integration test covers a dedicated owned Chrome
  target on Butcher; it does not establish browser isolation, multi-host
  correctness, production throughput, or real-time guarantees.

## Verification

Run `npm test` for the native smoke suite, lifecycle journal, worker
supervision, durable agent memory and regressions.
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

The next milestone is **identity-bound admission and stronger worker sandboxing**
(user namespaces, seccomp, filesystem/network restrictions), plus crash-safe
scheduler/receipt atomicity. The optional supervised process already kills hung
workers, but does not undo side effects. Measure wakeup jitter, large-burst
fairness, and backpressure on multiple systems. Add atomic scheduling/receipt transitions, identity-bound admission, and bounded-memory provenance before any
multi-user or embedded production use.


## Thread telemetry prototype (local-only)

See [docs-thread-pilot.md](docs-thread-pilot.md) for the isolated OpenThread
POSIX simulation, native Celix adapter, and end-to-end test. Build the
optional `tronix_thread_bundle` target and run `npm run test:thread`.
It exposes read-only `thread.nodes_up` and `thread.leaders` counts over
private sockets. This is neither a LoRaWAN gateway nor a deployed radio network.

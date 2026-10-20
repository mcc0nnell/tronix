# Tronix native agent kernel — experimental v0.3

This adds two **opt-in** features to the original C++17 / Apache Celix
scheduler. It does not implement an OpenClaw clone or a model-driven planner.

## Supervised Celix operation workers

With `--worker /absolute/path/to/tronix_worker`, each admitted operation
executes in a new `tronix_worker` process rather than calling the operation
bundle's `execute` function from the daemon. Scheduling, fair-share selection,
request admission, receipt collection and the **cached policy snapshot**
remain in the main native Celix daemon. **Adapter loading, initialization,
capability enumeration and policy callbacks are in subprocesses only** when
`--worker` is enabled.

1. Before the daemon opens its socket, it starts a bounded inspection
   subprocess. This child loads the reviewed Celix adapter, enumerates at most
   16 uniquely named operations and returns their trusted lane and execution
   budget in a strict MessagePack policy snapshot. The daemon refuses startup
   if the policy inspection fails. This snapshot is immutable until restart.
2. The scheduler claims a job; the daemon emits a `running` lifecycle record.
3. The supervisor launches a separate executable via `posix_spawn` (no
   multithreaded-daemon `fork`), passing the reviewed adapter path, allowed
   operation, and exact **expected lane and budget** from the startup snapshot.
4. The execution child loads the adapter in its own Celix framework, verifies
   that the operation is still listed and the lane/budget **exactly match** the
   startup snapshot, then executes it. Drift fails closed without executing.
   A short bounded MessagePack result returns on a private pipe.
5. The parent supervises wall-clock runtime and reaps the child. The ceiling
   includes a **200ms process-startup allowance** beyond the adapter's trusted
   execution budget; the recorded execution budget has no allowance.
6. On timeout, it sends SIGKILL to the child's process group (on Linux systems
   supporting POSIX_SPAWN_SETSID), kills the child directly as fallback, and
   waits for termination. The parent also cleans up the child's disposable
   Celix cache directory even if the child crashed.
7. The existing scheduler classifies an elapsed-time excess as `overrun`
   without exposing any success value; a faster abnormal child exit becomes
   `failed`. A subsequent queued job can execute after the failed child.

The child applies per-process resource ceilings (1 GiB address space, 64 file
descriptors, 5 CPU seconds, zero core dump) and sets a parent-death signal.

**Limitations:** This is **failure containment, not sandboxing**. The child
retains the daemon's Unix UID, filesystem rights, inherited environment and
network privileges. The main daemon does not activate the adapter when
supervision is enabled; however **same-UID worker code is not security
isolated**. No seccomp, namespace, cgroup, separate UID, or filesystem/network
confinement is implemented. Local code and metadata on disk can change between
inspection and execution; the child compares names, lanes and budgets, but
there is no signed artifact identity or containment for same-UID processes.
Child operations can have external side effects before being killed; timeouts
do not roll them back. The daemon still processes one active operation at a time.

The default behavior without `--worker` remains the in-process execution
path, for backward compatibility. Do not expose a daemon or socket publicly.

## Optional Landlock v1 confinement

With `--worker /absolute/tronix_worker --sandbox landlock`, both adapter
inspection and execution subprocesses must install the same fixed Landlock
filesystem/TCP rules before loading adapter code. The main daemon is not
restricted. See [docs-agent-sandbox.md](docs-agent-sandbox.md). An unsupported
kernel or failed rule installation blocks startup; no unconfined fallback.

## Optional UID-bound admission

The Linux-only `--authority PRIVATE.policy` setting enforces a frozen mapping
of kernel-verified `SO_PEERCRED` UIDs to one agent label and an operation
allowlist. It also filters results, acknowledgements, receipts and memory
access for that agent. **Different processes with the same UID remain
equally authorized.** Socket mode is still 0600, and this feature does not
provision multi-UID clients. See
[docs-agent-authority.md](docs-agent-authority.md).

## Persistent per-agent key/value memory

With `--memory /absolute/private/agent-memory.mp`, the daemon exposes four
MessagePack v1 actions:

```js
{v:1,action:"memory_put",agent:"agent-a",key:"preference",value:"dark mode"}
{v:1,action:"memory_get",agent:"agent-a",key:"preference"}
{v:1,action:"memory_delete",agent:"agent-a",key:"preference"}
{v:1,action:"memory_list",agent:"agent-a",afterKey:""}
```

The memory store is a native, append-only MessagePack journal and indexed
in-memory map. It is owner-private (0600), refuses symlinks, validates file
ownership and exclusive writer lock, syncs changes to disk, and replays valid
records on restart. Interrupted trailing frames are truncated on restart;
malformed complete records fail closed.

Memory keys are 1–64 printable ASCII characters (no spaces); values are
1–512 printable ASCII characters (spaces allowed, no newlines or Unicode yet).
Agent strings are 1–64 visible ASCII characters. There are at most **512 live
keys globally** and a **4 MiB on-disk file cap**; the store does not silently
evict or compact. Key listings are lexically sorted and paginated 16 per page
using `afterKey` from the prior `nextKey` response.

**Security boundary:** Agent names are unverified strings, not identities.
All processes with the same Unix UID accessing the private socket can read
or change other agents' memory. This is a **single-user local lab** store,
not an authorization boundary or secrets vault; do not store credentials.
Data is unencrypted on disk. Disabling `--memory` makes these operations
respond with `MEMORY_DISABLED`. Semantic retrieval, vector embeddings,
layered identity/essential/on-demand prompts and long-term narrative
compaction are **not implemented**.

## Example launch

```sh
mkdir -p ./private && chmod 700 ./private
./build/tronixd \
  --socket "$(pwd)/private/tronix.sock" \
  --scheduler "$(pwd)/build/tronix_scheduler.zip" \
  --adapter "$(pwd)/build/tronix_demo.zip" \
  --journal "$(pwd)/private/events.mp" \
  --worker "$(pwd)/build/tronix_worker" \
  --memory "$(pwd)/private/agent-memory.mp"
```

All feature arguments are optional (and order-independent after the three
required arguments). The daemon is not a production service.

## Verification

`npm test` now runs the original native smoke and hardening checks,
append-only lifecycle/receipt checks, the process-supervision fixture and
memory persistence/restart tests. The supervised fixture explicitly enables
`TRONIX_ISOLATION_FIXTURES=1` for demo-only `demo.pid`, `demo.crash`
and `demo.hang`; these operations are not exposed by default. The supervised
test also records Celix adapter activator PIDs, proves activation never
happens inside `tronixd`, simulates policy drift, and verifies that invalid
adapters cannot start the daemon.
`npm run test:thread` remains an independent opt-in OpenThread simulation.

Remaining reliability gaps: single-worker throughput, atomic scheduler and
receipt state transitions, worker resource utilization reporting, external
attestation, persistent queue/replay with operator review, authentic agent
identity and authority, constrained workers, agent planning and semantic
memory. Treat journal receipts as local evidence, not security attestations.

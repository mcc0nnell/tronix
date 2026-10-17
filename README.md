# TRONIX v0.1 — experimental native agent runtime

**Status:** built and smoke-tested on the isolated Butcher development host. Not installed
as a system daemon, not connected to the live CDP gateway, and not production-ready.

TRONIX borrows specific _scheduling ideas_ from TRON / μITRON, not implementation
code, TRON branding rights, or hard-real-time guarantees. It currently consists of:

- `tronixd`: C++17 process hosting a real Apache Celix framework.
- `tronix_scheduler.zip`: native C Celix scheduler **service bundle**.
- `tronix_demo.zip`: separately packaged, registered Celix operation **service bundle**.
- v1 MessagePack over length-prefixed private AF_UNIX sockets. Chrome CDP remains JSON.
- One asynchronous dispatch worker; two trusted lanes, up to 4 interactive
  operations before background progress, per-agent fair-share selection.
- Monotonic queue-wait deadlines, fixed-capacity admission (128 global pending,
  32 per agent), one active operation, strict request shapes, request-ID collision
  rejection, result lookup and explicit acknowledgment.
- Safe first adapter operations only: `demo.constant` -> 42 and
  `demo.pause` -> 1 after a fixed 15ms sleep. **No raw CDP or arbitrary
  shell commands are reachable.**

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
  --adapter "$(pwd)/build/tronix_demo.zip"
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
Results include `queued`, `running`, `completed`, `failed`, or `expired`.
Terminal results consume one of 256 bounded slots until acknowledged. Duplicate
IDs are rejected while retained. Deadlines apply **only until dispatch**, not
to in-progress work.

The optional JavaScript `TronixClient` is in `src/client.mjs`. It is an
IPC client, **not** the execution or scheduling runtime.

## Security and limitations

- Single local user and trust domain. The string `agent` is not authenticated.
  No multi-user isolation, capability leases, browser context separation, ACLs,
  or persistent credentials. Other processes with the same Unix UID may access
  its socket and read or acknowledge other work.
- Bundle code executes within the daemon's process. A hostile bundle is not
  sandboxed. Only load reviewed bundles.
- No crash-safe queue, WAL, durable receipts, replay, or OS scheduler priority.
  Result memory is bounded and a crashed daemon loses its work.
- Jobs are non-preemptive; deadlines apply to time spent waiting for dispatch.
- Celix can register/unregister services, but **runtime hot-reload is not
  implemented** in this version.
- The current event loop accepts one bounded client request at a time. It is
  a lab prototype, not an optimized high-concurrency server.
- Current tests exercise synthetic adapter operations, **not Chrome**, CDP
  isolation, Omarchy, or multi-host deployment. No production throughput or
  timing guarantee is implied by the local smoke test.

## Next engineering milestone

Provide a browser adapter with a narrow explicit capability contract, connect
it only to a dedicated isolated Chrome target, and benchmark direct CDP versus
TRONIX admission and MessagePack IPC under equivalent load. Keep browser
authorization outside the untrusted request envelope; record verifiable
dispatch/result receipts and apply client identity binding before multi-user use.

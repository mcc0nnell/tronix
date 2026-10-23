# 6lowBang — native C/Celix binary UDP adapter

This milestone removes the OpenThread CLI's ASCII-hex path from **the C/Celix integration test**. It adds a typed Celix UDP service, sends signed binary 6lowBang events between **two independent Celix frameworks in two separate Linux processes**, and admits the received events through the registered typed C caption stream service.

**Transport in this milestone is IPv6 loopback (`::1`), not OpenThread or physical RF.** The earlier OpenThread POSIX two-node CLI test remains documented separately in [the Thread pilot](docs-bang-thread-pilot.md). The next task is to make this native binary adapter work with an actual OpenThread POSIX network interface/stack, not to relabel Linux loopback as Thread.

## Code and service boundaries

- `include/tronix_bang_udp.h`: separately versioned `org.tronix.bang.udp` Celix service interface with explicit `local_port`, `send_event` and bounded `receive_to_stream` callbacks. It never invokes the old integer-returning Tronix job API.
- `src/bang_udp_adapter.c`: registers an opt-in IPv6 datagram socket bound to **`::1` only** (v6-only), rejects untrusted sources outside loopback, exact-decodes bounded binary events via the earlier `tronix_bang_wire` codec and invokes `org.tronix.bang.stream` using a scoped Celix service callback. The stream service performs pinned Ed25519 publisher-key verification and fans out only authenticated events.
- `tests/bang-udp-celix-node.c`: intentionally trusted test harness that loads **both** Celix bundles in each of two processes; a fixed synthetic Ed25519 fixture publisher sends two valid events out of sequence, a replay and a tampered frame. The sender additionally injects an oversized UDP datagram and an invalid header through a test-only raw socket; those paths are not exposed by the signed-event send service. The receiving process pins the corresponding public key, subscribes two local consumers, admits packets and checks ordering, duplicate rejection and signature failure.
- `tests/bang-udp-celix-live.mjs`: creates disposable private Celix caches, tests that activation fails without `TRONIX_BANG_UDP_ENABLE_FIXTURE=1`, spawns the two independent C/Celix processes, and verifies receipts before cleaning up.
- `CMakeLists.txt` and `package.json`: optional native bundle/executable build targets and `npm run test:bang:udp` workflow. The native GitHub Actions test now compiles this optional path using the same pinned 6lowStick C library.

The transport is **opt-in and synthetic**. `TRONIX_BANG_UDP_ENABLE_FIXTURE=1` plus a valid `TRONIX_BANG_UDP_BIND_PORT` are required even to register it. The test binds port 0 so the OS chooses an ephemeral port and transmits only to loopback port numbers 1024–65535. The receiving service obtains the receipt time **after** receiving the datagram, not before its bounded socket poll.

Messages may be unauthenticated until presented to the stream service. Merely receiving bytes never constitutes a caption receipt; unsigned or forged events cannot be delivered to subscribers. Datagram reads use `MSG_TRUNC` to reject oversized packets rather than silently accepting a prefix. The socket handler is synchronous and bounded by a caller-specified polling timeout capped at 1500 ms; no receive thread or unbounded queue is added.

## Build and test

Pin 6lowStick at `28a664c907d25934b92f3f91055ed8ba7f5b9d6a`, supply the reviewed Celix/msgpack-c sources, configure `-DTRONIX_BANG_SOURCE=/absolute/path/6lowstick/native/bang`, and run:

```sh
cmake --build build --target tronix_bang_stream_bundle tronix_bang_udp_bundle bang-udp-celix-node -j 2
npm ci --ignore-scripts
npm run test:bang:udp
```

No microphone, arbitrary radio command, host network routing, privileged port, external IP target, key enrollment endpoint or background auto-start is introduced. The network bridge is accessible only to other processes on the same host, but **loopback does not equal strong process isolation**. End-to-end caption confidentiality, per-device authorization and production key provisioning remain unfinished.

## Verification — October 23, 2026

On Butcher, all three consecutive independent-process Celix binary UDP runs passed, each with two native Celix processes, six UDP datagrams (two valid signed events, a replay, a forged text payload, a deliberately oversized datagram, and an invalid header), two subscribers, four correctly rejected negative frames, and a deliberate unconfigured-bundle refusal. The evidence is in [the machine-readable record](evidence/bang-udp-celix-20261023.json). Passing the tests demonstrates native typed Celix service activation and loopback binary transport, **not physical Thread radio, reliability at scale, acoustic transcription, human caption receipt or emergency dispatch**.

## Next gate

Replace the lab's Linux `::1` socket with a bounded OpenThread POSIX adapter using actual OpenThread UDP APIs. Exercise the same signed binary C event path between simulated nodes and across simulated fragmentation and partitions; measure loss, repeats, out-of-order behavior and process-side latency. Production radio framing must account for 802.15.4 MTU and security overhead. Never declare real RF reception without independent physical hardware results.

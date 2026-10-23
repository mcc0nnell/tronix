# Tronix × 6lowBang: signed caption transport over OpenThread POSIX

This is a two-node software-radio simulation transporting native C11 signed 6lowBang events over actual OpenThread UDP. It does not use physical RF, a phone, microphone, LoRa, or emergency networks.

## Implementation

- `src/bang_wire.c` and `include/tronix_bang_wire.h` define a bounded binary event envelope containing version, channel, language, source timestamp, expiry, 128-bit session, sequence, text and 64-byte Ed25519 signature. Parsing never authenticates on its own: pass decoded events through lib6lowBang.
- `src/bang_thread_codec.c` creates two explicitly synthetic signed events and verifies received events using the existing C tronix_bang_hub, including fanout to two subscribers, out-of-order reconstruction, replay rejection, forgery rejection and an explicitly suppressed-message gap.
- `tests/bang-thread-live.mjs` spawns two real OpenThread POSIX simulated-radio CLI processes and forms a fresh network. It creates a private temporary working directory and new simulator flash for every run. Earlier reused flash files correlated with failed UDP probes even though the nodes joined the network.
- The OpenThread CLI prints received messages as text. For the lab, the 133-byte binary event is represented as 266 ASCII hex characters in a UDP payload, intentionally larger than one IEEE 802.15.4 PHY frame. This is not an efficient production radio framing protocol.
- Bounded UDP retries (up to 4 attempts) and all actual attempt counts are recorded. There is no invented successful delivery.

## Reproduce

Supply pinned Tronix Celix/msgpack-c dependencies and 6lowStick native/bang from 6lowStick commit 28a664c907d25934b92f3f91055ed8ba7f5b9d6a. Follow docs-bang-pilot.md for the CMake setup.

Build target: bang-thread-codec
Run source-codec test: npm run test:bang:codec
Run full two-node test: set absolute OT_CLI_PATH and BANG_THREAD_CODEC pointing to the compiled binary, then run npm run test:bang:thread.
Optional THREAD_SIM_BASE_ID selects two adjacent unused simulator node IDs, default 27 and 28.

The OpenThread simulator binary is intentionally not auto-downloaded or provisioned by npm. Record its provenance and digest independently. No network-credential secrets, host firewall modifications, physical radio setup or arbitrary remote commands are involved.

## Recorded three fresh-flash trials (October 23, 2026)

| Trial | UDP send attempts for sequence 2 / 1 | Local CLI send-to-receive observation (ms), sequence 2 / 1 |
| --- | --- | --- |
| 1 | 1 / 1 | 2.208 / 4.594 |
| 2 | 1 / 1 | 4.195 / 5.905 |
| 3 | 1 / 1 | 6.414 / 5.294 |

Each completed 9/9 checks. Observations start before CLI command initiation and end at receiver CLI output; they are not RF latency, user-caption delay, reliability statistics, or service SLOs. The lab intentionally withheld sequence 1 in a separate receiver process to test gap reporting; this was NOT real packet loss on the simulated radio. Consult evidence/bang-thread-20261023.json for full checks, build provenance and caveats.

## Integration boundary

Tronix Celix bundle registration was validated in a separate native daemon test. In this UDP lab, the receiving C process invokes the same C caption stream hub directly; no registered Celix service is reached across the two nodes. The original Tronix integer job interface remains unchanged and carries no caption text.

Next steps: a dedicated C/Celix network adapter and authenticated provisioning, then constrained binary framing and backpressure/fragmentation analysis, repeatable fault injection and latency distributions, private-caption encryption, physical-radio testing, and a caption source plus accessible UI. No human receipt or ASR accuracy is demonstrated.

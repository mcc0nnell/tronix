# Tronix × OpenThread (simulation proof of concept)

This is **Thread (IEEE 802.15.4), not LoRaWAN**. The adapter runs an actual
OpenThread POSIX simulation and exposes only read-only network status to a
native Apache Celix operation bundle. The simulated radio uses local UDP
transport. No physical radio, border router, Matter commissioning, LoRa gateway,
customer deployment, or external network is configured.

## Architecture

```text
OpenThread ot-cli-ftd #1 ─┐
                          ├─ simulated IEEE 802.15.4 mesh over local UDP
OpenThread ot-cli-ftd #2 ─┘
          │ read-only fixed CLI commands: state; internal initial commissioning
          ▼
src/thread-bridge.mjs  (private AF_UNIX, fixed MessagePack schema)
          │ read-only thread.nodes_up / thread.leaders
          ▼
src/thread_adapter.cc  (native Celix operation service bundle)
          ▼
tronixd (restricted MessagePack queue over private AF_UNIX socket)
          ▼
local trusted TronixClient
```

### Reproduce on a supported local Linux host

The OpenThread simulator source used for the first build was a separate
local checkout at commit `4b8dae16f`, including unrelated pre-existing
research. **Do not modify that checkout or assume these tests validate
upstream main.** Use a vetted upstream OpenThread checkout for independent
reproduction.

```sh
# Use pinned upstream OpenThread source. Does not require Docker or sudo.
cmake -S /path/to/openthread -B .openthread-sim-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DOT_PLATFORM=simulation \
  -DOT_APP_CLI=ON -DOT_APP_NCP=OFF -DOT_APP_RCP=OFF \
  -DOT_FTD=ON -DOT_MTD=OFF -DBUILD_TESTING=OFF
cmake --build .openthread-sim-build --target ot-cli-ftd -j 2

# Build Tronix as described in the root README. Additionally build:
cmake --build build --target tronix_thread_bundle -j 2

npm run test:thread
# Override if installed elsewhere:
# OT_CLI_PATH=/absolute/path/to/ot-cli-ftd npm run test:thread
```

The integration test uses a mode-0700 temporary directory and a pair of
OpenThread simulated node IDs (25, 26 by default). It starts and stops both
nodes, the bridge, and the native Tronix daemon, then verifies successful
telemetry and that loss of the bridge produces a failed job instead of a
fabricated success. It does not require or modify host firewall rules.

### Security and scope

- Only `thread.nodes_up` and `thread.leaders` are exposed, both background
  scheduled operations. They report integer counts; no raw CLI or packet
  injection. Attempts to submit `thread.raw_cli` are denied.
- Bridge socket lives under an owner-controlled mode-0700 directory and must
  be owned by the runtime UID with mode 0600. The Celix adapter performs a
  mode and ownership check at start and on each request.
- Requests and replies are length-prefixed MessagePack with strict operation
  names and conservative sizes. Response timeout is 2 seconds; errors fail
  closed. The private socket only isolates other local UIDs, **not processes
  sharing that UID**. This is not a multi-tenant security boundary.
- The simulator bridge controls its own locally spawned two nodes. The initial
  dataset may contain credentials; do not log or expose it. The bridge has no
  network commissioning endpoint.
- Prototype limitations: no measurements of RF range, LoRaWAN connectivity,
  hardware reliability, interference, latency SLO, battery life, or production
  accessibility alerts. Not a substitute for certified life-safety systems.
- Tronix currently allows one operation service adapter per invocation.
  This prototype runs the Thread bundle **instead of** the demo or CDP bundle;
  composing multiple adapters is a separate runtime milestone.

## Suggested Chesapeake wireless evolution

1. **Thread**: use an off-the-shelf radio co-processor (RCP) and OpenThread
   Border Router. Keep Spinel/RCP framing within the proven OpenThread stack.
2. **LoRaWAN**: use a separate gateway and ChirpStack (or another authorized
   network server). Do not send Thread frames over LoRa or assume interchange.
3. **Unified Celix telemetry**: normalize health, device identity, delivery
   receipts and data provenance above the two independent link technologies.
4. **Chesapeake control plane**: policy, owner-scoped credentials, audit
   receipts, monitoring, and explicit customer consent.
5. Before any ISP or emergency-service claims, run authorized real-hardware
   coverage, interference and recovery tests and verify regulatory obligations.

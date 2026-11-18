# TRONIX opt-in native verification connection pool

Status: **experimental, draft-only**. This change is stacked on PR #21
(batch verifier), #20 (persistent typed verifier), and #19 (one-shot verifier).
No merge or deployment is implied.

## What changed

The existing `tronix_verify_daemon` now accepts an optional
`--workers N`, with N a canonical single digit from **1..8**.
Default **1** preserves serial operation unless the operator opts in.

- One accept loop feeds at most 32 waiting client connections to a fixed-size
  pool. The Unix listener backlog remains 32. An excess accepted client is
  **closed without a receipt**. No detached or unbounded worker threads.
- Each client receives a single **absolute 2-second deadline beginning at
  admission**, including queue wait, framing, slow-drip input, and response
  writes. Nonblocking poll/send/recv means small periodic writes cannot extend
  that deadline. This replaces per-system-call relative timeouts. The limit
  governs socket responsiveness, not a preemptive CPU or Celix callback budget.
- The pre-existing native Celix SHA-256 verifier bundle is pinned by exact
  operator-supplied digest, copied and validated before service activation.
  The immutable oracle manifest remains startup-frozen.
- Independent requests can now invoke the immutable deterministic service on
  separate worker threads. Aggregate stats are synchronized by a mutex.
  No binary or service ABI changes, no provider calls, no arbitrary command,
  shell, model execution, filesystem publication or automatic Watcher review.
- SIGTERM stops admission, closes queued sockets, wakes idle workers and joins
  active workers before destroying Celix. In-flight operations can complete up
  to their absolute deadline.

Each request still uses **one** exact MessagePack v2 map and returns one
typed result. VERIFIED/REJECTED/UNVERIFIABLE and all deny-authority fields
are unchanged. The `tronixd` v1 scheduler, grant policy, workers and
receipts are unchanged.

## Reproduce

Build with the repository's pinned Celix and msgpack-c revisions:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCELIX_SOURCE=/absolute/path/to/pinned/apache-celix \
  -DMSGPACK_SOURCE=/absolute/path/to/pinned/msgpack-c
cmake --build build --target tronix_verify_daemon \
  tronix_answer_verifier_bundle tronix_demo_bundle -j 3
npm ci --ignore-scripts
npm run test:verification:batch
npm run test:verification:pool
npm run test:verification
npm run license:check
```

Operator invocation (absolute owner-controlled paths as documented in
`docs-persistent-verification.md`):

```sh
./build/tronix_verify_daemon \
  --socket /absolute/private/verify.sock \
  --oracle /absolute/private/oracle.txt \
  --bundle /absolute/reviewed/tronix_answer_verifier.zip \
  --bundle-sha256 EXACT_REVIEWED_BUNDLE_HASH \
  --workers 4
```

## Tested behavior on Butcher (November 17–18, 2026)

- Baseline persistent verifier integration: 22/22 passed before pool.
- Native pool tests: **7/7 passed** with the real pinned Celix service,
  including 3 idle peers vs. a 4th active worker, 72 bounded concurrent
  candidate outcomes, frozen oracle stability, invalid settings, cleanup,
  a single slow-drip client closed at the absolute deadline, and a local
  80-connection flood retaining only a bounded number of daemon descriptors.
  The seven tests passed in three consecutive local runs.
- Existing persistent verifier integration: 22/22 passed after pool.
- Batch helper: 6/6 local unit tests pass.
- License checker: passed.
- Exact-head GitHub Actions may be blocked by the account's billing status;
  local evidence is **not** a substitute for clean CI.

## Performance: no credible throughput speedup claimed

The opt-in benchmark in `tests/bench-native-pool.mjs` uses local Unix sockets,
fixed trusted bytes, SHA-256, an existing Celix service and eight concurrent
client requests. It rotates test order after warming all worker configurations.

The initial sequential single-run test appeared to favor eight workers, but
reversing the order inverted that result. A follow-up **four-round,
1,000-request-per-round** test on Butcher reported:

| Workers | Mean fixture requests/s | Observed range |
| --- | ---: | ---: |
| 1 | 11,915 | 7,661–14,499 |
| 2 | 12,198 | 6,516–14,316 |
| 4 | 10,265 | 7,752–11,649 |
| 8 | 7,794 | 7,056–8,379 |

No increase in sustained throughput was established. Higher concurrency may
**reduce** performance for lightweight SHA-256 work because thread scheduling,
socket setup and shared-service overhead dominate; the host was not idle or
performance-isolated.

**The proven benefit is responsiveness with stalled peers, not faster useful
work.** Production sizing would require load testing with realistic payloads,
queue saturation, resource budgets, process isolation, repeated randomized
runs, error rates and latency under host contention. The program also does
not measure NeonOxide synthesis, Rizzer campaigns, signed WASM, AI inference
cost or security-vulnerability discovery.

## Security boundaries / next gates

The endpoint stays on an operator-owned 0600 Unix socket under a 0700 directory
and restricts peers to the daemon UID via SO_PEERCRED. Processes sharing that
UID are **not isolated from one another**. Native Celix bundles share the
daemon's address space and are **not sandboxed**, even with multiple threads.
The deterministic SHA service is reentrant but the change does not prove that
arbitrary future Celix services will be concurrency-safe.

Do not attach unreviewed or dynamic adapters to the pool or interpret a
VERIFIED byte equality check as objective truth, authorization, or permission
to execute a generated NeonOxide recipe. Future integrations need explicit
typed capability contracts, authenticated evidence and separate process or
WASM isolation with end-to-end adversarial tests.

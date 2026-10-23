# TRONIX and the Celix capability ecosystem

TRONIX is the **bounded local execution supervisor**, not a replacement for the
applications, network transports, or assurance engines around it. This comparison
reflects repository documentation reviewed on 2026-10-23; it is not a claim that
the listed applications are deployed together.

| Source | Demonstrated or proposed capability | Reusable boundary for TRONIX |
| --- | --- | --- |
| [fineract-celix](https://github.com/mcc0nnell/fineract-celix) | OBSERVE-only Celix handlers, explicit operator grants, fixed handler/bundle/config digests, durable authenticated execution evidence | Freeze stable implementation identity before execution; do not trust service ranking to confer authority |
| [SignPipe](https://github.com/mcc0nnell/SignPipe) | Celix engine registry, Google/Microsoft backends, ordered segments, bounded capture/backpressure | Typed, versioned streaming adapter contract **in addition to** the current finite-operation service |
| [iTRS-NG](https://github.com/mcc0nnell/itrs-ng) | Proposed typed accessibility resource resolution and provenance | Separate **selection policy** from service discovery; not a source of deployed dispatch guarantees |
| [Apache Celix](https://github.com/apache/celix) | Dynamic service registration, dependency manager, shell, pub/sub and remote services | Prefer maintained Celix lifecycle mechanisms over bespoke registry/event-loop reimplementations |

## What TRONIX actually implements today

- C++17 `tronixd` hosts native Celix C bundles.
- `org.tronix.scheduler` selects between two lanes with finite queues and
  monotonic queue-wait deadlines.
- `org.tronix.operation` exposes a *static*, trusted, finite set of operation
  names and `int64_t` results, with adapter-defined lanes and budgets.
- MessagePack v1 carries strictly validated requests over private Unix sockets.
- Optional supervised subprocesses improve failure containment, but **do not
  establish a production sandbox**. In-process operations remain non-preemptive.
- Receipts and the optional journal describe local lifecycle. They do not yet
  provide fineract-celix's independent HMAC/checkpoint verification.

See [README.md](README.md) and [include/tronix_service.h](include/tronix_service.h)
for the authoritative current ABI and protocol. Existing experimental IHB,
Landlock, and agent-framework boundary work remains in separate PRs; do not
treat it as merged runtime functionality.

## Component ownership

| Component | Owns | Must not acquire implicitly |
| --- | --- | --- |
| TRONIX | Admission, scheduling, local worker supervision, operation lifecycle receipts | Arbitrary tool authority, radio safety decisions, model output trust |
| 6lowReactor / 6lowStick | Radio/network state, failover, authenticated delivery receipts | Direct host execution permission from a radio packet |
| 6lowBang | Proposed bounded live-text/caption delivery data plane | Scheduling of each caption token as a synchronous `int64_t` Celix job |
| SignPipe | Frame segmentation, recognition, ordered hypotheses and transcripts | TRONIX agent privileges based on model output |
| Rizzer / IHB | Native fuzz campaigns, crash/replay evidence, their own execution limits | Unreviewed targets or Docker access via TRONIX |
| WindAnvil / Blade | Admission decisions and external evidence verification where configured | Implicit trust from a TRONIX `completed` state |

## Next contracts, in order

### 1. Native reproducibility — first gate

`.github/workflows/native.yml` builds **exactly pinned** Apache Celix and
msgpack-c sources, compiles `tronixd`, the supervised worker and fixture
bundles, and runs the existing native-backed `npm test` suite on Linux.
It makes a CI result visible on pull requests; **a workflow file is not
evidence of a passing run**. Keep the separate license workflow.

### 2. Stable capability identity — design, not implemented

Introduce an operator-controlled admission descriptor for each adapter:

```text
operation name and ABI version
bundle symbolic name and version
SHA-256 of exact bundle archive
SHA-256 of reviewed adapter configuration
allowed peer principal and permitted operation
trusted deadline and execution budget bounds
```

Fail closed if the identity differs between admission, supervised inspection
and execution. Record both the selected descriptor and the observed Celix
service identity; do not use Celix service ID, service ranking, or bundle
install location as stable authority. Initially test a single local adapter
using the existing v1 protocol; do **not** copy financial tenancy semantics
or assume TRONIX journals are authenticated merely because Fineract's are.

### 3. Async typed data plane — separate versioned ABI

The current one-active-operation `int64_t` interface cannot deliver continuous
captions or stream-level radio receipts. A future `tronix.stream.v1` should
use opaque bounded bytes with explicit content type, origin, sequence,
deadline, cancellation, backpressure and per-subscriber quotas. Let
6lowBang/6lowReactor own payload transformation and radio dispatch.
Measure tail latency and dropped/expired frames under overload before
attaching real devices. No implicit bridge from model text to tool execution.

### 4. Celix-native lifecycle and readiness

Prototype Celix Dependency Manager for required/optional service activation
and the Components Ready Check bundle. Test registration, withdrawal,
replacement and in-flight work; a missing service must not silently change
the authority of an admitted job. Only then evaluate Celix pub/sub and Remote
Service Admin (RSA) for cross-process transport. RSA's sample discovery policy
is not itself an authorization policy.

### 5. Independent assurance

Keep bounded synthetic fixtures for adapter integration, with Rizzer/IHB
performing controlled native fuzzing and WindAnvil retaining external
verification. Distinguish a scheduler-complete receipt from an application
delivery receipt and from a verified finding.

## Release gates

1. Pinned native build and current tests pass in CI on the actual PR head.
2. Worker termination, budget overrun, queue overload and stale-deadline
   behavior remain reproducible.
3. Any new adapter has an explicit owner, allowlist, frozen identity,
   failure behavior and independent tests.
4. No operational security or radio reliability claims exceed tested
   environment boundaries.

**Out of scope now:** embedding Kafka/Camel in `tronixd`, moving SignPipe or
6lowStick data ownership into TRONIX, enabling an unreviewed remote-service
discovery policy, and enabling container/Docker authority from agent input.

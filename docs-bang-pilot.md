# Tronix × 6lowBang — C/Celix synthetic caption fixture

**Scope:** Optional native Celix bundle linking the authoritative 6lowStick C11 library. This is a process-local synthetic protocol exercise, not an audio or caption streaming service.

## Architectural boundary

Tronix retains scheduling, worker lifecycle, private MessagePack request/receipt flow and fixed operation authorization. 6lowStick owns Ed25519 caption signing, pinned public-key checks, bounded sequence ordering and missing-sequence reporting. The existing `tronix_operation_service_t` can return only `int64_t` results, so **actual caption text must not travel through that interface**. This PR adds no streaming service and no Thread transmit path.

Source files: `src/bang_fixture.c` is independent of Celix and calls the C11 library; `src/bang_adapter.c` exposes exactly two operations through the existing Celix service ABI v2.0.0; `tests/bang-fixture.c` verifies signed event ordering and a missing-message gap; `tests/bang-live.mjs` exercises the real Tronix daemon, authorization, queue and receipts on a provisioned Linux host.

## Build with reviewed dependencies

The integration was written for public 6lowStick PR #32 at full commit `28a664c907d25934b92f3f91055ed8ba7f5b9d6a`. **Pin this exact commit**, not the moving branch. Obtain its `native/bang` source directory and existing pinned Tronix Celix/msgpack-c dependencies. This project does not silently fetch or vendor external source.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/absolute/deps/local \
  -DCELIX_SOURCE=/absolute/apache-celix \
  -DMSGPACK_SOURCE=/absolute/msgpack-c \
  -DTRONIX_BANG_SOURCE=/absolute/6lowstick/native/bang
cmake --build build --target tronixd tronix_scheduler_bundle \
  tronix_bang_bundle tronix_bang_fixture_test \
  tronix_bang_stream_bundle tronix_bang_stream_test -j 2
./build/tronix_bang_fixture_test
./build/tronix_bang_stream_test
npm ci --ignore-scripts
npm run test:bang
npm run test:bang:stream
```

The C library is compiled as a position-independent static dependency of the optional Celix bundle. The bundle is not built or installed by default. `tests/bang-live.mjs` starts a disposable local runtime with `TRONIX_BANG_ENABLE_FIXTURE=1`, registers only the research adapter, and removes its socket afterward. Without that explicit variable, bundle activation fails closed.


## Separate native C caption stream service

\`include/tronix_bang_stream.h\` defines the new \`org.tronix.bang.stream\`
service at **ABI 1.0.0**, intentionally independent of the existing
\`org.tronix.operation\` **2.0.0** integer-returning scheduler ABI.
\`src/bang_stream.c\` implements a thread-safe in-process hub; the Celix
registration adapter is \`src/bang_stream_adapter.c\`.

- Publishers must be pinned by a **trusted in-process caller** before enable.
  Subscribers explicitly opt in with numeric local capabilities.
- Only signed, authenticated and unexpired \`bang_event\` structures may be
  admitted. The underlying pinned-key \`lib6lowBang\` receiver validates them.
- An ordered event is copied into each matching subscriber's FIFO, not into
  the Tronix scheduler result. Up to eight subscribers and 64 queued events
  per subscriber are supported.
- Before signing-state admission the hub reserves room for a full 32-event
  reorder burst in **every** matching subscriber FIFO; when one queue lacks
  this headroom the producer receives \`BANG_CAPACITY\`. No partial fanout or
  silent displacement of a previously authenticated event occurs.
- A caller must poll. \`BANG_NOT_READY\` means no queued event. The service
  does not create a hidden thread, open a transport, expose an inbound socket,
  or deliver a physical/human notification.
- \`flush\` reports signed sequence gaps after a caller-controlled hold;
  missing words are never guessed or synthesized. \`disable\` clears
  ephemeral subscriber queues and signing-session state.

\`tronixd --bang-stream /absolute/tronix_bang_stream.zip\` enables **only
optional in-process Celix registration** alongside the existing scheduler and
one ordinary operation bundle; it does not expose an unauthenticated
MessagePack caption injection API. Both \`--bang-stream\` and the bundle
require \`TRONIX_BANG_STREAM_ENABLE_FIXTURE=1\`. The daemon rejects combining
that fixture with \`--worker\`, \`--authority\` or \`--policy\` until explicit
identity and isolation semantics exist. The separate smoke command
\`npm run test:bang:stream\` verifies registration, refusal when disabled,
the unchanged numeric operation API and denial of caption injection over
the legacy IPC.

**Security limits:** Celix services here are trusted in-process C code;
subscription IDs are local capabilities, not verified user/device identities.
Any other trusted code able to call \`pin\` can provision a publisher key; there
is no deployment-grade enrollment, rotation or revocation yet. Queueing does
not provide an on-air reliability guarantee. Neither signatures nor these
bounded queues encrypt transcripts or authenticate the truth of ASR output.

## Fixed operations

| Operation | Lane | Integer result | Actual meaning |
| --- | --- | ---: | --- |
| `bang.synthetic_delivery` | Background | `2` | Two signed synthetic caption/sound events validated and reordered |
| `bang.synthetic_gap` | Background | `1` | One missing sequence reported after a caller-controlled hold, with no invented text |

Unknown commands including `bang.raw_cli` are denied. Test signing keys are generated randomly in-process and never published. The caller cannot supply unreviewed caption text, keys, radio controls, HTTP destinations or locations through the existing API. These two operations are **not** speech recognition, live captioning or packet transmission.

## Verification and omissions

The `6lowBang fixture` GitHub Actions workflow compiles and tests **both native C test drivers** (the fixed-operation fixture and the bounded caption stream), against pinned 6lowStick source. It does not build or execute the Celix framework, run a radio simulator or establish real delivery latency. Run `npm run test:bang` and `npm run test:bang:stream` after building the native Celix targets on a provisioned Linux host.

The native Celix runtime supports one operation adapter per invocation. Here it replaces the current Thread/demo/CDP adapter. Supervised workers scrub their environment, so the fixture environment gate is not currently forwarded to a worker; the supplied smoke test runs in-process on a trusted, isolated developer host. Do not treat it as a production sandbox or weaken worker policies to make it start.

## Next gate

The **typed, bounded C caption service interface now exists**, but full native Celix service activation has not been validated by GitHub CI. Next: independently build and verify that integration on a provisioned host, then connect an authorized OpenThread POSIX UDP transport, measure process-side latency and loss/reordering, and design a compact radio codec. The existing `docs-thread-pilot.md` is read-only telemetry, not caption delivery. Hardware RF, ASR/translation, encryption of private captions and human comprehension remain unverified.

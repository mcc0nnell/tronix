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
  tronix_bang_bundle tronix_bang_fixture_test -j 2
./build/tronix_bang_fixture_test
npm ci --ignore-scripts
npm run test:bang
```

The C library is compiled as a position-independent static dependency of the optional Celix bundle. The bundle is not built or installed by default. `tests/bang-live.mjs` starts a disposable local runtime with `TRONIX_BANG_ENABLE_FIXTURE=1`, registers only the research adapter, and removes its socket afterward. Without that explicit variable, bundle activation fails closed.

## Fixed operations

| Operation | Lane | Integer result | Actual meaning |
| --- | --- | ---: | --- |
| `bang.synthetic_delivery` | Background | `2` | Two signed synthetic caption/sound events validated and reordered |
| `bang.synthetic_gap` | Background | `1` | One missing sequence reported after a caller-controlled hold, with no invented text |

Unknown commands including `bang.raw_cli` are denied. Test signing keys are generated randomly in-process and never published. The caller cannot supply unreviewed caption text, keys, radio controls, HTTP destinations or locations through the existing API. These two operations are **not** speech recognition, live captioning or packet transmission.

## Verification and omissions

The `6lowBang fixture` GitHub Actions workflow compiles and tests the **Celix-independent native fixture** using the pinned 6lowStick C implementation. It does not build or execute the Celix framework, run a radio simulator or establish real delivery latency. Run `npm run test:bang` after building the native Celix targets to verify the actual runtime interaction.

The native Celix runtime supports one operation adapter per invocation. Here it replaces the current Thread/demo/CDP adapter. Supervised workers scrub their environment, so the fixture environment gate is not currently forwarded to a worker; the supplied smoke test runs in-process on a trusted, isolated developer host. Do not treat it as a production sandbox or weaken worker policies to make it start.

## Next gate

Implement a **separate bounded asynchronous C Celix caption service interface** with channel provisioning, subscriber lifecycle and backpressure. Connect that service to an independently tested OpenThread POSIX UDP transport, measure process-side latency and loss/reordering, and then tackle a compact radio codec. The current `docs-thread-pilot.md` is read-only telemetry, not caption delivery. Hardware RF, ASR/translation, encryption of private captions and human comprehension remain completely unverified.

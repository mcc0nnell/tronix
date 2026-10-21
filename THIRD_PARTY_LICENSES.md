# Third-party software and licensing boundary

Tronix's original source and documentation are licensed under Apache License 2.0.
This does **not** change the rights or obligations associated with separately
licensed projects used to build, test, or run Tronix.

The repository does not vendor the following projects' source trees. Builds or
integration tests may obtain them separately; users who redistribute those
components must retain the upstream license and notices as applicable.

| Component | License | Relationship to Tronix |
| --- | --- | --- |
| [Apache Celix](https://github.com/apache/celix) | Apache-2.0 | Separately checked-out native framework required for builds; not copied into this repository |
| [msgpack-c](https://github.com/msgpack/msgpack-c) | BSL-1.0 | Separately checked-out C++ MessagePack headers required for builds |
| [@msgpack/msgpack](https://www.npmjs.com/package/@msgpack/msgpack) v3.1.3 | ISC | npm dependency; `node_modules/` is ignored |
| [OpenThread](https://github.com/openthread/openthread) | BSD-3-Clause | Optional external POSIX simulation/CLI exercised through a restricted adapter |
| [Lightpanda](https://github.com/lightpanda-io/browser) | AGPL-3.0-only | Optional separately obtained browser executable invoked by a restricted bridge; no Lightpanda source is included |
| [Lightpanda Agent Bridge](https://github.com/mcc0nnell/lightpanda-agent-bridge) | Apache-2.0 | Independently maintained and checked-out browser record/replay component |
| Chrome / Chrome DevTools Protocol | Respective provider terms | Optional browser fixture or protocol integration; Chrome is not distributed here |
| WindAnvil CDP gateway | Separately licensed/private implementation | Optional external gateway module; not incorporated into or relicensed with Tronix |

**Distribution boundary:** Tronix's Apache-2.0 license does not grant rights
to redistribute third-party executables, artifacts, images, or independently
checked-out source. If a distribution includes any external component, check
its precise version, full license text, associated notices, and obligations
before distributing. In particular, do not assume AGPL obligations disappear
merely because an external browser is launched from Tronix.

**Source provenance:** No third-party license or copyright headers were
identified in the original `src/`, `tests/`, or `include/` files inspected
on 2026-10-20. This is a source-header and repository-layout review, not a
legal determination or proof that no external code was ever incorporated.
Contributors must preserve pre-existing third-party notices and obtain the
appropriate rights before adding independently sourced code.

# Tronix → Lightpanda: bounded workflow record/replay

This is a separate, **opt-in** native Celix operation bundle that reuses the
existing [Lightpanda Agent Bridge](https://github.com/mcc0nnell/lightpanda-agent-bridge)
record/replay implementation against the
[mcc0nnell/browser](https://github.com/mcc0nnell/browser) Lightpanda fork.

## Working data path

```text
Tronix MessagePack submit("browser.record" / "browser.replay")
  -> Celix scheduler + native lifecycle journal
  -> tronix_lightpanda_record.zip (bounded, fixed operation set)
  -> private 0600 Unix socket, fixed MessagePack {v:1,op}
  -> lightpanda-workflow-bridge.mjs
  -> fresh supervised Python workflow child
  -> pinned Lightpanda Agent Bridge lightpanda_evidence.py
  -> pinned Lightpanda browser executable via real CDP Unix socket
  -> SHA-256 checked local receipt in owner-private runs/
```

The workflow is **administrator-defined**: extract `#heading` from the initial
page, navigate to `/second` on the same fixture origin, extract `#heading`
again. The fixture must be a static, operator-owned
`http://127.0.0.1:PORT/` web server. A caller cannot choose the URL, selectors,
page scripts, headers, receipt ID, binary, or bridge checkout. Replay targets
only the last successfully recorded workflow in the running sidecar.

The native operations deliberately return `42` on successful record and
`43` on successful replay, preserving the current integer-only Celix service
ABI. Failures return no numeric result and are captured by Tronix as failed
jobs. The Python bridge writes separate `windanvil.browser-receipt.v1` files
that include workflow metadata and **hashes of observations**, not raw page
content. Its receipt `receipt_id` is logged by the local bridge, but
**is not atomically linked to the native Tronix job ID**. Avoid claiming
transactional provenance or independent verification.

## Run on the isolated Butcher development host

Prerequisites: built native Tronix/Celix/MessagePack dependencies from
[README.md](README.md), Node 22, Python 3 with a venv containing
`websockets>=14,<17`, a reviewed Lightpanda Agent Bridge git checkout, and
a compatible Lightpanda executable with Unix-domain CDP support.

Build both new and legacy adapters:

```sh
cmake --build build --target tronixd tronix_scheduler_bundle \
  tronix_demo_bundle tronix_cdp_bundle tronix_lightpanda_record_bundle -j 3
npm ci --ignore-scripts
```

Set these variables to your **actual** approved local paths and pin values:

```sh
export TRONIX_LIGHTPANDA_BINARY=/absolute/path/to/reviewed/lightpanda
export TRONIX_LIGHTPANDA_SHA256="$(sha256sum "$TRONIX_LIGHTPANDA_BINARY" | cut -d' ' -f1)"
export TRONIX_AGENT_BRIDGE_DIR=/absolute/path/to/lightpanda-agent-bridge
export TRONIX_AGENT_BRIDGE_COMMIT="$(git -C "$TRONIX_AGENT_BRIDGE_DIR" rev-parse HEAD)"
export TRONIX_AGENT_BRIDGE_PYTHON=/absolute/path/to/reviewed/venv/bin/python
npm run test:lightpanda-record
```

The test starts a disposable, loopback-only two-page fixture and a private
sidecar; it does not browse public websites or reuse a signed-in user profile.
It exercises all operations **through the native daemon**, validates evidence,
tests changed-page mismatch and tampered-record rejection, checks invalid binary
hash / bridge revision refusals, verifies the native ledger, and cleans up.

See [evidence/lightpanda-record-live.json](evidence/lightpanda-record-live.json)
for the captured local test summary and the corresponding SHA-256 checksum.
Evidence is local and unsigned, not external CI or an attestation.

## Security / reliability boundaries

- Only trusted, single-user local development. A Unix socket with 0600 mode
  does not isolate processes sharing the same UID.
- The browser binary digest and bridge git revision are checked at startup;
  a clean tracked git worktree is required. These checks do not prove the
  binary's build provenance, and untracked/importable modules or a compromised
  host remain a risk.
- A child-process group is killed after 16 seconds; the native adapter has
  bounded 18-second transport timeouts and a 17.5-second post-hoc budget.
  This is **not** a general OS sandbox, and a native plugin still executes
  inside the Tronix daemon.
- The local fixture serves trusted static HTML only. Page scripts, redirects,
  active cross-origin fetches, DNS rebinding, and real-site policies have not
  been hardened or tested. **Do not expose arbitrary untrusted URLs.**
- The agent bridge receipts are owner-private and integrity-checked with an
  unkeyed SHA-256, not signed or tamper-proof against a local same-UID actor.
  URLs, selectors and hashes can still reveal metadata.
- The sidecar remembers the source receipt only in memory. A restart loses
  the replay pointer. Crashes between scheduler, browser side effects and
  receipts are **not atomic**; in-flight work must not be automatically
  retried without operator reconciliation.
- No external hosted CI, production deployment, multi-tenant admission,
  browser-profile security, accessibility conformance or arbitrary interactive
  browser agent capability is claimed.

## Follow-on engineering work

1. Versioned typed browser results and native job-to-receipt correlation,
   without extending raw CDP access to callers.
2. Identity-bound admission, origin/destination policy and per-agent browser
   context leases with explicit operator approvals.
3. Isolated subprocess/container execution with strict network policy,
   crash recovery, bounded resource usage and independent receipt validation.
4. Expand record/replay beyond the local fixed workflow only after the above
   policies and tests are present.

## Interaction with Tronix supervised workers

The live browser workflow above uses the default **in-process** Celix
operation bundle. The separate, opt-in `--worker` mode in current Tronix
intentionally passes a minimal environment to each child and does **not**
forward `TRONIX_LIGHTPANDA_WORKFLOW_SOCKET`. Its policy inspection therefore
cannot start this adapter in supervised mode. This is a deliberate, documented
integration gap, not permission to relax global worker environment filtering.
A future implementation should convey an explicitly approved socket capability
through the supervisor, validate it in the child, and keep its isolation and
credential-stripping tests intact. The Python/Lightpanda browser subprocess is
bounded and separately killable, but the current native Celix adapter itself
is not process-isolated by this feature.

# Tronix as an execution boundary for agent frameworks

**Status (2026-10-20):** experimental adapter and synthetic native demo, **not**
an OpenClaw plugin, production security sandbox, or deployed service.

## The architectural distinction

OpenClaw provides model-driven planning, tool discovery, approval policies,
and configurable sandbox backends. Tronix is a smaller, model-independent
Celix runtime whose trusted adapters expose a finite set of operations with
budgeted scheduling and lifecycle receipts. OpenClaw already has meaningful
sandboxing and approval mechanisms; Tronix must demonstrate incremental
value rather than claim they do not exist.

The experimental bridge in `src/agent-tool-boundary.mjs` accepts precisely
`{ "tool": "tronix_demo_constant" }` and nothing else. It constructs its own
random run ID, fixed Unix-local agent label, `demo.constant` operation, and
2-second queue deadline. It cannot forward a caller-provided shell command,
URL, operation name, identity, filesystem path, or budget.

This is a **boundary protocol experiment**, not a defense against a
compromised host or arbitrary untrusted Celix adapter code. The modeled caller
does not have access to the underlying runtime socket or other execution tools.

## Testable assertions

| Threat / assumption | Test / observation | Current evidence |
| --- | --- | --- |
| Tool caller supplies raw shell or URL | Strict single-property guard rejects before IPC | Unit negative tests |
| Caller tries another operation or budget | Rejected; adapter supplies fixed values | Unit negative tests |
| Caller supplies forged agent identity | Rejected before IPC | Unit negative tests |
| Caller requests unknown native operation | Celix returns DENIED | Native integration fixture |
| Trusted fixed operation requested | Returns synthetic 42 | Native integration fixture |
| Successful run is acknowledged | Receipt remains queryable (not an attestation) | Native integration fixture |
| Same-UID malicious worker accesses host | Not prevented by the adapter | **Not solved** |
| Agent can separately use generic host exec | Adapter cannot constrain that route | **Configuration prerequisite** |
| Worker can reach arbitrary UDP / Unix sockets | Not isolated by this adapter | **Not solved** |
| Stop/restart yields exactly-once semantics | Not guaranteed by receipts | **Not solved** |

Run after building existing demo targets:

```sh
npm run test:tool-boundary
```

No OpenClaw SDK package or production integration is installed. This proof
tests a framework-neutral function an authorized OpenClaw *plugin could call*.
A real plugin requires separate version-pinned SDK integration, manifest and
permission-surface review, safe configuration, and end-to-end tests. Current
OpenClaw documentation uses `openclaw/plugin-sdk/tool-plugin` and Node 24.16+
or 26.1+; Butcher's current Node 22 environment is insufficient to claim
the supported plugin has been compiled or loaded.

## Layered policy requirements

1. The operator grants the agent **only** the intended fixed Tronix tool;
   other OpenClaw tools including generic `exec`, elevated execution,
   browser shell surfaces and plugin loaders must be restricted separately.
2. Do not give the OpenClaw model raw `tronixd` socket access, an arbitrary
   local process launcher, or a credential that confers wider authority.
3. Continue to require reviewed Celix adapters, immutable policy snapshots,
   kernel peer credential admission, and separate, verified OS confinement.
4. Treat `queued`/`running`/`interrupted` as unknown side-effect states;
   do not infer cancellation, rollback, or automatically replay.
5. Move from harmless synthetic fixtures to approved OSS security test
   campaigns only after separate-UID operation, resource bounding,
   filesystem/network confinement, evidence preservation, and independent
   testing all pass.

## Alpha-Omega value proposition and requested outcomes

Alpha-Omega's stated mission is measurable security improvement in critical
open-source projects. Tronix is currently an early-stage tool, not itself
established critical infrastructure. The credible funding hypothesis is that
reusable execution restrictions could help **existing OSS maintainers** deploy
advanced security tools with less host risk and operational burden.

Proposed measurable milestones (not commitments or completed results):

- **M1 – threat model and admission contract:** maintain a public adversarial
  test catalog; prove zero prohibited operations dispatched across a
  documented test corpus. Publish reproducible baseline tests.
- **M2 – separately confined worker:** use unprivileged identities, filesystem
  and network controls, cgroups and verifiable resource restrictions; publish
  denial evidence for filesystem read/write, network access, process
  interference, identity spoofing, hung work, and bad artifact digests.
- **M3 – authorized OSS security workflow:** support one bounded synthetic
  fuzzing campaign through the existing IHB preflight path with an independent
  execution boundary and separate execution-vs-finding outcomes.
- **M4 – independent evaluation and adoption:** obtain outside security review,
  fix observed defects, and seek at least one maintainer pilot before claiming
  demonstrated ecosystem impact.

**Not claimed:** hard-real-time performance, a complete container sandbox,
multi-tenant security, general-agent equivalence, independent attestations,
upstream maintainer commitments, or Alpha-Omega sponsorship.

## External references

- OpenClaw tool and permission security:
  https://docs.openclaw.ai/gateway/security/tool-permissions
- OpenClaw tool plugin SDK:
  https://docs.openclaw.ai/plugins/tool-plugins
- Alpha-Omega eligibility and co-design:
  https://alpha-omega.dev/grants/how-to-apply/
- Tronix Landlock research:
  https://github.com/mcc0nnell/tronix/pull/9
- Tronix IHB preflight:
  https://github.com/mcc0nnell/tronix/pull/10

No grant form, email, or external outreach is authorized by this document.

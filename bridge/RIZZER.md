# Rizzer — IHB's bounded campaign executor

**Name:** Rizzer
**Purpose:** Execute a *pre-approved* IHB native-fuzzer campaign under an
independent, resource-bounded execution authority.
**Status:** Contract and naming specification. The read-only Tronix
`fuzz.preflight.*` adapter in this branch does **not** run campaigns.
Do not confuse successful preflight with successful execution.

## Responsibilities

**Tronix** selects a named operation from a reviewed static catalog, enforces
fairness and records scheduling/lifecycle receipts.

**Rizzer** is the external campaign execution service. Only approved synthetic
profiles such as `fuzz.campaign.safe` and `fuzz.campaign.buggy` are admissible
in the initial lab. It validates immutable source, seed, toolchain/image and
profile identities; launches a disposable restricted container; bounds CPU,
memory, process count and time; reconciles interrupted work; and stores an
immutable per-run receipt and the preserved IHB evidence manifest.

**IHB native_fuzz** is the mutation/coverage engine, not a service managed by
Rizzer internally. It owns corpus selection, ASan/UBSan results, crash replay
and candidate artifact generation.

**Blade** decides admission, and **WindAnvil** may independently verify
provenance. Neither a successful Rizzer campaign nor a Tronix receipt
automatically certifies a security issue.

## Interface contract

- Public-name proposal: `Rizzer`. Internal service command/name can be
  `rizzer`, but do **not** break the existing experimental operation names.
- Preflight: `fuzz.preflight.safe`, `fuzz.preflight.buggy` — file/profile
  identity only, not a campaign.
- Campaign proposal: `fuzz.campaign.safe`, `fuzz.campaign.buggy` — fixed
  synthetic fixtures, never arbitrary binary paths or shell commands.
- Results must distinguish **execution state** (`completed`, `failed`,
  `timeout`, `interrupted_unknown`, `cgroup_oom_kill`) from **fuzz result**
  (`no_failure_observed`, `asan_failure`, `ubsan_failure`,
  `input_timeout`, `unknown`). A synthetic ASan discovery can be a completed
  campaign and is not independently validated exploitability.
- Retain exact original IHB evidence, error logs and input digests; write only
  to fresh non-overwriting per-run output directories. Keep cancellation and
  evidence reconciliation explicit; never silently restart uncertain work.

## Isolation and authority

No Docker socket, host privileges or raw target-execution authority in
`tronixd` or its Celix adapter. Use distinct non-Docker service identity
for the scheduler and independently controlled container execution identity.
An approved broker verifies Linux peer identity and operation allowlists,
but same-UID peer checks alone provide no role separation.

Treat host-level Docker control as privileged. Test denied network access,
absence of credential mounts, cgroup OOM classification, process-tree cleanup,
timeouts, source/catalog drift and restart reconciliation before wider use.
Image digest and SHA-256 evidence hashes are identity evidence, **not**
security attestations. Only owned, authorized synthetic targets are in scope
for initial testing.

## First acceptance gate

1. Existing 9 preflight unit checks and 13 Tronix integration checks pass.
2. A bounded safe fixture returns `no_failure_observed`.
3. A deliberately buggy fixture returns reproducible `asan_failure`, and
   its corrected control is safe under exact replay bytes.
4. A bounded memory-exhaustion fixture returns `cgroup_oom_kill` only with
   observed local cgroup v2 OOM-kill counters.
5. A broker crash or timeout leaves a recoverable `interrupted_unknown`
   record, not a blind retry, and cannot orphan an unbounded container.
6. Permission checks prove the scheduler identity cannot open the Docker
   socket or write campaign evidence.

These are acceptance requirements, not claims that this branch has completed
them. The existing IHB local Docker acceptance and native tests are separate
from a live Tronix→Rizzer end-to-end campaign.

Related work:
- [Tronix architecture](../docs-ihb-fuzzing.md)
- [IHB native-fuzzer MR !203](https://gitlab.com/ihatebanks-group/IHATEBANKS-project/-/merge_requests/203)

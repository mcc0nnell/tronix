# Tronix + IHB native fuzzing — proposed integration

**Status (2026-10-20): read-only synthetic preflight implemented; campaign execution still proposed.**
Tronix and IHB remain independent projects. An opt-in Celix adapter now
advertises only `fuzz.preflight.safe` and `fuzz.preflight.buggy`; these verify
fixed reviewed IHB file digests via a private Unix socket and return Tronix
receipts. The adapter does **not** submit or execute a fuzz campaign, cancel
work or import finding evidence. See [bridge/README.md](bridge/README.md).
Do not interpret this preflight as a sandbox or an operational execution boundary.

- Tronix: admission, fair scheduling, bounded execution orchestration, lifecycle
  receipts and visibility into interrupted work.
- **Rizzer**: proposed external, independently restricted executor for
  pre-approved IHB campaigns. See [Rizzer contract](bridge/RIZZER.md).
- IHB native fuzzer: mutations, corpus/coverage selection, instrumented target
  execution, sanitizer reports, replay and candidate finding artifacts.
- WindAnvil: optional downstream provenance verification and independent
  assurance. Neither Tronix receipts nor IHB hashes constitute attestation.
- Blade: separately reviews candidate evidence and decides whether a capability
  or finding is admitted. No automatic admission from a crash.

## Architecture

```text
Authorized operator / approved campaign catalog (proposed)
        |
        v
Tronix native scheduler + ledger (existing)
        |  only reviewed, named campaign profiles
        v
Proposed Celix fuzz adapter / restricted job broker (NOT implemented)
        |  immutable manifest ID, not arbitrary shell / URL / path
        v
Disposable restricted execution runtime (independent admission needed)
        |  pinned image, read-only inputs, cgroup memory+pids+CPU,
        |  network-off, least-privilege filesystem + user separation
        v
IHB native_fuzz/run.py -> engine.cc -> ASan/UBSan instrumented harness
        |
        v
Immutable evidence bundle + optional bounded minimization (IHB)
        |
        v
Independent Blade review -> optional WindAnvil assurance
```

The scheduler schedules **campaigns**, not individual mutated inputs. IHB owns
per-input fork/replay, the corpus and the stop-on-failure rule. The orchestration
plane must never silently alter seeds, change compiler flags or suppress failures.

## Current capabilities and interface constraints

Tronix v1 already has a private MessagePack socket, `submit`, `result`,
`receipt`, `events`, `ack`, fixed capacity, trusted adapter execution budgets
and opt-in one-shot Celix workers. Its `submit` message takes a known operation
name; it has **no typed campaign arguments**. It cannot currently start an
operator-supplied campaign manifest, cancel active work, attach a bundle or
return an evidence manifest through a new API. Only one operation executes at
a time; lengthy campaigns block unrelated work.

The opt-in `tronix_worker` currently sets `RLIMIT_AS=1 GiB`, CPU time to five
seconds and a finite operation budget with a 200 ms startup allowance.
AddressSanitizer reserves large virtual address ranges; an address-space limit
of this kind may prevent sanitizer harnesses from starting. **Do not route an
ASan campaign through the existing child worker unchanged.** Running a Docker
CLI inside an otherwise privileged adapter is also not a sandbox: Docker socket
access is effectively host-level authority. Do not grant the daemon a Docker
socket or widen its capabilities to make the prototype run.

The IHB draft MR currently contains `native_fuzz/run.py`, `engine.cc`, a
Docker-based **opt-in** `isolated.py` helper and `minimize.py`. The Docker
helper specifies a container cgroup memory/swap budget, no network and restricted
mounts; its image must be separately approved and pinned. It has not been
integrated with Tronix. IHB's `minimize.py` explicitly expects trusted campaign
artifacts and currently executes the saved binary directly, outside this
proposed restricted orchestration boundary.

## Proposed first increment: fixed, reviewed profiles

Keep MessagePack protocol v1 unchanged for a proof of concept. A reviewed
Celix adapter may advertise a finite set of static operation names, for example
`fuzz.fixture-small`, each mapped server-side to an **immutable, allowlisted**
campaign manifest. This example name is a proposal and is not currently
supported. The requester may choose only a name: never command strings,
arbitrary URLs, raw paths, binary names, build flags, runtime images or extra
mounts. Deny unknown profiles and requests with extra fields.

A trusted broker would pass the manifest ID to a separately confined,
unprivileged executor. The executor would run a pinned, digest-verified image,
copy in fixed verified seed/harness artifacts read-only, and write output to a
new private directory. Keep the execution backend's credentials, container
socket and target artifacts outside the main Tronix daemon. Initially use
short, operator-triggered fixture campaigns rather than automatic recurring
searches or arbitrary public targets.

### Proposed typed campaign record (design only)

```json
{
  "schema": "ihb.fuzz-campaign.v1-proposed",
  "campaign_id": "operator-issued-unique-id",
  "profile_id": "fixture-small",
  "manifest_sha256": "<64 lowercase hex digits>",
  "target_source_sha256": "<64 lowercase hex digits>",
  "image_digest": "sha256:<64 lowercase hex digits>",
  "seed": 17,
  "runs": 96,
  "wall_seconds": 30,
  "per_input_timeout_ms": 1000,
  "memory_mib": 1024,
  "network": "none",
  "admission": "not_performed"
}
```

This is a *planned* manifest, **not** an accepted Tronix wire request.
Validate ranges, immutable digests and profile ownership before dispatch.
Version every schema; never interpolate any manifest field into a shell.
Add parameters to Tronix only through a reviewed typed admission extension,
not by weakening strict v1 parsing.

### Lifecycle mapping (proposed; separate from fuzz findings)

| Tronix state | Campaign interpretation |
| --- | --- |
| `queued` | Admitted profile waiting for the bounded executor |
| `running` | Executor started; outcome still unknown |
| `completed` | Runner terminated normally and evidence was checked, **not** "no vulnerability" |
| `failed` | Runner failed or produced an unaccepted result; preserve evidence if any |
| `expired` | Queued job did not start before its deadline |
| `overrun` | Execution exceeded its trusted budget; kill/reap and preserve partial evidence |
| `interrupted` | Restart or crash left the outcome unknown; inspect external executor before retry |

A reproducible ASan/UBSan crash may be a **successful campaign execution**
with a candidate IHB finding. Conversely a container OOM, sanitizer startup
failure, compiler error, harness nonzero return or timeout is not automatically
a memory-safety finding. Record campaign execution state and candidate finding
classification in **different fields**. A `completed` state does not mean the
fuzzer found nothing. Ensure the adapter treats `run.py`'s nonzero discovery
exit code separately from infrastructure failure.

Tronix v1 receipts are local, best-effort execution evidence, not transactional
proof. The scheduler and journal do not commit atomically. The proposed broker
must use unique job IDs and never auto-retry an `interrupted` or `overrun`
campaign. The external executor may have completed, or have written artifacts,
when the main process died. Reconcile by campaign ID and manifest digest first;
manual operator approval is needed for any retry.

## Evidence interchange (proposed)

Retain original IHB files without rewriting them: `evidence.json`,
`summary.json`, `journal.jsonl`, seed/harness/compiler identities, retained
corpus, failure input and diagnostic logs. The proposed integration envelope
would add an externally unique campaign ID, Tronix receipt/event sequence,
approved profile and manifest digest, executor image digest, resource limits,
observed termination (including timeout vs cgroup OOM), and the SHA-256 of
the preserved original IHB evidence manifest. Link documents by digest; never
forge or overwrite IHB logs or present hashes as cryptographic attestations.
Redact credentials and sensitive target data before publishing.

Minimization is optional and a separate, explicitly approved stage; it must not
mutate the original crash or contaminate the original coverage/corpus. Crash
grouping is conservative, scoped to a particular binary/build and reproducible
input; identical sanitizer classes do not prove identical root causes.

## Security admission requirements (not yet met)

1. **Threat model and authority:** approved local operator, authenticated
   campaign identity, static allowlisted target profiles, denied arbitrary
   execution, mount/UID/Docker-socket separation from `tronixd`.
2. **Containment:** disposable sandbox with no default network, no host secrets,
   read-only pinned inputs, private output, restricted privileges and syscalls,
   memory/pids/CPU/wall budgets, process-tree cleanup and enforced disk quota.
   Validate containment empirically; Docker flags alone do not certify it.
3. **Sanitizers:** run ASan/UBSan with adequate virtual address space while
   bounding real memory with cgroups. Distinguish OOM, SIGKILL, sanitizer fault,
   harness failure, deadline, compiler error and executor failure.
4. **Integrity:** pin image/toolchain and source/seed digests. Create unique
   non-overwriting outputs. Preserve failing bytes and all raw evidence.
5. **Reliability:** resolve executor state independently after daemon crashes;
   no blind retries, correct cancellation/kill semantics, no leaked processes,
   bounded backpressure and no cross-agent evidence disclosure.
6. **Authorization:** do not feed a real or third-party target into the pipeline
   without reviewing target ownership, permitted scope and research rules.
   Synthetic fixtures are the initial admissible test set.

## Acceptance plan

- Run existing Tronix smoke/hardening/ledger/isolation/memory tests and the
  existing IHB native/reduction tests independently before integration.
- Demonstrate a safe synthetic campaign and a deliberate ASan heap-overflow
  fixture inside the admitted bounded executor. Replay the exact failure and
  run the corrected control with a fresh output directory.
- Demonstrate OOM, hung input, malformed manifest, unknown profile, denied
  network, inaccessible host secrets, and mismatched artifact/image digest.
  Check the host and worker return to idle with no orphan processes.
- Kill and restart `tronixd` during execution, then reconcile receipts and
  executor evidence without auto-replaying side-effecting work.
- Confirm that `result`/`receipt` remain accurate while an operation is
  running, and that a candidate finding never causes automatic Blade admission.
- Measure single-worker throughput and queue fairness. Consider a dedicated
  external campaign queue **after** the evidence/sandbox contract passes;
  do not silently transform the one-worker runtime into a distributed service.

### Work breakdown

1. Document and review the campaign manifest/profile schema and static
   allowlist. Define independent execution vs finding statuses.
2. Admit an unprivileged cgroup-backed executor on Butcher using reviewed,
   digest-pinned sanitizer images, with isolation/negative tests.
3. Implement a small opt-in broker/adapter for one synthetic profile, with
   immutable evidence linking and explicit operator dispatch.
4. Add reconciliation, cancellation semantics, timeout/OOM classification and
   guarded crash minimization before wider scheduling.
5. Only then consider multiple workers, structure-aware mutation, more targets,
   Blade admission and WindAnvil provenance verification.

**Non-goals for this document:** implementing the adapter, changing v1 wire
rules, granting Docker daemon privileges, running unreviewed targets, claiming
production-grade sandboxing or equating synthetic sanitizer failures with
eligible real-world vulnerabilities.

**Related repositories:** [Tronix](https://github.com/mcc0nnell/tronix) and
[IHB draft native-fuzzer MR !203](https://gitlab.com/ihatebanks-group/IHATEBANKS-project/-/merge_requests/203).

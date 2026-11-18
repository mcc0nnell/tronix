# TRONIX: pinned cartridge catalog and signed Rizzer evidence digests

## Scope and prerequisites

This change **stacks on** the bounded single-operation Rizzer orchestration
adapter in TRONIX PR #23 and the Rizzer harness factory/native-parity MRs
!78 and !83.

This is *startup-time data-only discovery*: the operator supplies a fixed,
explicit, SHA-256-pinned local JSON catalog of offline-reference casebooks.
The Celix adapter advertises up to eight operation names from the catalog,
and TRONIX's existing `tronix.grants.v1` policy selects which are exposed
and schedulable. A cartridge name alone grants **no new execution privilege**.
Native evaluator families are still limited to the four reviewed Rizzer
offline-reference oracles.

This increment also carries the **complete SHA-256 digest of each successful
Rizzer `native-parity.json` artifact** through a versioned optional native
Celix service and the supervised worker, finally incorporating that digest
in a **HMAC-protected TRONIX v2 terminal evidence record**.

Existing `tronix_operation_service_t` and `tronix.grants.v1` interfaces
remain unchanged. Existing non-Rizzer operations continue emitting v1
evidence records and need no artifact-digest service.

## Data and trust flow

```text
Operator: casebooks with pinned IR, exact SHA-256 catalog and grants
   │
   ├── /tmp/tronix-rizzer-harness-UID/catalog.json (private mode 0600)
   ├── /tmp/tronix-rizzer-harness-UID/catalog.sha256 (private mode 0600)
   └── /tmp/tronix-rizzer-harness-UID/bridge.token (private mode 0600)
   │
   ▼
TRONIX Celix adapter: bounded static startup catalog parser
  └── operations() returns only enumerated catalog names
   │
TRONIX daemon: immutable startup policy ∩ exact operator grants
   │
TRONIX scheduler -> one-shot worker (existing stripped environment)
   │
Celix adapter -> owner-private socket -> tokened Node sidecar
   │
Sidecar: exact selected casebook IR hash + pinned native host/bundle
   │
Rizzer native offline test + Python reference parity
   │
Full report SHA-256, count (3..32), preserved local receipt
   │
Sidecar MsgPack: {ok, value, digest} (digest exactly 64 lowercase hex)
   │
Celix optional org.tronix.artifact.digest/1.0.0
   │
TRONIX supervised worker: fixed v2 result with digest, fail-closed if absent
   │
TRONIX daemon: binds digest to job ID, operation and dispatch
   │
GrantEvidence: HMAC-chained v2 terminal record includes full digest
   │
Independent verify_grant_evidence.py --artifact-dir:
  signed digest == actual report bytes
```

## Catalog

The operator owns the catalog contents, which are **not** selected by TRONIX
clients. The stable schema is `tronix.rizzer.catalog.v1`:

```json
{
  "schema": "tronix.rizzer.catalog.v1",
  "entries": [
    {
      "operation": "rizzer.harness.spinel",
      "family": "spinel-packed-uint",
      "casebook": "/absolute/private/spinel-bundle",
      "ir_sha256": "64_lowercase_hex_sha256_of_exact_ir_json_file"
    },
    {
      "operation": "rizzer.harness.fullmath",
      "family": "evm-fullmath",
      "casebook": "/absolute/private/fullmath-bundle",
      "ir_sha256": "64_lowercase_hex_sha256_of_exact_ir_json_file"
    }
  ]
}
```

The `ir_sha256` is the SHA-256 of the **actual `ir.json` file bytes,
including its formatting newline**, not the factory's canonical-JSON IR
digest in `build-receipt.json`. The latter is still independently checked
by the reviewed Rizzer bridge. All paths must be absolute, regular,
owner-private where required, and selected by the operator.

Allowed families: `mcp-authorization`, `sixlowpan-fragments`,
`spinel-packed-uint`, `evm-fullmath`. Up to eight unique names of the form
`rizzer.harness.<lowercase-identifier>`. Each is a fixed lane-0 operation
with a 12-second supervised budget. No hot reload, arbitrary commands,
target URLs, device access, model calls or arbitrary Python plugin loading.

Prepare the fixed existing UID-private root and token exactly as documented
in `docs/rizzer-harness-orchestration.md`, then add the catalog and its
byte-exact SHA-256 pin. Example:

```bash
umask 077
ROOT="/tmp/tronix-rizzer-harness-$(id -u)"
# ROOT must already exist, be owned by this UID and have 0700 mode.
cp /absolute/operator-selected/catalog.json "$ROOT/catalog.json"
# Store no newline in the expected hash file:
python3 -c 'import hashlib,pathlib,sys; p=pathlib.Path(sys.argv[1]); pathlib.Path(sys.argv[2]).write_text(hashlib.sha256(p.read_bytes()).hexdigest())' \
  "$ROOT/catalog.json" "$ROOT/catalog.sha256"
chmod 600 "$ROOT/catalog.json" "$ROOT/catalog.sha256"
```

Pass `--catalog "$ROOT/catalog.json"` and
`--catalog-sha256 "$(cat "$ROOT/catalog.sha256")"` to the same reviewed
`scripts/rizzer_harness_sidecar.mjs` invocation used by PR #23,
**replacing** the old `--casebook` argument. Existing arguments for the
operator-selected Rizzer CLI, native host, compiled native Celix bundle,
token, socket and private evidence root remain mandatory.

The `tronix.grants.v1` file names the exact `org.tronix.adapter.rizzer_harness`
archive SHA-256 and must contain an explicit `allow <operation>` line
for each desired cartridge, for example:

```text
tronix.grants.v1
bundle org.tronix.adapter.rizzer_harness
version 0.1.0
artifact <lowercase actual SHA-256 of tronix_rizzer_harness.zip>
allow rizzer.harness.spinel
allow rizzer.harness.fullmath
```

Unknown, unlisted, duplicate, or ungranted operation names fail closed.
The parent TRONIX worker's environment-stripping policy has not changed.

## Signed evidence record, v2

Existing v1 terminal evidence is preserved unchanged:

```text
v1 sequence id_hex agent_hex op_hex state dispatch value
   adapter_sha256 grant_sha256 previous_mac hmac
```

For a **successful offline harness** operation, v2 adds the complete
SHA-256 of the Rizzer native parity **report bytes** after the result:

```text
v2 sequence id_hex agent_hex op_hex completed dispatch value
   rizzer_report_sha256 adapter_sha256 grant_sha256 previous_mac hmac
```

The line above is whitespace-wrapped for illustration only; actual records
are single-line, space-separated, with exactly 13 fields including the MAC.
The HMAC includes the new `rizzer_report_sha256` field, so changing it
invalidates the MAC and the subsequent chain. The digest is tied to the
TRONIX job ID, operation, dispatch sequence and exact granted adapter.
Failures do not manufacture a digest or claim a successful artifact;
legacy v1 receipts continue to represent unsuccessful terminal outcomes.

The optional `org.tronix.artifact.digest/1.0.0` Celix service exposes
a one-shot, 65-byte output buffer for a lowercase SHA-256 digest. It is
separate from the frozen TRONIX v2 operation ABI. Worker and supervisor
refuse to report successful `rizzer.harness.*` execution without the
full valid digest. The daemon signs the digest in `GrantEvidence` only
for completed jobs.

Independent verification:

```bash
python3 scripts/verify_grant_evidence.py \
  --key /absolute/private/hmac.key \
  --grants /absolute/private/grants.conf \
  --adapter /absolute/build/tronix_rizzer_harness.zip \
  --evidence /absolute/private/tronix-grant-evidence.txt \
  --artifact-dir /absolute/private/rizzer-sidecar-evidence
```

This verifies the HMAC chain and the exact bytes of each
`run-*/native-parity.json` against the signed digests.
Without `--artifact-dir`, the independent verifier confirms the signed
digest values but does not check actual report files. Existing operator
checkpoints can still anchor v1/v2 HMAC chain prefixes.

## Actual tests — 2026-11-18

- Native Celix adapter, original scheduler, supervised worker, daemon and
  versioned digest service compiled on Butcher.
- One unchanged daemon discovered **four** operator-registered cartridges
  in a single startup catalog and ran all four using explicit grants:
  MCP (4 cases), 6LoWPAN (6), Spinel (6), FullMath (5).
- **21/21** offline reference cases completed with Python/native parity.
- The independent verifier checked **four v2 HMAC records** against the
  four report files. The 24-assertion multi-cartridge integration suite
  passed, including tampered report bytes and a forged digest in the HMAC
  ledger, both correctly rejected.
- Wrong-token and unregistered-operation direct socket requests are denied
  before execution. Catalog file and catalog pin are verified at startup.
- Existing TRONIX v1 grants and legacy evidence verification remain
  supported; that compatibility is exercised by the existing
  `tests/grants-evidence.mjs` regression suite.
- Runner-independent `tests/signed-artifact-v2.mjs` passed 8/8 checks:
  mixed v1/v2 evidence, full artifact-byte digest binding, tampered reports,
  altered HMAC fields, and forged-but-valid daemon-signed digest values.
- The Celix adapter uses fixed-capacity POD bundle state because pinned
  `CELIX_GEN_BUNDLE_ACTIVATOR` allocates that state with `calloc`/`free`.
  This avoids undefined behavior from unconstructed C++ STL members.
- The complete existing `npm test` suite and `npm run license:check`
  passed. The existing queue-overload smoke assertion was timing-sensitive
  under heavy concurrent CPU load and failed twice before passing on retry;
  no scheduler or queue limit was changed.

No production target execution, external network target activity, bounty
finding, third-party code execution, hard OS sandbox, signed supply-chain
attestation, or cross-UID separation is claimed.

## Review gates

These are SHA-256 content identities and a daemon-held HMAC key,
**not** immutable signatures independent of the daemon. A compromised
same-UID process may read the bridge token and could access private files;
for true multi-tenant isolation, use separately sandboxed UIDs and a
separate trusted evidence writer. Existing independent checkpoint signing
remains the mitigation for anchored-chain rollback.

Startup discovery is bounded by the reviewed catalog and fixed native
families. Registering a new harness *implementation* still requires
a separately reviewed native/Python oracle and capability policy,
not merely a catalog entry.

CI workflows may fail before acquiring a GitHub-hosted runner; only
successful executed jobs count as upstream verification. Merge this
stacked change after its parent PR and independent review.

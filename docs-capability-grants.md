# TRONIX — operator grants and verifiable terminal receipts (experimental)

The optional grants mode uses the existing **real Apache Celix** process-isolated
operation worker. It constrains the worker's dynamically discovered operations
with an **administrator-authored, private allowlist** and binds that decision
to a SHA-256-pinned Celix ZIP plus the bundle metadata in the ZIP.

**This mode is opt-in.** Existing demo, legacy UID policy, scheduler,
MessagePack v1 and unpinned modes remain compatible. This is not a multi-tenant
security system or production sandbox.

## Boundary and exact format

Grants are ASCII, newline-terminated, 4096 bytes at most, regular
single-link files owned by the daemon UID with no group/other permissions.
Every directive is mandatory except that multiple unique `allow` directives
are supported, up to 16.

```text
tronix.grants.v1
bundle org.tronix.adapter.demo
version 0.1.0
artifact <64-lowercase-hex-SHA256-of-exact-ZIP>
allow demo.constant
```

Startup fails closed unless all of the following hold:

1. An absolute `--grants` file is private, owned and syntactically strict.
2. `--worker` and a matching `--adapter-sha256` pin are specified.
3. The pinned private adapter ZIP contains a bounded Celix
   `META-INF/MANIFEST.json` whose `CELIX_BUNDLE_SYMBOLIC_NAME` and
   `CELIX_BUNDLE_VERSION` match the exact grant.
4. Every operator-granted operation is actually in the worker's previously
   inspected, frozen finite Celix service policy.
5. Two absolute paths `--evidence` and `--evidence-key` are supplied;
   the HMAC key is an owner-private **raw 32-byte file**; the evidence file
   must **not** already exist.

The grant file's exact SHA-256 is the *operator-grant configuration digest*.
It is not a claim that an adapter's external configuration files are pinned.
Bundle service ranking, Celix service ID and install path are never accepted
as execution authority. At submission, the permitted operation is the
intersection of adapter policy, operator grants and any configured peer
UID policy. Existing trusted lane/budget values remain immutable for
the lifetime of a supervised daemon.

## Reproducible local fixture

```sh
mkdir -p private && chmod 700 private
ADAPTER="$(pwd)/build/tronix_demo.zip"
PIN="$(sha256sum "$ADAPTER" | cut -d' ' -f1)"
GRANTS="$(pwd)/private/capabilities.grants"
cat > "$GRANTS" <<EOF
tronix.grants.v1
bundle org.tronix.adapter.demo
version 0.1.0
artifact $PIN
allow demo.constant
EOF
chmod 600 "$GRANTS"
openssl rand 32 > private/receipt.key
chmod 600 private/receipt.key

./build/tronixd \
  --socket "$(pwd)/private/tronix.sock" \
  --scheduler "$(pwd)/build/tronix_scheduler.zip" \
  --adapter "$ADAPTER" \
  --worker "$(pwd)/build/tronix_worker" \
  --adapter-sha256 "$PIN" \
  --grants "$GRANTS" \
  --evidence "$(pwd)/private/receipts-001.txt" \
  --evidence-key "$(pwd)/private/receipt.key"
```

After one or more completed/failed supervised operations, verify from a
**separate Python program**:

```sh
python3 scripts/verify_grant_evidence.py \
  --key private/receipt.key \
  --grants private/capabilities.grants \
  --adapter build/tronix_demo.zip \
  --evidence private/receipts-001.txt
```

Evidence is strictly canonical space-delimited ASCII:

```text
v1 sequence id-hex agent-hex operation-hex terminal-state dispatch-sequence
   value-or-dash archive-sha256 grant-sha256 previous-HMAC current-HMAC
```

Each row signs all preceding fields with HMAC-SHA256 using the 32-byte
operator key. The first previous HMAC is 64 zeros. After successful
`fdatasync`, the next row uses the preceding row's HMAC. The independent
verifier recomputes archive identity, operator grant identity, Celix manifest
name/version, operation eligibility, every row's MAC, contiguous sequence,
and links. It rejects altered or missing earlier records, invalid field
structures, other signing keys and mismatched supplied artifacts/grants.

**Critical limitations:** The HMAC proves knowledge of the key and internal
chain integrity for the **presented rows**, not completeness of all executions.
Truncating final complete rows or replacing a whole valid log with an earlier
valid prefix is **not detected** without an externally retained terminal
sequence number and chain-head checkpoint. There is no such external
checkpoint yet. Host/root compromise, same-UID access to the daemon/key,
altered Celix dependencies, or malicious adapters are outside this
protection. The receipt is emitted after a terminal operation and is not an
authorization token or a proof that external side effects were safe,
accurate, delivered, or rolled back. A failed write rejects new submissions,
but previously queued operations are not guaranteed rollback.

On restart use a **new evidence filename**: deliberately no silent reuse,
reopening or unverified chain continuation. Secure key storage, rotation,
external checkpoints, durable admission-vs-terminal reconciliation and
stronger sandboxing are separate release gates.

## Validation

`npm test` includes `tests/grants-evidence.mjs`, which starts real native
Celix workers and tests metadata/artifact mismatch refusal, invalid grants,
read permissions, symlink refusal, operator filtering, successful execution,
a two-receipt chain, standalone verification and negative tamper/key tests.

No real radio dispatch, browser scripting, arbitrary shell execution, or
IHB/Docker authority is enabled by grants mode.

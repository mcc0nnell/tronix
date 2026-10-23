# TRONIX independently retained evidence checkpoints

The operator can now create and validate a small, HMAC-signed checkpoint for an
existing **verified** terminal receipt chain. This is an opt-in verification
feature; the daemon, scheduler, MessagePack protocol and receipt format are
unchanged. It follows [operator grants](docs-capability-grants.md).

## Security boundary

The supervised daemon already signs each terminal receipt using its receipt
key. Since the daemon knows that key, the original HMAC chain alone cannot
detect restoration of a *previously valid complete prefix* or substitution
of a newly forged chain by a process that has the same key.

A checkpoint binds four values: the receipt **sequence number**, the
**chain-head MAC at exactly that sequence**, the **approved adapter ZIP
SHA-256** and the **grants-file SHA-256**. It is signed with an **independent
operator-only 32-byte HMAC-SHA256 key**. The verifier refuses using the
same key for both signing domains.

**The checkpoint and the separate signing key must be retained outside the
daemon's write-access trust boundary.** Use a separate verification host and
account, or immutable/off-host operator-controlled storage with appropriate
access controls. The provided tool creates and verifies files; it **does
not send files to another machine, configure object locking, run automatically
or claim external persistence**. Creating a checkpoint on the same
host/UID as the daemon is a useful functional test, not durable off-host
anchoring.

## Create a checkpoint

Copy the evidence log, approved adapter ZIP, exact grant file and the
receipt-verification key into an independently controlled verification
environment. The receipt key is needed to verify the original receipts;
the operator checkpoint key must **not** be provided to the daemon.

Generate a fresh, distinct raw 32-byte checkpoint key on the operator's
verification host (once, under operator key-management controls):

```sh
umask 077
openssl rand 32 > operator-checkpoint.key
chmod 600 operator-checkpoint.key
```

Verify the complete existing chain and create a **new**, non-overwriting
checkpoint:

```sh
python3 scripts/verify_grant_evidence.py \
  --key private/receipt.key \
  --grants private/capabilities.grants \
  --adapter build/tronix_demo.zip \
  --evidence private/receipts-001.txt \
  --checkpoint-key operator-checkpoint.key \
  --write-checkpoint checkpoint-0001.json
```

The new checkpoint is a strictly scoped JSON record with
`schema = tronix.evidence-checkpoint.v1`, `sequence`, `chainHead`,
`grantSha256`, `artifactSha256` and the operator HMAC signature.
A pre-existing file, including a symbolic link, is never overwritten. The
writer flushes checkpoint bytes to the local file before reporting success.
**Separately export the checkpoint and protect it from replacement or
removal**; retaining only the daemon-accessible file is insufficient.

## Independently verify one or multiple retained checkpoints

Fetch the protected checkpoint(s) into the verification host. Independently
verify every original receipt and the anchored prefix:

```sh
python3 scripts/verify_grant_evidence.py \
  --key private/receipt.key \
  --grants private/capabilities.grants \
  --adapter build/tronix_demo.zip \
  --evidence private/receipts-001.txt \
  --checkpoint-key operator-checkpoint.key \
  --checkpoint checkpoint-0001.json \
  --checkpoint checkpoint-0002.json
```

The verifier checks every row's existing HMAC, then authenticates each
checkpoint with the independent operator key. It compares each checkpoint's
artifact, grant, sequence and **historical chain head at that sequence**.
This detects a log shorter than the latest retained checkpoint and a
divergent or recomputed prefix, including one signed using the compromised
daemon receipt key. A valid longer chain is accepted when its anchored
prefix has not changed. Multiple checkpoints (up to 64) may be checked in a
single invocation.

Exit code 0 means the presented receipt chain is valid and consistent with
all supplied checkpoints. Exit code 1 means failure. The JSON output
includes `checkpointMode`, `anchoredPrefix` and `checkpointCount`.

## Limitations and release gates

- **Rollback after the newest checkpoint is not detected.** For example,
  if the checkpoint covers sequence 10, then valid records 11–20 can be
  removed and the remaining 10 can still verify.
- External storage must not let the daemon rewrite, replace, hide or roll
  back the saved checkpoint, or obtain the operator-only key. Its integrity
  is not enough if an attacker can suppress or replace the anchor supplied
  to the verifier.
- A checkpoint proves integrity and continuity for a particular receipt
  chain, **not** that every operation was recorded, that the system was
  available, that wall-clock timestamps were accurate, or that any external
  effect was delivered safely.
- The operator decides when to checkpoint; there is no built-in recurring
  publication schedule, cloud integration, hardware-protected key custody,
  cross-process acknowledgment protocol or fail-closed admission on a
  missing future checkpoint.
- The key can be used to forge checkpoints if compromised. Treat it as
  independent operator authority, and do not store it in TRONIX worker or
  daemon environments.

The native-backed regression suite exercises valid-prefix rollback, forged
chains signed with the daemon key, multiple checkpoints, tampered checkpoints,
key separation, checkpoint file overwrite refusal, longer valid histories
and the uncheckpointed-suffix limitation using real Celix operations.
Run `npm run test:grants` or the full `npm test`.

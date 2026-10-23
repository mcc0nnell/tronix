# TRONIX Cloudflare checkpoint publication (experimental)

The separate [operator-signed checkpoint](docs-evidence-checkpoints.md) can now
be published to a **Cloudflare Worker backed by D1** and read back from an
independent verification host. No AWS services or tooling are involved.
This is intentionally a small **off-host publication integration**, not a
claim of WORM or immutable custody. Neither the Worker nor D1 is run by the
TRONIX daemon, and the server never accepts unsigned checkpoint records.

## Trust and custody

The checkpoint uses the operator HMAC key from PR #17. The Worker verifies
the same key from an **independently provisioned Cloudflare secret**,
`CHECKPOINT_HMAC_KEY_HEX`. It also requires two distinct, separately held
tokens: `PUBLISH_TOKEN` and `READ_TOKEN`, each at least 32 characters.
These are never sent to the daemon and must not be checked into Git.

**Do not deploy from or configure this Worker using the compromised daemon's
own security principal.** Use a separate Cloudflare account/role and an
operator-controlled verification host. This arrangement can detect ordinary
daemon receipt-chain rollback, but is **not** tamper-resistant against a
Cloudflare account administrator: an administrator can alter the Worker,
secrets, D1 records, triggers, or served historical checkpoints.
Cloudflare D1 cannot be represented as inherently immutable storage.

The D1 schema indexes independently by the exact approved adapter digest and
operator grant digest. Each stream has a strictly increasing sequence and
a single signed checkpoint per sequence. A single conditional SQLite INSERT
atomically refuses a sequence at or below the current maximum; SQLite triggers
reject UPDATE/DELETE and direct nonmonotonic INSERT at the SQL level. This is
application-level protection, not an account-level retention guarantee.
Publishing the same identical last checkpoint again is idempotent. Publishing
a divergent record at that sequence or a lower sequence returns HTTP 409.
Two separate HTTP tokens give independent publishing and reading privileges.

## Provision under independent operator control

On an authorized verification host with Wrangler installed:

1. Create a separate D1 database named `tronix-checkpoints`.
2. Copy `cloudflare/wrangler.example.jsonc` to
   `cloudflare/wrangler.jsonc` and replace the example database UUID.
   The real config is gitignored.
3. Run (from the project root):

```sh
npx wrangler d1 migrations apply tronix-checkpoints \
  --config cloudflare/wrangler.jsonc --remote
```

4. Provision Worker **secrets**, not plaintext `vars`: use
   `npx wrangler secret put CHECKPOINT_HMAC_KEY_HEX
   --config cloudflare/wrangler.jsonc` with the **hex encoding** of
   the raw 32-byte operator checkpoint key. Similarly set
   `PUBLISH_TOKEN` and `READ_TOKEN` to independent long, random
   token strings. Do not use the daemon's receipt HMAC key.
5. Deploy with
   `npx wrangler deploy --config cloudflare/wrangler.jsonc`.
   Restrict Cloudflare account roles and Worker/D1 administration separately
   from the daemon.

This repository includes no Cloudflare account, D1 ID, access tokens, secret
values or assumed production URL. A successful **deployment and real D1 smoke
test have not been asserted**. Cloudflare account setup and secret custody
must be performed by an authorized operator.

## Sign and publish from verification host

First use `scripts/verify_grant_evidence.py` to verify the native receipts
and **create** an operator-signed checkpoint with `--write-checkpoint`.
Then publish it from the verification host (not Butcher/daemon):

```sh
export TRONIX_CHECKPOINT_PUBLISH_TOKEN='your-independent-publish-token'
export TRONIX_CHECKPOINT_READ_TOKEN='your-separate-read-token'

python3 scripts/publish_cloudflare_checkpoint.py \
  --url https://YOUR-WORKER.YOUR-SUBDOMAIN.workers.dev \
  --checkpoint checkpoint-0001.json \
  --checkpoint-key operator-checkpoint.key
```

Use a secure secret-management mechanism in place of exporting secrets
through shell history. The CLI refuses non-HTTPS endpoints and redirects.
It authenticates the operator checkpoint **locally** before submission.
The Worker verifies the HMAC a second time and commits the monotonic insert.
The CLI then fetches the **exact checkpoint at its sequence**, requires
byte-for-byte equality with the local signed file, and verifies the latest
remote checkpoint signature and sequence. It exits nonzero if publication or
read-back diverges; do not treat the attempted upload as confirmed.

The API has two routes, both requiring bearer authorization:
`POST /v1/checkpoints` (canonical signed JSON with trailing newline)
and `GET /v1/checkpoints/<adapter-sha256>/<grant-sha256>`.
The GET route also accepts `?sequence=N` for exact historical read-back.
There is no unauthenticated public listing, arbitrary endpoint forwarding,
mutable checkpoint API or delete operation.

## Validation and missing security gates

Run `npm run test:checkpoint-cloudflare` for 26 Cloudflare Worker boundary
checks using a modeled D1 API, plus 9 checks of the **actual SQL** against
Python SQLite. `npm test` includes those checks alongside the native
Celix and checkpoint suites. These do **not** constitute a deployed
Cloudflare/Wrangler D1 end-to-end test.

Remaining for a high-assurance service: genuinely independent and
non-rewritable external custody (for example, another organizational
administrator or external transparency witness), production D1 end-to-end
smoke testing, key rotation, durable publication scheduling and alerting,
retention policies, server-side rate limiting, and disaster/replay handling.
An HMAC secret placed inside a Worker is accessible to sufficiently
privileged Cloudflare administrators. A future asymmetric signature where
the Worker holds only the public verification key would narrow that risk,
but would **not** solve the database administrator's ability to erase history.

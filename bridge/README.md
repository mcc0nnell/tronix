# IHB read-only synthetic preflight — experimental

**Implemented:** optional Celix bundle `tronix_ihb_preflight.zip`, a small
nonprivileged Python AF_UNIX preflight broker, a SHA-256-pinned fixture catalog,
9 negative/unit tests and a real Tronix/MessagePack integration fixture.

**Not implemented:** Docker campaign dispatch, a privileged campaign broker,
remote sandbox admission, arbitrary targets, IHB evidence retrieval, cancellation,
automatic retries, production authorizations or Blade/WindAnvil assurance.

## What the new integration actually does

A reviewer-selected catalog lists the exact current SHA-256 for `run.py`,
`engine.cc`, `isolated.py`, and IHB's intentionally safe/buggy C fixtures.
The operator passes the digest of that catalog when starting
`bridge/ihb_preflight.py`. The Python service rejects mutable image tags,
unapproved file names, changed or symlinked source files, non-private socket
directories, unauthorized peer UIDs and unknown operations.

The native Celix adapter exposes **only** `fuzz.preflight.safe` and
`fuzz.preflight.buggy`, with background priority and a 3-second budget.
The compiled adapter knows exactly one Unix-socket path and the expected broker
UID. A successful response returns integer 1 through an ordinary Tronix
`completed` receipt. **Completed means source preflight passed; it does NOT
mean a fuzz campaign was executed or a vulnerability was found.**

No requests permit image changes, URLs, shell commands, raw paths, Docker socket
mounts, corpus mutations or user-controlled build flags. Neither the adapter
nor broker links libFuzzer/AFL or launches any process to run a target.

## Reproducing on the local development machine

From the Tronix source checkout with the pinned Celix and msgpack-c dependencies
specified in the top-level README:

```sh
mkdir -p private && chmod 700 private

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/home/commander/windanvil-msgpack-work/.deps/local \
  -DCELIX_SOURCE=/home/commander/windanvil-msgpack-work/.deps/apache-celix \
  -DMSGPACK_SOURCE=/home/commander/windanvil-msgpack-work/.deps/msgpack-c \
  -DTRONIX_ENABLE_IHB_PREFLIGHT=ON \
  -DTRONIX_IHB_PREFLIGHT_SOCKET="$(pwd)/private/ihb.sock" \
  -DTRONIX_IHB_PREFLIGHT_UID="$(id -u)"

cmake --build build --target tronix_ihb_preflight_bundle tronixd \
  tronix_worker tronix_scheduler_bundle -j 3

IHB_NATIVE_ROOT=/absolute/path/to/approved/IHATEBANKS-project/native_fuzz \
  npm run test:ihb
```

The test starts a private preflight broker and `tronixd` only for synthetic
checks, then stops them. It directly rejects an injected command and an
unlisted Tronix operation. Set the catalog to a newly reviewed/digest-pinned
source revision when approved files legitimately change. Never regenerate the
catalog simply to bypass a mismatch or publish source digests as attestations.

The standard build does **not** contain this adapter:
`TRONIX_ENABLE_IHB_PREFLIGHT=OFF` by default. No protocol v1 changes are
introduced. The harness requires Linux SO_PEERCRED.

## Trust boundaries

The observed Butcher fixture runs the broker and Tronix under the **same
`commander` UID**. Therefore kernel peer-UID checks cannot distinguish
privileged and unprivileged processes in that fixture. This is NOT separation
of Docker privileges. Although the read-only broker never invokes Docker,
the machine's `commander` account itself belongs to the Docker group.

Production-oriented privilege separation requires a non-Docker Tronix service
identity, a separately owned/gated execution service, strict socket DAC policy,
operator authority, scope review and tests of all failure/cancellation paths.
The current broker deliberately sets socket mode 0600 and requires a private
parent directory: it is **single-UID local lab plumbing**, not cross-UID access.

The catalog pins a **reference to a local image ID**, but this preflight does
not query Docker to prove that the image exists or can run; the earlier separate
IHB container tests supply that narrower evidence. A successful result is
only a consistency check on the current reviewed source files and local catalog.

The next stage must be a separately admitted operator-owned *executor* for one
fixed synthetic profile, with complete process-tree cleanup and reconciliation.
Until then, run campaigns using IHB's existing opt-in `isolated.py` **outside**
Tronix, under direct operator control.

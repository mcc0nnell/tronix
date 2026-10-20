# Tronix sandbox profile v1 — experimental Linux Landlock

**Status:** opt-in native subprocess restrictions, verified only on Butcher.
This is a narrow, fixed-profile implementation, not a general-purpose
Bubblewrap/NsJail container or a guarantee against hostile same-UID code.

## Usage

Build `tronixd`, `tronix_worker`, and the reviewed Celix bundles, then
add `--sandbox landlock` to the supervised daemon:

```sh
./build/tronixd \
  --socket "$(pwd)/private/tronix.sock" \
  --scheduler "$(pwd)/build/tronix_scheduler.zip" \
  --adapter "$(pwd)/build/tronix_demo.zip" \
  --worker "$(pwd)/build/tronix_worker" \
  --journal "$(pwd)/private/events.mp" \
  --sandbox landlock
```

`--sandbox landlock` requires `--worker`. Both adapter inspection
and each execution subprocess apply the same kernel Landlock ruleset
*before* loading any Celix adapter bundle. Failure to create or apply
the complete ruleset (including TCP access rights) returns an error;
the main daemon refuses startup if the inspection child cannot run.
An unrecognized sandbox mode such as `bubblewrap` is rejected and
never runs a less restricted worker.

## Fixed reviewed profile, not user-supplied grants

The first profile permits:

- Read and execute under `/usr` (including the host's dynamic loader
  and libraries); read the exact canonicalized adapter archive; and
  read `/etc/ld.so.cache`.
- Read, write, create, delete and execute inside the **fresh parent-created
  private Celix cache directory** for this one subprocess.
- No Landlock TCP bind or connect rules, so all TCP binds and connects
  are denied on a kernel supporting Landlock ABI 4 or higher.

All other Landlock-handled filesystem rights are denied by default,
including reading unrelated host files and writing outside the cache.
A separate cache is created per inspection/execution child and removed
by the parent even after the child terminates abnormally.

**Not covered:** UDP and other non-TCP networking; abstract Unix sockets;
inherited file descriptors and environment variables (the existing
supervisor separately strips ambient environment and inherited descriptors,
forwarding only a minimal runtime environment plus opt-in synthetic test
fixture values); filesystem access
outside Landlock's handled rights or through previously opened handles;
same-UID process interaction; already-held privileges; resource exhaustion
beyond existing rlimits; and external side effects from an authorized
operation. The `/usr` subtree is deliberately broad for initial dynamic
linker compatibility. This profile does not hide the host's process tree
through a PID namespace and does not use seccomp, cgroups or a new UID.

The default in-process execution mode and the existing supervised
mode *without* `--sandbox` are unchanged for compatibility. Do not expose
the private socket publicly or run unreviewed Celix bundles.

## Verification on Butcher, October 20, 2026

Observed host: Linux 6.8.0-142-generic; kernel Landlock ABI 4.

`node tests/sandbox.mjs` uses the reviewed demo adapter, and tests
that:

- `demo.constant` returns 42 with Landlock enabled.
- Reading a separate private host file returns Linux `EACCES` (13);
  the source file remains unchanged.
- Creating a file outside the ephemeral Celix cache returns
  `EACCES` (13); that file never appears.
- Connecting via TCP to a **real locally listening** port returns
  `EACCES` (13), not a connection refusal.
- Unsupported `--sandbox bubblewrap` and Landlock without a worker are
  refused, rather than silently falling back.
- Scheduler admission restrictions, durable receipts and private
  agent memory continue to work.

**Bubblewrap limitation:** `/usr/bin/bwrap` v0.9.0 is present on Butcher,
but direct namespace probes fail with `setting up uid map: Permission
denied`, or with `loopback: Failed RTM_NEWADDR: Operation not permitted`.
We did **not** wire up Bubblewrap as an active backend or claim it works.
This native Landlock alternative gives a tested first restriction layer
without downgrading from a requested `bubblewrap` mode.

## Next hardening work

1. Pin the adapter archive to a stable, reviewed artifact identity;
   current execution checks operation names, lanes and budgets, not an
   immutable binary hash or sealed file descriptor.
2. Add an explicit network policy that also handles UDP and Unix sockets
   where supported; Linux ABI 4 Landlock alone cannot enforce this.
3. Separate workers into dedicated Unix UIDs with audited socket rights;
   the current UID-bound admission applies to client connections, not
   sandboxed child processes.
4. Add startup/sandbox-status receipts that distinguish denied sandbox
   initialization from an ordinary adapter failure, and independently
   attest host kernel features.

This feature is intentionally a draft PR pending hosted CI and security
review. Local tests are evidence of the exact operations tested, **not**
proof of comprehensive sandboxing.

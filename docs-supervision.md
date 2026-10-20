# Tronix supervised execution (Linux pilot)

An opt-in `--worker /absolute/path/to/tronix_worker` launches each reviewed
Celix adapter operation in a fresh subprocess using `posix_spawn`.
The parent daemon retains the native scheduler but does NOT load the
adapter in supervised mode. At startup, a bounded inspection child enumerates
the reviewed operations, priorities and budgets; the daemon freezes the
snapshot. Each execution child independently revalidates the requested
operation and its expected priority/budget before executing. Clients cannot
supply executable paths or arbitrary commands.

## Run on the isolated local host

```sh
cmake --build build --target tronixd tronix_worker tronix_scheduler_bundle tronix_demo_bundle -j 3
# Add to the normal private daemon invocation:
#   --worker "$(pwd)/build/tronix_worker"
node tests/isolation.mjs
```

Using `--policy` is recommended; the isolation test uses a private UID policy.
The child runs a one-shot Celix framework in a private cache and emits a
bounded MessagePack result on fd 3. The parent accepts the result only after
successful child exit and strict decoding.

### Credential and descriptor hygiene

On this Linux/glibc implementation, the supervisor does not forward the
daemon's environment. It sends fixed PATH and C locale settings (plus
explicit synthetic test-only fixture variables), and closes fd 4 and above using
the glibc close-from spawn action, and points child stdin/stdout at /dev/null.
The child retains stderr for diagnostics and the MessagePack reply pipe on
fd 3. A worker also applies Linux NO_NEW_PRIVS to prevent gaining privileges
through subsequent executable transitions.

The integration test places a synthetic variable in the daemon's environment
and an extra canary file descriptor in the daemon, verifies the daemon has
the descriptor, then proves the worker does not observe either canary. This
checks inheritance hygiene, not whether malicious same-UID code can read
the same files or inspect accessible processes by another route.

A Celix initialization experiment showed that changing the worker's current
directory before framework startup breaks this pinned build. The worker
therefore retains its inherited working directory for now. Do not treat
this feature as current-directory or filesystem isolation.

The child is supervised up to the trusted operation budget **plus 200 ms**
for startup. If the child has not exited by then, the supervisor sends
SIGKILL to its process/session group, reaps the worker PID, cleans its cache,
and records the elapsed operation as overrun. A crash is reported as failure
without taking down the daemon. The scheduler still enforces its original
trusted execution budget when deciding whether to expose a successful result.

## Tested behavior

The integration test covers child-only adapter inspection, PID separation,
policy-drift rejection, worker crash and hang termination, durable receipts,
later work, synthetic secret/descriptor canaries and NO_NEW_PRIVS.
The base smoke, hardening, UID-policy and ledger tests continue running.

## Security limits

**Fault containment is not a hostile-code sandbox.** In supervised mode,
adapter policy inspection and execution run in children, not in the daemon.
Workers still use the daemon's UID, filesystem and network permissions,
though the worker's inherited
environment and unrelated descriptors are stripped. A malicious same-UID
worker can still access permitted files and potentially inspect processes.
Resource limits and SIGKILL cannot undo completed side effects or reliably
stop descendants that escape the worker session.
No automatic rollback, exactly-once execution, hard-real-time latency,
multi-tenant safety, or arbitrary plugin isolation is claimed. Load only
reviewed bundles; do not grant an untrusted agent the worker process UID.

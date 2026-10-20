# Tronix supervised execution (Linux pilot)

An opt-in `--worker /absolute/path/to/tronix_worker` launches each reviewed
Celix adapter operation in a fresh subprocess using `posix_spawn`.
The parent daemon retains the scheduler and still loads the trusted adapter
to run `lane_for` and `budget_for`. It sends **only** a fixed, validated
operation name to the worker; clients cannot supply executable paths.

## Run on the isolated local host

```sh
cmake --build build --target tronixd tronix_worker tronix_scheduler_bundle tronix_demo_bundle -j 3
# Add to the normal private daemon invocation:
#   --worker "$(pwd)/build/tronix_worker"
node tests/isolation.mjs
```

Using `--policy` is recommended; the isolation test uses a private UID policy.
The child runs a one-shot Celix framework in a private cache and emits a
bounded MessagePack result on a dedicated inherited descriptor. The parent
accepts the result only after successful child exit and strict decoding.

The child is supervised up to the trusted operation budget **plus 200 ms**
for startup. If the child has not exited by then, the supervisor sends
SIGKILL to its process/session group, reaps the worker PID, cleans its cache,
and records the elapsed operation as overrun. A crash is reported as failure
without taking down the daemon. The scheduler still enforces its original
trusted execution budget when deciding whether to expose a successful result.

## Tested behavior

The integration test covers: PID separation, authorized operation execution,
child process crash, hung child termination, receipt preservation, completion
of a later job, worker cache cleanup, and rejection of arbitrary commands.
The base smoke, hardening, UID-policy and ledger tests continue running.

## Security limits

**Fault containment is not a hostile-code sandbox.** The daemon still loads
native adapter code for admission policy and workers still use the daemon's
UID, environment, filesystem and network privileges. Resource limits and
SIGKILL cannot undo completed side effects, stop a descendant that escapes
the worker session, or protect credentials available to the worker.
No automatic rollback, exactly-once execution, hard-real-time latency,
multi-tenant safety, or arbitrary plugin isolation is claimed. Load only
reviewed bundles; do not grant an untrusted agent the worker process UID.

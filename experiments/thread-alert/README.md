# Thread emergency alert receipt lab

**Experimental, non-life-safety test harness.** This demonstration uses four actual
`ot-cli-ftd` processes in OpenThread's POSIX simulated-radio network. It does
not create a real emergency warning, access IPAWS, or operate physical devices.

```text
synthetic evacuation gateway  (OpenThread leader, UDP 61617)
            │
            ├── UDP EA1|drill001|EVACUATE --> "strobe"  (UDP 61616)
            ├── UDP EA1|drill001|EVACUATE --> "display" (UDP 61616)
            └── UDP EA1|drill001|EVACUATE --> "haptic"  (UDP 61616)
                      <-- UDP ACK1|drill001|<receiver> from each live receiver
```

## Run

From the Tronix repository checkout, with Node 22 and the POSIX simulation
`ot-cli-ftd` executable already built:

```sh
OT_CLI_PATH=/absolute/path/to/ot-cli-ftd \
  node experiments/thread-alert/lab.mjs
```

The lab has **no additional npm dependencies**. It starts node IDs 10–13
(default; override with `THREAD_ALERT_BASE_ID=18`, for example). It creates a
mode-0700 private temporary working directory with fresh simulated flash per
run, and a randomized OpenThread `PORT_OFFSET` for radio-port isolation.
Override the latter with `THREAD_ALERT_PORT_OFFSET=75` only for controlled
reproduction when no other simulation uses that offset. The simulated nodes
receive a fresh operational dataset generated in-process; it is not logged.
The private temporary directory is removed after the spawned nodes exit.

## Assertions

1. All three healthy alert devices attach to the same real simulated Thread
   network and each returns a separate application UDP acknowledgement.
2. Re-sending the same alert ID yields a fresh ACK without invoking a second
   visual/haptic activation in the harness application.
3. After killing the simulated haptic receiver, a second alert is ACKed by
   the two live nodes; the missing haptic receipt is **reported missing**,
   even though the sender's UDP command may still return `Done`.
4. The final JSON includes per-recipient observed time from the start of send
   to the first matching **fresh** ACK. The harness exits nonzero on
   failed assertions.

## Evidence and limitations

- Output describes receipt at the **simulated receiver application**. It does
  **not** prove that a human saw, felt, or understood an alert.
- Duplicate suppression is in this Node.js harness, **not in OpenThread
  firmware**, and resets when the harness restarts.
- `EA1` / `ACK1` are lab-only ASCII framing formats. There is **no**
  CAP parsing, application signing, authorization, anti-replay persistence,
  COAP, multicast fanout, retry queue, delivery deadline policy, or external
  emergency-alert integration. Thread's own link security alone does not
  authenticate an emergency alert's originating authority.
- This is an isolated UDP test of the actual OpenThread data path, **not**
  an RF interference, power-failure, sleepy-end-device latency, or certification test.
- Never connect this fixture to a real alert actuator or represent it as
  certified or code-compliant fire/life-safety infrastructure.
- No modifications to Tronix's existing read-only Thread bridge,
  authority boundaries, or ongoing upstream OpenThread branches.

## Sensible next increment

Use a bounded, application-authenticated alert envelope, durable replay
tracking, signed source-policy verification, retries under congestion and
partitions, and a small separate real-hardware test. Treat the proof of
receipt as a measured observation, not a delivery guarantee.

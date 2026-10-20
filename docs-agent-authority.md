# Tronix UID-bound admission — experimental

This is an **optional** Linux-local authority mechanism for the private
MessagePack-over-AF_UNIX interface. It replaces caller-chosen `agent`
labels with an immutable administrator-authored binding from Linux UID to
one agent label and an allowlist of operation names.

## Setup

Create a private configuration file readable only by the daemon owner:

```sh
mkdir -p private && chmod 700 private
printf '%s trusted-agent demo.constant,demo.pause\n' "$(id -u)" > private/authority.policy
chmod 600 private/authority.policy
```

Start `tronixd` with its normal arguments and also:

```sh
  --authority "$(pwd)/private/authority.policy"
```

One line per UID, in the form `UID agent operation1,operation2`.
Comments beginning with `#` are allowed on their own lines. The parser
rejects duplicate UIDs, duplicate agent names, duplicate operations,
invalid names, trailing commas, and files that are not single-link
regular files owned by the effective daemon UID with no group/other
permission bits. Maximum policy size is 8 KiB, with at most 32 UID
records, one agent per UID, and 16 allowlisted operations per identity.

## Enforcement

On every accepted Unix socket connection, Tronix calls Linux
`getsockopt(SOL_SOCKET, SO_PEERCRED)` and looks up the kernel-reported
peer UID in its frozen policy. An unlisted UID receives a `DENIED`
response. If the kernel credential lookup cannot be performed, the
connection is rejected.

- **submit**: the caller-supplied `agent` must equal the mapped agent
  and `operation` must be allowed by both that UID's policy and the
  trusted Celix adapter policy. The server still selects scheduling
  lane and execution budget.
- **capabilities**: returns only operations in both allowlists.
- **memory_put/get/delete/list**: every request must name exactly the
  assigned agent. The journal is not exposed to other mapped agents.
- **result / ack**: only the mapped agent's in-memory jobs can be read
  or acknowledged; foreign job IDs behave as absent / not terminal.
- **receipt / events**: returns only the mapped agent's receipts and
  lifecycle events, including older durable records loaded on startup.
  Event cursors retain global sequence numbers and may have gaps.
- **snapshot**: disabled in UID-bound mode because it exposes
  aggregate activity across different agents.
- **ping**: succeeds for mapped UIDs and is denied for unmapped UIDs.

The existing v1 MessagePack requests are unchanged. `--authority`
is opt-in; no authority file means the previous single-user
unauthenticated mode is retained. The policy is reloaded only at daemon
restart. Authenticated mode refuses a malformed policy at startup.

## Important limits

This is **UID-bound authorization, not process identity**. Every
process running under the same Linux UID can claim exactly the same
agent and operations; neither a PID nor a user-supplied agent label is
a secure distinct principal. No separate-UID clients are presently
provisioned by the default socket: it is created mode `0600`, so only
the owning Unix user (and privileged processes) can connect. Mapping
multiple UIDs does **not** make the socket accessible to them. A
multi-user deployment requires separately designed and tested socket
permissions, privilege separation, and real worker sandboxing.

This is not a secrets vault and does not supply encryption or
isolated filesystem/network rights. File owners and privileged
administrators can modify authority or data on disk. Disabling this
optional mode when restarting a daemon will remove the protocol
restrictions on existing journals; keep the private socket protected.

Test with `node tests/authority.mjs`. The test verifies peer-UID
binding, operation filtering, legacy-to-authenticated journal replay,
memory and receipt isolation, and malformed config refusal. The
test's one observed UID does not prove cross-UID operation or
kernel-level multi-user isolation.

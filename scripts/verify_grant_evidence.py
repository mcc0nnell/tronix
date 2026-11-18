#!/usr/bin/env python3
# Copyright 2026 Robert McConnell
# SPDX-License-Identifier: Apache-2.0
"""Independently verify TRONIX operator grants and terminal receipt HMAC chain."""
import argparse
import hashlib
import hmac
import json
import re
import sys
import zipfile
from pathlib import Path
from grant_checkpoint import (create_checkpoint_file, read_anchor_key,
                              read_checkpoint_file, sign_checkpoint, verify_checkpoint)


def fail(message):
    raise ValueError(message)


def read_grants(raw, archive):
    try:
        lines = raw.decode("ascii").splitlines(keepends=True)
    except UnicodeError:
        fail("grants are not ASCII")
    if not lines or lines[0] != "tronix.grants.v1\n" or any(
        not line.endswith("\n") for line in lines
    ):
        fail("invalid grants format")
    fields = {}
    allowed = set()
    for line in lines[1:]:
        parts = line.rstrip("\n").split(" ")
        if len(parts) != 2:
            fail("invalid grant line")
        key, value = parts
        if key == "allow":
            if not re.fullmatch(r"[A-Za-z0-9._-]{1,48}", value) or value in allowed:
                fail("invalid or duplicate allowed operation")
            allowed.add(value)
        elif key in ("bundle", "version", "artifact"):
            if key in fields:
                fail("duplicate grant key")
            fields[key] = value
        else:
            fail("unknown grant key")
    if set(fields) != {"bundle", "version", "artifact"} or not allowed:
        fail("missing grant fields")
    if fields["artifact"] != hashlib.sha256(archive).hexdigest():
        fail("grant archive digest differs from exact file")
    if not re.fullmatch(r"[a-z0-9]{64}", fields["artifact"]):
        fail("invalid artifact hex")
    with zipfile.ZipFile(__import__("io").BytesIO(archive)) as z:
        manifest = json.loads(z.read("META-INF/MANIFEST.json"))
    if (manifest.get("CELIX_BUNDLE_SYMBOLIC_NAME") != fields["bundle"] or
        manifest.get("CELIX_BUNDLE_VERSION") != "version<" + fields["version"] + ">"):
        fail("bundle name/version mismatch")
    return fields, allowed


def verify(key, raw_grants, archive, raw_evidence, checkpoint_sequences=frozenset()):
    fields, allowed = read_grants(raw_grants, archive)
    grant_hash = hashlib.sha256(raw_grants).hexdigest()
    prev = "0" * 64
    count = 0
    checkpoint_heads = {}
    artifact_references = []
    for line_number, line in enumerate(raw_evidence.splitlines(keepends=True), 1):
        if not line.endswith(b"\n"):
            fail(f"evidence line {line_number}: truncated final record")
        try:
            parts = line[:-1].decode("ascii").split(" ")
        except UnicodeError:
            fail(f"evidence line {line_number}: invalid encoding")
        if any(not p for p in parts):
            fail(f"evidence line {line_number}: empty field")
        if len(parts) == 12 and parts[0] == "v1":
            v, seq, job_id, agent, op, state, dispatch, value, artifact, grant, prior, mac = parts
            artifact_digest = None
        elif len(parts) == 13 and parts[0] == "v2":
            (v, seq, job_id, agent, op, state, dispatch, value,
             artifact_digest, artifact, grant, prior, mac) = parts
            if state != "completed" or not re.fullmatch(r"[0-9a-f]{64}", artifact_digest):
                fail(f"evidence line {line_number}: invalid attached evidence digest")
        else:
            fail(f"evidence line {line_number}: invalid record version/field count")
        if seq != str(line_number) or            (not dispatch.isdigit()) or dispatch.startswith("0") and dispatch != "0":
            fail(f"evidence line {line_number}: sequence or dispatch invalid")
        if artifact != fields["artifact"] or grant != grant_hash or prior != prev:
            fail(f"evidence line {line_number}: grant/chain identity differs")
        def decodehex(field):
            if not re.fullmatch(r"(?:[0-9a-f]{2})+", field):
                fail(f"evidence line {line_number}: invalid lowercase field hex")
            return bytes.fromhex(field).decode("ascii")
        for item in (job_id, agent, op):
            decodehex(item)
        if decodehex(op) not in allowed:
            fail(f"evidence line {line_number}: ungranted operation")
        if state not in ("completed", "failed", "overrun", "expired"):
            fail(f"evidence line {line_number}: invalid terminal state")
        if state == "completed":
            if not re.fullmatch(r"-?(?:0|[1-9][0-9]*)", value):
                fail(f"evidence line {line_number}: invalid completed value")
            if not -(1 << 63) <= int(value) < (1 << 63):
                fail(f"evidence line {line_number}: value overflow")
        elif value != "-":
            fail(f"evidence line {line_number}: unexpected value")
        if not re.fullmatch(r"[0-9a-f]{64}", mac):
            fail(f"evidence line {line_number}: invalid MAC")
        expected = hmac.new(key, " ".join(parts[:-1]).encode("ascii"),
                            hashlib.sha256).hexdigest()
        if not hmac.compare_digest(mac, expected):
            fail(f"evidence line {line_number}: HMAC mismatch")
        if artifact_digest is not None:
            artifact_references.append({"sequence": line_number,
                                        "id": decodehex(job_id),
                                        "operation": decodehex(op),
                                        "reportSha256": artifact_digest})
        prev = mac
        count += 1
        if count in checkpoint_sequences:
            checkpoint_heads[count] = mac
    if count == 0:
        fail("no terminal receipts present")
    return count, prev, grant_hash, fields["artifact"], checkpoint_heads, artifact_references


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--key", required=True)
    parser.add_argument("--grants", required=True)
    parser.add_argument("--adapter", required=True)
    parser.add_argument("--evidence", required=True)
    parser.add_argument("--artifact-dir", help="Optional private Rizzer evidence directory to verify against signed v2 digests")
    checkpoints = parser.add_mutually_exclusive_group()
    checkpoints.add_argument("--checkpoint", action="append", default=[],
                             help="One or more separately retained operator checkpoints")
    checkpoints.add_argument("--write-checkpoint", metavar="NEW_FILE",
                             help="Create an operator-signed checkpoint; export it off-host")
    parser.add_argument("--checkpoint-key",
                        help="Separate, owner-private 32-byte operator checkpoint signing key")
    args = parser.parse_args()
    try:
        key = Path(args.key).read_bytes()
        if len(key) != 32:
            fail("verification key must contain 32 bytes")
        uses_checkpoints = bool(args.checkpoint or args.write_checkpoint)
        if uses_checkpoints != bool(args.checkpoint_key):
            fail("checkpoint operations require an independent --checkpoint-key")
        operator_key = read_anchor_key(args.checkpoint_key) if uses_checkpoints else None
        if operator_key is not None and hmac.compare_digest(operator_key, key):
            fail("operator checkpoint key must differ from daemon receipt key")
        checkpoints = []
        if len(args.checkpoint) > 64:
            fail("too many operator checkpoints")
        for path in args.checkpoint:
            checkpoints.append(verify_checkpoint(read_checkpoint_file(path), operator_key))
        sequences = {c["sequence"] for c in checkpoints}
        count, head, grant, artifact, heads, references = verify(
            key, Path(args.grants).read_bytes(), Path(args.adapter).read_bytes(),
            Path(args.evidence).read_bytes(), sequences)
        if args.artifact_dir:
            if not references:
                fail("no HMAC-signed v2 artifact digests to verify")
            root = Path(args.artifact_dir)
            if not root.is_dir() or root.is_symlink():
                fail("invalid artifact evidence root")
            files = sorted(root.glob("run-*/native-parity.json"))
            if len(files) > 8 or len(files) != len(references):
                fail("artifact evidence count differs from signed digest count")
            observed = []
            for file in files:
                if file.is_symlink() or file.parent.is_symlink() or file.stat().st_size > 65536:
                    fail("untrusted native parity report file")
                observed.append(hashlib.sha256(file.read_bytes()).hexdigest())
            if sorted(observed) != sorted(ref["reportSha256"] for ref in references):
                fail("artifact bytes differ from HMAC-signed v2 report digest")
        for checkpoint in checkpoints:
            position = checkpoint["sequence"]
            if position > count:
                fail("receipt rollback or truncation below independent checkpoint")
            if (checkpoint["chainHead"] != heads[position]
                    or checkpoint["grantSha256"] != grant
                    or checkpoint["artifactSha256"] != artifact):
                fail("verified receipt chain diverges from independent checkpoint")
        if args.write_checkpoint:
            raw = sign_checkpoint(operator_key, count, head, grant, artifact)
            create_checkpoint_file(args.write_checkpoint, raw)
        print(json.dumps({"schema": "tronix.grant-evidence.v1", "verified": count,
                          "chainHead": head, "grantSha256": grant,
                          "artifactSha256": artifact,
                          "artifactReferences": references,
                          "artifactFilesVerified": bool(args.artifact_dir),
                          "checkpointMode": ("created" if args.write_checkpoint else
                                             "verified" if checkpoints else "none"),
                          "anchoredPrefix": max(sequences, default=0),
                          "checkpointCount": len(checkpoints),
                          "limitations": ("Only independently retained checkpoints prevent "
                                          "undetected rollback of an anchored prefix; "
                                          "later uncheckpointed suffixes remain unprotected")},
                         indent=2))
    except Exception as exc:
        print(f"TRONIX EVIDENCE REJECTED: {exc}", file=sys.stderr)
        raise SystemExit(1)


if __name__ == "__main__":
    main()

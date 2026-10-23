#!/usr/bin/env python3
# Copyright 2026 Robert McConnell
# SPDX-License-Identifier: Apache-2.0
"""Check D1 SQL on real SQLite, independent of the JavaScript D1 fixture."""
import pathlib
import re
import sqlite3

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCHEMA = ROOT / "cloudflare/migrations/0001_checkpoints.sql"
WORKER = ROOT / "cloudflare/checkpoint-worker.mjs"
source = WORKER.read_text()
match = re.search(r"const insertSQL = `(.+?)`;", source, re.S)
if not match:
    raise SystemExit("could not find exact Worker insert SQL")

conn = sqlite3.connect(":memory:")
conn.executescript(SCHEMA.read_text())
sha_a = "a" * 64
sha_b = "b" * 64
head_1 = "c" * 64
head_2 = "d" * 64
payload = '{"test":true}\n'
digest = "e" * 64


def insert(sequence, head, artifact=sha_a, grant=sha_b):
    p = (artifact, grant, sequence, head, payload, digest,
         artifact, grant, sequence)
    return conn.execute(match.group(1), p).rowcount


assert insert(1, head_1) == 1, "initial insert should succeed"
assert insert(1, head_1) == 0, "identical retry cannot insert"
assert insert(2, head_2) == 1, "higher sequence should insert"
assert insert(1, head_1) == 0, "stale sequence should not insert"
assert insert(2, head_1) == 0, "same sequence conflict cannot insert"
assert insert(1, head_1, grant="f"*64) == 1, "independent grant namespace"
assert conn.execute("SELECT COUNT(*) FROM tronix_checkpoints").fetchone()[0] == 3

for statement in (
    "UPDATE tronix_checkpoints SET chain_head = '0' WHERE seq = 1",
    "DELETE FROM tronix_checkpoints WHERE seq = 1",
):
    try:
        conn.execute(statement)
        raise AssertionError("mutation unexpectedly accepted")
    except sqlite3.IntegrityError:
        pass

try:
    conn.execute("""INSERT INTO tronix_checkpoints
      (artifact_sha,grant_sha,seq,chain_head,payload,payload_sha256)
      VALUES (?,?,?,?,?,?)""",
      (sha_a,sha_b,1,head_1,payload,digest))
    raise AssertionError("direct bypassed stale insert should fail")
except sqlite3.IntegrityError:
    pass

print("TRONIX Cloudflare D1 migration + atomic monotonic insert: PASS (9 checks)")

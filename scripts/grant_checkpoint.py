#!/usr/bin/env python3
# Copyright 2026 Robert McConnell
# SPDX-License-Identifier: Apache-2.0
"""Operator-held checkpoint primitives for independently verified TRONIX receipts.

A checkpoint is useful for rollback detection only when its key and signed
checkpoint are retained outside the daemon's write-access trust boundary.
This module does not transmit, schedule, or publish checkpoints.
"""
import hashlib
import hmac
import json
import os
import re
import stat

SCHEMA = "tronix.evidence-checkpoint.v1"
FIELDS = frozenset(("schema", "sequence", "chainHead", "grantSha256", "artifactSha256"))
HEX256 = re.compile(r"[0-9a-f]{64}\Z")


def _fail(reason):
    raise ValueError(reason)


def _hex(value):
    return isinstance(value, str) and HEX256.fullmatch(value) is not None


def _serialize(payload):
    return json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")


def read_anchor_key(path):
    """Read only an owner-private, exact 32-byte file without following links."""
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(fd)
        if not (stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid()
                and info.st_nlink == 1 and (info.st_mode & 0o077) == 0
                and info.st_size == 32):
            _fail("operator checkpoint key must be an owner-private 32-byte regular file")
        key = b""
        while len(key) < 32:
            chunk = os.read(fd, 32 - len(key))
            if not chunk:
                _fail("checkpoint key truncated")
            key += chunk
        return key
    finally:
        os.close(fd)


def sign_checkpoint(key, sequence, chain_head, grant_hash, artifact_hash):
    if type(sequence) is not int or sequence < 1 or not all(
            _hex(value) for value in (chain_head, grant_hash, artifact_hash)):
        _fail("invalid checkpoint fields")
    fields = dict(schema=SCHEMA, sequence=sequence, chainHead=chain_head,
                  grantSha256=grant_hash, artifactSha256=artifact_hash)
    signature = hmac.new(key, _serialize(fields), hashlib.sha256).hexdigest()
    return _serialize({**fields, "signature": signature}) + b"\n"


def verify_checkpoint(raw, key):
    if not raw.endswith(b"\n") or len(raw) > 2048:
        _fail("checkpoint must be bounded and newline-terminated")
    try:
        data = json.loads(raw.decode("ascii"),
                          object_pairs_hook=_unique_object)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("malformed checkpoint") from exc
    if type(data) is not dict or set(data) != FIELDS | {"signature"}:
        _fail("checkpoint has unknown or missing fields")
    if data["schema"] != SCHEMA or type(data["sequence"]) is not int or data["sequence"] < 1:
        _fail("invalid checkpoint version or sequence")
    if not all(_hex(data[k]) for k in ("chainHead", "grantSha256",
                                      "artifactSha256", "signature")):
        _fail("invalid checkpoint hashes")
    unsigned = {k: data[k] for k in FIELDS}
    computed = hmac.new(key, _serialize(unsigned), hashlib.sha256).hexdigest()
    if not hmac.compare_digest(data["signature"], computed):
        _fail("operator checkpoint HMAC mismatch")
    return data


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            _fail("duplicate checkpoint JSON key")
        result[key] = value
    return result


def create_checkpoint_file(path, raw):
    """Create-only with fsync: caller must export to an independent trust domain."""
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC | os.O_NOFOLLOW,
                 0o600)
    try:
        written = 0
        while written < len(raw):
            n = os.write(fd, raw[written:])
            if n <= 0:
                _fail("checkpoint write incomplete")
            written += n
        os.fsync(fd)
    except BaseException:
        os.close(fd)
        os.unlink(path)
        raise
    os.close(fd)


def read_checkpoint_file(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK)
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            _fail("operator checkpoint must be a regular file")
        data = os.read(fd, 2049)
        if len(data) > 2048:
            _fail("checkpoint exceeds 2 KiB")
        return data
    finally:
        os.close(fd)

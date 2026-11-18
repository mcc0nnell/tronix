#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify TRONIX's vendored snapshot against a reviewed, immutable NeonOxide lock.

Fully offline: TRONIX CI never contacts or authenticates to private NeonOxide.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

REPOSITORY = "https://github.com/mcc0nnell/NeonOxide"
CANONICAL_COMMIT = "c408c190b8e1bc30df7aa23cfd9677e88eb6afb6"
ORIGIN_REPOSITORY = "https://github.com/mcc0nnell/tronix"
ORIGIN_COMMIT = "019e681d43e5906ec0c42b63ea61c2f17f3a3cab"
EXPECTED_TREE = "fe3896262506d9bfb62152f563987ba28e84b9d509c74cad541e87e718ef2911"
EXPECTED_SOURCE_MANIFEST = "332b0b5dbe54fb777299d64e29c4c97465271e5e42999129ce00fa39767d5381"
SOURCE_FILES = (
    "CMakeLists.txt",
    "include/watchcore/watchcore.hpp",
    "src/shadow_security.cpp",
    "src/watchcore.cpp",
    "tests/watchcore_tests.cpp",
)
PINNED_FILES = tuple(sorted((*SOURCE_FILES, "LICENSE", "NOTICE")))
LOCK_KEYS = {
    "schema", "version", "canonical_repository", "canonical_source_commit",
    "canonical_path", "origin_repository", "origin_commit", "origin_path",
    "source_manifest_sha256", "files_sha256", "tree_sha256",
}
ROOT = Path(__file__).resolve().parents[1]


def _no_duplicate_fields(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate field: {key}")
        result[key] = value
    return result


def _safe_read(path: Path, anchor: Path) -> bytes:
    # All paths are statically allowlisted; reject all symlinks, including parents.
    relative = path.relative_to(anchor)
    cur = anchor
    for segment in relative.parts:
        cur = cur / segment
        if cur.is_symlink():
            raise ValueError(f"symlink forbidden: {relative}")
    if not path.is_file():
        raise ValueError(f"missing snapshot file: {relative}")
    return path.read_bytes()


def verify(root: Path = ROOT) -> dict:
    root = root.resolve()
    watchcore = root / "integrations" / "atlas-watchcore"
    lock_bytes = _safe_read(watchcore / "NEONOXIDE_SOURCE_LOCK.json", root)
    lock = json.loads(lock_bytes, object_pairs_hook=_no_duplicate_fields)
    if set(lock) != LOCK_KEYS:
        raise ValueError("invalid release lock shape")
    fixed = {
        "schema": "neonoxide.watchcore.release.v1",
        "version": "0.1.0",
        "canonical_repository": REPOSITORY,
        "canonical_source_commit": CANONICAL_COMMIT,
        "canonical_path": "components/watchcore",
        "origin_repository": ORIGIN_REPOSITORY,
        "origin_commit": ORIGIN_COMMIT,
        "origin_path": "integrations/atlas-watchcore",
        "tree_sha256": EXPECTED_TREE,
    }
    for key, value in fixed.items():
        if lock[key] != value:
            raise ValueError(f"unexpected pinned {key}")
    if lock["source_manifest_sha256"] != EXPECTED_SOURCE_MANIFEST:
        raise ValueError("unexpected source manifest digest")
    pins = lock["files_sha256"]
    if not isinstance(pins, dict) or set(pins) != set(PINNED_FILES):
        raise ValueError("invalid snapshot file set")
    for rel in PINNED_FILES:
        pin = pins[rel]
        if not isinstance(pin, str) or not re.fullmatch(r"[0-9a-f]{64}", pin):
            raise ValueError(f"invalid file hash for {rel}")
        # LICENSE and NOTICE already live at public TRONIX's root; no private source dependency.
        path = root / rel if rel in ("LICENSE", "NOTICE") else watchcore / rel
        if hashlib.sha256(_safe_read(path, root)).hexdigest() != pin:
            raise ValueError(f"Watchcore snapshot drift: {rel}")
    serialized = "".join(f"{name}\0{pins[name]}\n" for name in sorted(pins)).encode()
    tree = hashlib.sha256(serialized).hexdigest()
    if tree != EXPECTED_TREE:
        raise ValueError("unexpected snapshot tree digest")
    return lock


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=ROOT, help="local TRONIX checkout")
    args = parser.parse_args()
    lock = verify(args.root)
    print(f"TRONIX Watchcore source lock PASS: {lock['version']}, "
          f"{len(PINNED_FILES)} pinned files, canonical {CANONICAL_COMMIT[:12]}")


if __name__ == "__main__":
    main()

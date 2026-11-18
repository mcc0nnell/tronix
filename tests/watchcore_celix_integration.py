#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Actual Apache Celix lifecycle differential and fail-closed regression.

This exercises the externally pinned C++20 bundle (not a mock Celix service).
The original stateless Watchcore CLI remains an independent native oracle.
Python Atlas Watcher authority is tested separately in Rizzer.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile


def call(binary: Path, case: list[int], env: dict[str, str]):
    return subprocess.run(
        [str(binary), "security", *(str(x) for x in case)],
        capture_output=True, text=True, env=env, timeout=5, check=False,
    )


def main() -> int:
    if len(sys.argv) not in (4, 5):
        raise SystemExit("usage: test.py ABS_CELIX_HOST ABS_REFERENCE ABS_BUNDLE [ABS_WRONG_SERVICE_BUNDLE]")
    host, reference, bundle = [Path(x).resolve() for x in sys.argv[1:4]]
    wrong_service = Path(sys.argv[4]).resolve() if len(sys.argv) == 5 else None
    for item in (host, reference, bundle):
        assert item.is_file(), item
    pin = hashlib.sha256(bundle.read_bytes()).hexdigest()
    env = {"PATH": "/usr/bin:/bin", "WATCHCORE_CELIX_BUNDLE": str(bundle),
           "WATCHCORE_CELIX_BUNDLE_SHA256": pin}
    randomizer = random.Random(0xC311A)
    cases = [[65, 2, 2, 0, 10, 0, 0, 0, 1, 2, 1],
             [0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1],
             [100, 3, 1, 0, 0, 0, 0, 0, 1, 2, 1],
             [10, 0, 0, 0, 10, 0, 1, 0, 1, 2, 0]]
    for _ in range(340):
        warn = randomizer.choice([0, 1, 2, 3])
        block = randomizer.choice([warn, 1, 2, 3])
        cases.append([
            randomizer.randint(-10, 120), randomizer.randrange(4),
            randomizer.randrange(5), randomizer.randrange(2),
            randomizer.randint(-10, 120), randomizer.randrange(4),
            randomizer.randrange(2), randomizer.choice([-4, 0, 1, 3]),
            warn, block, randomizer.randrange(2),
        ])
    for index, case in enumerate(cases):
        r = call(reference, case, {"PATH": "/usr/bin:/bin"})
        c = call(host, case, env)
        assert r.returncode == 0, (index, case, r.stderr)
        assert c.returncode == 0, (index, case, c.stderr)
        assert json.loads(c.stdout) == json.loads(r.stdout), (index, case, c.stdout, r.stdout)
        assert set(json.loads(c.stdout)) == {
            "score", "risk", "decision", "block_downgraded_to_warn",
            "fallback_upgraded_to_warn",
        }
    print(f"PASS: {len(cases)} actual Celix lifecycle calls match independent original C++ oracle")

    normal = cases[0]
    malformed = [
        ({}, normal),
        ({"WATCHCORE_CELIX_BUNDLE": str(bundle)}, normal),
        ({"WATCHCORE_CELIX_BUNDLE_SHA256": pin}, normal),
        (env | {"WATCHCORE_CELIX_BUNDLE_SHA256": "0" * 64}, normal),
        (env | {"WATCHCORE_CELIX_BUNDLE_SHA256": "A" * 64}, normal),
        (env, normal[:3] + [100001] + normal[4:]),
        (env, normal[:3] + [-1] + normal[4:]),
        (env, normal[:5] + [8] + normal[6:]),
        (env, normal[:6] + [8] + normal[7:]),
    ]
    with tempfile.TemporaryDirectory(prefix="watchcore-celix-test-") as td:
        root = Path(td)
        fake = root / "invalid.zip"
        fake.write_bytes(b"not a Celix bundle")
        link = root / "link.zip"
        link.symlink_to(bundle)
        malformed.extend([
            (env | {"WATCHCORE_CELIX_BUNDLE": str(link)}, normal),
            (env | {"WATCHCORE_CELIX_BUNDLE": str(fake),
                    "WATCHCORE_CELIX_BUNDLE_SHA256": hashlib.sha256(fake.read_bytes()).hexdigest()}, normal),
        ])
        # A valid, pinned Celix bundle that registers the WRONG service
        # must fail at versioned Watchcore lookup (not call the local core).
        if wrong_service is not None:
            assert wrong_service.is_file()
            malformed.append((
                env | {
                    "WATCHCORE_CELIX_BUNDLE": str(wrong_service),
                    "WATCHCORE_CELIX_BUNDLE_SHA256":
                        hashlib.sha256(wrong_service.read_bytes()).hexdigest(),
                }, normal,
            ))
        for index, (supplied, case) in enumerate(malformed):
            result = call(host, case, supplied)
            assert result.returncode != 0 and not result.stdout.strip(), (
                index, result.returncode, result.stdout, result.stderr)
    print(f"PASS: {len(malformed)} invalid/missing/pin/symlink/service inputs fail closed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

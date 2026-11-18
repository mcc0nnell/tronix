#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Pinned real Celix ledger service: lifecycle, boundary and Python parity.

Run with 4 args for public CI; optional 5th Rizzer root enables comparison
against the ACTUAL Python ledger module (with file-bound classifiers stubbed).
No provider, git mutation, network or posting calls.
"""
from __future__ import annotations

from datetime import UTC, datetime
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import types
from unittest.mock import patch

STATUS = ("done", "security_blocked", "failed", "deferred", "other")


def run(host: Path, bundle: Path, request: str, *, pinned: bool = True):
    environment = {
        "PATH": "/usr/bin:/bin",
        "WATCHCORE_CELIX_BUNDLE": str(bundle),
        "WATCHCORE_CELIX_BUNDLE_SHA256":
            hashlib.sha256(bundle.read_bytes()).hexdigest() if pinned else "0" * 64,
    }
    return subprocess.run(
        [str(host), "ledger"], input=request,
        capture_output=True, text=True, check=False, timeout=5,
        env=environment,
    )


def baseline(**updates):
    result = {
        "repo": "example/atlas", "kind": 0, "number": 42,
        "revision": "sha123", "default_repo": "example/atlas",
        "now": 1773001500, "max_retries": 3,
        "backoff_base_seconds": 900,
        "backoff_max_seconds": 86400,
        "rows": [],
    }
    result.update(updates)
    return result


def row(**updates):
    result = {
        "repo": "example/atlas", "kind": 0, "number": 42,
        "revision": "sha123", "status": 2,
        "has_timestamp": True, "timestamp": 1773001000,
        "failure_was_limit": False,
    }
    result.update(updates)
    return result


def verify(host: Path, bundle: Path, case: dict, expected: dict):
    output = run(host, bundle, json.dumps(case, separators=(",", ":")))
    assert output.returncode == 0, (case, output.returncode, output.stderr)
    actual = json.loads(output.stdout)
    assert set(actual) == {
        "eligibility", "failure_count", "next_retry_utc_seconds",
    }, actual
    assert actual == expected, (case, actual, expected)
    return actual


def native_fixtures(host: Path, bundle: Path):
    now = baseline()["now"]
    failure = row()
    second = row(timestamp=1773001200)
    third = row(timestamp=1773001300)
    fixtures = [
        (baseline(), "ready", 0, None),
        (baseline(rows=[row(status=0)]), "already_terminal", 0, None),
        (baseline(rows=[row(status=1)]), "already_terminal", 0, None),
        (baseline(rows=[failure]), "backoff", 1, 1773001900),
        (baseline(now=1773001900, rows=[failure]), "ready", 1, 1773001900),
        (baseline(rows=[failure, second]), "backoff", 2, 1773003000),
        (baseline(rows=[failure, second, third]), "retries_exhausted", 3, None),
        (baseline(rows=[failure, second, third, row(status=0)]),
         "already_terminal", 3, None),
        (baseline(rows=[row(failure_was_limit=True)]), "ready", 0, None),
        (baseline(rows=[row(repo="", status=0)]), "already_terminal", 0, None),
        (baseline(repo="elsewhere/atlas", rows=[row(repo="", status=0)]),
         "ready", 0, None),
        (baseline(rows=[row(revision="other", status=0)]), "ready", 0, None),
        (baseline(rows=[row(status=3)]), "ready", 0, None),
        (baseline(rows=[row(has_timestamp=False)]), "ready", 1, None),
        (baseline(rows=[row(kind=1,status=0)]), "ready", 0, None),
    ]
    for case, eligibility, count, retry in fixtures:
        verify(host, bundle, case, {
            "eligibility": eligibility,
            "failure_count": count,
            "next_retry_utc_seconds": retry,
        })
    print(f"PASS {len(fixtures)} real-Celix ledger known-answer cases")


def security_regression(host: Path, bundle: Path):
    env = {
        "PATH": "/usr/bin:/bin",
        "WATCHCORE_CELIX_BUNDLE": str(bundle),
        "WATCHCORE_CELIX_BUNDLE_SHA256": hashlib.sha256(bundle.read_bytes()).hexdigest(),
    }
    args = [str(host), "security", "65", "2", "2", "0", "10", "0",
            "0", "0", "1", "2", "1"]
    p = subprocess.run(args, env=env, capture_output=True, text=True, timeout=5)
    assert p.returncode == 0, p.stderr
    assert json.loads(p.stdout) == {
        "score": 65, "risk": "high", "decision": "block",
        "block_downgraded_to_warn": False, "fallback_upgraded_to_warn": False,
    }
    print("PASS existing security Celix command unchanged")


def invalid_fixtures(host: Path, bundle: Path, wrong: Path):
    valid=baseline()
    bad = [
        "{", "null", "[]", "", "{\"rows\":[]}",
        json.dumps(valid).replace('"kind": 0', '"kind": 6'),
        json.dumps(valid).replace('"kind": 0', '"kind": "pr"'),
        json.dumps(valid).replace('"repo": "example/atlas"', '"repo": "example/atlas\\u0000"'),
        json.dumps(valid).replace('"max_retries": 3', '"max_retries": -1'),
        json.dumps(valid).replace('"rows": []', '"rows": [null]'),
        json.dumps(valid).replace('"rows": []',
                                   '"rows": [' + json.dumps(row(status=9)) + ']'),
        json.dumps(valid).replace('"rows": []',
                                   '"rows": [' + json.dumps(row(failure_was_limit=9)) + ']'),
        json.dumps(valid).replace('"rows": []',
                                   '"rows": [' + json.dumps(row()) + ']' ),
        json.dumps(valid).replace('"rows": []', '"rows": [] ,"rows": []'),
        json.dumps(valid).replace('"number": 42', '"number": 0'),
        json.dumps(valid).replace('"revision": "sha123"', '"revision": ""'),
        " " * 65537,
    ]
    # Distinguish a valid row from a malformed boolean, and retain the
    # valid case for a sanity probe before counting invalid inputs.
    valid_with_one = bad.pop(12)
    assert run(host, bundle, valid_with_one).returncode == 0
    for index, sample in enumerate(bad):
        p=run(host,bundle,sample)
        assert p.returncode != 0 and not p.stdout.strip(), (
            index, p.returncode, p.stdout, p.stderr,
        )
    missing = subprocess.run([str(host), "ledger"], input=json.dumps(valid),
                             text=True, capture_output=True, timeout=5,
                             env={"PATH":"/usr/bin:/bin"})
    assert missing.returncode != 0 and not missing.stdout.strip()
    wrong_pin=run(host,bundle,json.dumps(valid),pinned=False)
    assert wrong_pin.returncode != 0 and not wrong_pin.stdout.strip()
    # Valid signed bundle which does NOT contain the ledger Celix service.
    wrong_service=run(host,wrong,json.dumps(valid))
    assert wrong_service.returncode != 0 and not wrong_service.stdout.strip()
    print(f"PASS {len(bad) + 3} strict-schema/pin/service failure cases")


def python_parity(host: Path, bundle: Path, rizzer: Path):
    source = rizzer / "integrations/atlas-watcher/src"
    assert (source / "atlas_watcher/ledger.py").is_file(), source
    sys.path.insert(0, str(source))
    paths = types.ModuleType("atlas_watcher.paths")
    config = types.ModuleType("atlas_watcher.config")
    config.settings=types.SimpleNamespace()
    cooldown = types.ModuleType("atlas_watcher.cooldown")
    cooldown.classify_limit = lambda *_a, **_k: False
    paths.pr_dir = lambda *_a: Path("/unused")
    paths.issue_dir = lambda *_a: Path("/unused")
    paths.ledger_file = lambda: Path("/unused")
    sys.modules["atlas_watcher.paths"] = paths
    sys.modules["atlas_watcher.config"] = config
    sys.modules["atlas_watcher.cooldown"] = cooldown
    from atlas_watcher import ledger

    rng=random.Random(0xA71A5)
    with tempfile.TemporaryDirectory(prefix="watchcore-ledger-") as td:
        transcript=Path(td) / "ledger.jsonl"
        paths.ledger_file=lambda: transcript
        ledger.ledger_file=lambda: transcript
        # Source's transcript classifier is an explicit, owner-derived
        # normalized boolean; don't inspect arbitrary filesystem or prompts.
        with patch.object(ledger, "_failure_was_limit",
                          lambda r: bool(r.get("fixture_limit", False))):
            for index in range(260):
                now=1773001500 + rng.randint(-1800,1800)
                case=baseline(
                    repo=rng.choice(["example/atlas","elsewhere/atlas"]),
                    kind=rng.randrange(2),
                    number=rng.choice([1,42,500]),
                    revision=rng.choice(["sha123","comments:7","abc"]),
                    now=now,
                    max_retries=rng.randrange(1,6),
                    backoff_base_seconds=rng.choice([0,30,900,3600]),
                    backoff_max_seconds=rng.choice([0,120,3600,86400]),
                )
                case["default_repo"]=rng.choice(["example/atlas","elsewhere/atlas",""])
                case["rows"]=[]
                raw_rows=[]
                for _ in range(rng.randrange(16)):
                    r=row(
                        repo=rng.choice(["","example/atlas","elsewhere/atlas"]),
                        kind=rng.choice([case["kind"],rng.randrange(2)]),
                        number=rng.choice([case["number"],500,999]),
                        revision=rng.choice([case["revision"],"other"]),
                        status=rng.randrange(5),
                        has_timestamp=bool(rng.randrange(2)),
                        timestamp=now+rng.randint(-1500,1500),
                        failure_was_limit=bool(rng.randrange(4)==0),
                    )
                    case["rows"].append(r)
                    rec = {
                        "kind": ("pr","issue")[r["kind"]],
                        "number": r["number"],
                        "key": r["revision"],
                        "status": STATUS[r["status"]],
                        "fixture_limit": r["failure_was_limit"],
                    }
                    if r["repo"]:
                        rec["repo"]=r["repo"]
                    if r["has_timestamp"]:
                        rec["ts"]=datetime.fromtimestamp(
                            r["timestamp"], UTC
                        ).strftime("%Y-%m-%dT%H:%M:%SZ")
                    raw_rows.append(rec)
                transcript.write_text(
                    "".join(json.dumps(x)+"\n" for x in raw_rows),encoding="utf-8"
                )
                ledger.settings=types.SimpleNamespace(
                    repos=([case["default_repo"]] if case["default_repo"] else []),
                    max_retries=case["max_retries"],
                    retry_backoff_base_secs=case["backoff_base_seconds"],
                    retry_backoff_max_secs=case["backoff_max_seconds"],
                )
                k=(case["repo"], ("pr","issue")[case["kind"]],
                   case["number"], case["revision"])
                count=ledger.failure_count(*k)
                if ledger.has_done(*k):
                    eligibility="already_terminal"
                    retry=None
                elif ledger.retries_exhausted(*k):
                    eligibility="retries_exhausted"
                    retry=None
                else:
                    next_dt=ledger.next_retry_at(*k)
                    retry=int(next_dt.timestamp()) if next_dt else None
                    eligibility="backoff" if retry is not None and now<retry else "ready"
                expected={
                    "eligibility":eligibility,
                    "failure_count":count,
                    "next_retry_utc_seconds":retry,
                }
                verify(host,bundle,case,expected)
    print("PASS 260 Celix ledger decisions match actual Atlas Watcher Python ledger functions")


def main():
    if len(sys.argv) not in (4,5):
        raise SystemExit(
            "usage: test.py ABS_HOST ABS_BUNDLE ABS_WRONG_SERVICE_BUNDLE [ABS_RIZZER_ROOT]"
        )
    host,bundle,wrong=(Path(x).resolve() for x in sys.argv[1:4])
    for p in (host,bundle,wrong): assert p.is_file(), p
    native_fixtures(host,bundle)
    security_regression(host,bundle)
    invalid_fixtures(host,bundle,wrong)
    if len(sys.argv)==5:
        python_parity(host,bundle,Path(sys.argv[4]).resolve())
    return 0


if __name__=="__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Offline parity of *actual* Python Atlas Watcher assess() against Celix.

Only the provider and heuristic acquisition inputs are synthetic. The Python
security decision logic and observation adapter execute from a pinned Rizzer
checkout supplied on the command line. No provider, network or posting calls.
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
import types
from types import SimpleNamespace
from unittest.mock import patch


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: test.py ABS_RIZZER_ROOT ABS_CELIX_HOST ABS_BUNDLE")
    root, host, bundle = [Path(p).resolve() for p in sys.argv[1:]]
    source = root / "integrations/atlas-watcher/src"
    assert (source / "atlas_watcher/security.py").is_file()
    assert (source / "atlas_watcher/watchcore_shadow.py").is_file()
    for p in (host, bundle):
        assert p.is_file(), p
    # Stub external imports only. Actual security._decide, _corroborated,
    # _max_risk, _score_to_risk and assess are not replaced.
    sys.path.insert(0, str(source))
    for name in ("agents", "swarm"):
        sys.modules[f"atlas_watcher.{name}"] = types.ModuleType(name)
    config = types.ModuleType("atlas_watcher.config")
    config.settings = SimpleNamespace()
    sys.modules["atlas_watcher.config"] = config
    logging = types.ModuleType("atlas_watcher.log")
    logging.log = lambda _: None
    sys.modules["atlas_watcher.log"] = logging
    paths = types.ModuleType("atlas_watcher.paths")
    paths.PROMPTS = Path("/invalid-unused-prompt-path")
    sys.modules["atlas_watcher.paths"] = paths

    from atlas_watcher import security

    digest = hashlib.sha256(host.read_bytes()).hexdigest()
    pin = hashlib.sha256(bundle.read_bytes()).hexdigest()
    env = {
        "WATCHCORE_SHADOW_BIN": str(host),
        "WATCHCORE_SHADOW_SHA256": digest,
        "WATCHCORE_CELIX_BUNDLE": str(bundle),
        "WATCHCORE_CELIX_BUNDLE_SHA256": pin,
    }
    risks = ("low", "medium", "high", "critical")
    rng = random.Random(0xC311A)
    with tempfile.TemporaryDirectory(prefix="atlas-python-celix-") as td:
        # Use actual source assessment over varying normalized input pairs.
        # No model or heuristic acquisition: their outputs are fixtures.
        with patch.dict(os.environ, env, clear=True):
            with patch.object(security, "_write_human_readable", lambda *_: None):
                for i in range(240):
                    count = rng.randrange(5)
                    heur = {
                        "score": rng.randint(0, 105),
                        "risk": rng.choice(risks),
                        "signals": ["synthetic"] * count,
                        "decisive": bool(rng.randrange(2)),
                    }
                    model = {
                        "score": rng.randint(0, 105),
                        "risk": rng.choice(risks),
                        "agent_fallback": bool(rng.randrange(2)),
                        "agent_rc": rng.choice([-2, 0, 0, 1]),
                        "agent": "synthetic-provider",
                    }
                    thresholds = SimpleNamespace(
                        security_exempt=lambda _: False,
                        security_warn_above=rng.choice(risks),
                        security_block_above=rng.choice(risks),
                        security_require_corroboration=bool(rng.randrange(2)),
                    )
                    folder = Path(td) / str(i)
                    with patch.object(security, "settings", thresholds), \
                         patch.object(security, "_heuristic_verdict", lambda _t: heur), \
                         patch.object(security, "_run_model_verdict", lambda _t, _p: model):
                        verdict = security.assess("test/synthetic", "pr", i, "fixture", folder)
                    observed = json.loads((folder / "watchcore-shadow.json").read_text())
                    assert observed["status"] == "match", (i, heur, model, verdict, observed)
                    assert observed["native"]["decision"] == verdict["decision"]
                # Force unavailable observation but preserve authoritative Python result.
                bad_env = dict(env, WATCHCORE_CELIX_BUNDLE_SHA256="0" * 64)
                with patch.dict(os.environ, bad_env, clear=True):
                    heur = {"score": 65, "risk": "high", "signals": ["one", "two"],
                            "decisive": False}
                    model = {"score": 10, "risk": "low", "agent_fallback": False,
                             "agent_rc": 0}
                    thresholds = SimpleNamespace(
                        security_exempt=lambda _: False,
                        security_warn_above="medium", security_block_above="high",
                        security_require_corroboration=True,
                    )
                    folder = Path(td) / "fail-closed"
                    with patch.object(security, "settings", thresholds), \
                         patch.object(security, "_heuristic_verdict", lambda _t: heur), \
                         patch.object(security, "_run_model_verdict", lambda _t, _p: model):
                        verdict = security.assess("test/synthetic", "pr", 999, "fixture", folder)
                    observed = json.loads((folder / "watchcore-shadow.json").read_text())
                    assert observed["status"] == "unavailable", observed
                    assert verdict["decision"] == "block", verdict
    head = subprocess.run(["git", "-C", str(root), "rev-parse", "--short=12", "HEAD"],
                          capture_output=True, text=True, check=False).stdout.strip()
    print(f"PASS: 240 actual Python assess() decisions match real pinned Celix host (Rizzer {head})")
    print("PASS: Celix failure did not alter Python authoritative block decision")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
# Copyright 2026 Robert McConnell
# SPDX-License-Identifier: Apache-2.0
"""One-shot, operator-pinned Lightpanda Agent Bridge record/replay driver.

No agent-supplied URL, selector, script, executable, or receipt ID is accepted.
The Node service supplies a verified loopback fixture and an internal source ID.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from urllib.parse import urlsplit

def main() -> int:
    if len(sys.argv) != 5:
        raise ValueError("usage: runner.py MODE BRIDGE_DIR FIXTURE_URL SOURCE_ID")
    mode, bridge_dir, url, source_id = sys.argv[1:]
    if mode not in ("record", "replay"):
        raise ValueError("unrecognized operation")
    p = urlsplit(url)
    if p.scheme != "http" or p.hostname != "127.0.0.1" or not p.port or p.path != "/" or p.query or p.fragment:
        raise ValueError("fixture must be loopback")
    if mode == "replay" and not re.fullmatch(r"[0-9a-f]{32}", source_id):
        raise ValueError("invalid replay source")
    if mode == "record" and source_id != "-":
        raise ValueError("unexpected replay source")
    source = Path(bridge_dir).resolve(strict=True)
    if not source.is_dir():
        raise ValueError("missing vetted agent bridge")
    sys.path.insert(0, str(source))
    from lightpanda_evidence import record, replay  # trusted, operator-pinned checkout
    if mode == "record":
        base = f"http://127.0.0.1:{p.port}"
        actions = [
            {"type": "extract", "selector": "#heading"},
            {"type": "navigate", "url": base + "/second"},
            {"type": "extract", "selector": "#heading"},
        ]
        receipt = record(url, actions)
    else:
        receipt = replay(source_id)
    # Deliberately never print observations or page content.
    print(json.dumps({
        "outcome": receipt.get("outcome"),
        "receipt_id": receipt.get("receipt_id"),
        "parent_receipt_id": receipt.get("parent_receipt_id"),
        "kind": receipt.get("kind"),
        "step_count": len(receipt.get("steps", [])),
        "workflow_sha256": receipt.get("workflow_sha256"),
    }, separators=(",", ":")), flush=True)
    return 0

if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        # Restrict error output to class only to avoid leaking runtime values.
        print(json.dumps({"outcome": "BLOCKED", "error_type": type(exc).__name__}), flush=True)
        raise SystemExit(2)

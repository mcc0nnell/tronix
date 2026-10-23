#!/usr/bin/env python3
# Copyright 2026 Robert McConnell
# SPDX-License-Identifier: Apache-2.0
"""Publish an operator-signed TRONIX checkpoint to the Cloudflare/D1 Worker.

Run on the independent operator verification host, NOT the TRONIX daemon.
Tokens are read from environment variables; never supply them as CLI values.
"""
import argparse
import hashlib
import json
import os
import sys
from urllib import request, error, parse

from grant_checkpoint import (read_anchor_key, read_checkpoint_file,
                              verify_checkpoint)


class NoRedirect(request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise ValueError("checkpoint HTTP redirects refused")


def fail(message):
    raise ValueError(message)


def fetch(url, method, body, token):
    headers = {"Authorization": "Bearer " + token, "Accept": "application/json"}
    if method == "POST":
        headers["Content-Type"] = "application/json"
    req = request.Request(url, data=body, headers=headers, method=method)
    opener = request.build_opener(NoRedirect)
    try:
        with opener.open(req, timeout=15) as resp:
            raw = resp.read(4096)
            if len(raw) >= 4096:
                fail("oversized Worker response")
            return resp.status, raw
    except error.HTTPError as exc:
        # Errors contain no secrets; never print response or Authorization.
        fail("Cloudflare checkpoint request rejected (HTTP " + str(exc.code) + ")")
    except error.URLError as exc:
        fail("Cloudflare checkpoint transport unavailable")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--url", required=True, help="https://your-worker.workers.dev")
    p.add_argument("--checkpoint", required=True)
    p.add_argument("--checkpoint-key", required=True)
    args = p.parse_args()
    base = args.url.rstrip("/")
    parsed = parse.urlsplit(base)
    if (parsed.scheme != "https" or not parsed.netloc or parsed.username or
        parsed.password or parsed.query or parsed.fragment or
        parsed.path not in ("", "/")):
        fail("base URL must be a bare HTTPS Worker origin without credentials")
    publish_token = os.environ.get("TRONIX_CHECKPOINT_PUBLISH_TOKEN", "")
    read_token = os.environ.get("TRONIX_CHECKPOINT_READ_TOKEN", "")
    if (len(publish_token) < 32 or len(read_token) < 32 or
        publish_token == read_token):
        fail("distinct operator publish and read tokens must be in environment")
    key = read_anchor_key(args.checkpoint_key)
    checkpoint_raw = read_checkpoint_file(args.checkpoint)
    checkpoint = verify_checkpoint(checkpoint_raw, key)
    if checkpoint["sequence"] > 9007199254740991:
        fail("sequence exceeds Worker exact integer range")

    endpoint = base + "/v1/checkpoints"
    status, reply = fetch(endpoint, "POST", checkpoint_raw, publish_token)
    if status not in (200, 201):
        fail("unexpected publication status")
    acknowledged = json.loads(reply)
    if (acknowledged.get("sequence") != checkpoint["sequence"] or
        acknowledged.get("sha256") != hashlib.sha256(checkpoint_raw).hexdigest() or
        acknowledged.get("state") not in ("published", "already-published")):
        fail("publication acknowledgment does not match signed checkpoint")

    lookup = (endpoint + "/" + checkpoint["artifactSha256"] + "/" +
              checkpoint["grantSha256"] + "?sequence=" +
              str(checkpoint["sequence"]))
    status, stored = fetch(lookup, "GET", None, read_token)
    if status != 200 or stored != checkpoint_raw:
        fail("independent read-back differs from exact signed checkpoint")
    verify_checkpoint(stored, key)

    latest_url = (endpoint + "/" + checkpoint["artifactSha256"] + "/" +
                  checkpoint["grantSha256"])
    status, latest_raw = fetch(latest_url, "GET", None, read_token)
    latest = verify_checkpoint(latest_raw, key)
    if status != 200 or latest["sequence"] < checkpoint["sequence"]:
        fail("latest remote checkpoint sequence regressed")
    print(json.dumps({
        "schema": "tronix.cloudflare-publish.v1",
        "state": "published-and-read-back-verified",
        "checkpointSequence": checkpoint["sequence"],
        "remoteLatestSequence": latest["sequence"],
        "checkpointSha256": hashlib.sha256(checkpoint_raw).hexdigest(),
        "limitations": "Cloudflare account administrators can modify D1; "
                       "independent immutable retention is not yet proven"
    }, indent=2))


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print("TRONIX CHECKPOINT PUBLISH REFUSED: " + str(exc), file=sys.stderr)
        sys.exit(1)

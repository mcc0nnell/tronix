#!/usr/bin/env python3
"""Read-only, synthetic-only IHB profile preflight broker (no Docker access)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import stat
import struct

OPS = {
    "fuzz.preflight.safe": "fixtures/safe.c",
    "fuzz.preflight.buggy": "fixtures/buggy.c",
}
FILES = frozenset({"run.py", "engine.cc", "isolated.py", *OPS.values()})


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class Preflight:
    def __init__(self, root, catalog, catalog_sha, sockpath, allowed_uid):
        if Path(root).is_symlink() or Path(catalog).is_symlink():
            raise ValueError("source root and catalog may not be symlinks")
        self.root = Path(root).resolve(strict=True)
        self.catalog = Path(catalog).resolve(strict=True)
        self.sockpath = Path(sockpath).absolute()
        self.allowed_uid = allowed_uid
        if sha(self.catalog) != catalog_sha:
            raise ValueError("catalog digest mismatch")
        data = json.loads(self.catalog.read_text())
        if set(data) != {"schema", "files", "image_id"} or data["schema"] != "tronix.ihb-preflight.v0":
            raise ValueError("invalid catalog schema")
        if set(data["files"]) != FILES or not re.fullmatch(r"sha256:[a-f0-9]{64}", data["image_id"]):
            raise ValueError("unapproved catalog contents")
        if any(not re.fullmatch(r"[a-f0-9]{64}", x) for x in data["files"].values()):
            raise ValueError("invalid file digests")
        self.files = data["files"]
        parent = os.lstat(self.sockpath.parent)
        if (not stat.S_ISDIR(parent.st_mode) or parent.st_uid != os.getuid()
                or parent.st_mode & 0o077):
            raise ValueError("socket parent must be private and owned by broker")
        if self.sockpath.exists() or self.sockpath.is_symlink():
            raise ValueError("refusing to replace existing socket")
        if len(os.fsencode(self.sockpath)) > 103:
            raise ValueError("socket path exceeds Unix limit")
        self.verify()

    def verify(self):
        for rel, expected in self.files.items():
            path = self.root / rel
            if path.is_symlink() or not path.is_file() or sha(path) != expected:
                raise ValueError("approved source digest changed: " + rel)

    def handle(self, conn):
        conn.settimeout(2)
        cred = conn.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
        _pid, uid, _gid = struct.unpack("3i", cred)
        if uid != self.allowed_uid:
            return b"ERR\n"
        # The C adapter sends operation and delimiter separately. Handle
        # fragmented streams, while rejecting oversized or trailing data.
        message = bytearray()
        while len(message) < 65 and b"\n" not in message:
            chunk = conn.recv(65 - len(message))
            if not chunk:
                return b"ERR\n"
            message.extend(chunk)
        if message.count(b"\n") != 1 or not message.endswith(b"\n"):
            return b"ERR\n"
        try:
            operation = message[:-1].decode("ascii")
        except UnicodeDecodeError:
            return b"ERR\n"
        if operation not in OPS:
            return b"ERR\n"
        try:
            self.verify()
        except (OSError, ValueError):
            return b"ERR\n"
        return b"OK 1\n"

    def serve(self, max_requests=0):
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        inode = None
        try:
            server.bind(str(self.sockpath))
            os.chmod(self.sockpath, 0o600)
            inode = os.stat(self.sockpath).st_ino
            server.listen(4)
            print("IHB_PREFLIGHT_READY", flush=True)
            calls = 0
            while max_requests == 0 or calls < max_requests:
                conn, _ = server.accept()
                calls += 1
                with conn:
                    try:
                        conn.sendall(self.handle(conn))
                    except (OSError, ValueError):
                        try:
                            conn.sendall(b"ERR\n")
                        except OSError:
                            pass
        finally:
            server.close()
            if inode is not None:
                try:
                    if os.stat(self.sockpath).st_ino == inode:
                        self.sockpath.unlink()
                except FileNotFoundError:
                    pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--catalog-sha256", required=True)
    parser.add_argument("--socket", type=Path, required=True)
    parser.add_argument("--allow-uid", type=int, required=True)
    parser.add_argument("--max-requests", type=int, default=0)
    args = parser.parse_args()
    if not re.fullmatch(r"[a-f0-9]{64}", args.catalog_sha256) or args.max_requests < 0:
        parser.error("invalid catalog hash or request limit")
    try:
        Preflight(args.root, args.catalog, args.catalog_sha256, args.socket,
                  args.allow_uid).serve(args.max_requests)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print("IHB preflight refused:", exc)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

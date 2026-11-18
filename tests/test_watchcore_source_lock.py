# SPDX-License-Identifier: Apache-2.0
"""Self-contained, offline source-lock regression tests."""
from __future__ import annotations
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from verify_watchcore_source import PINNED_FILES, ROOT as SOURCE_ROOT, verify


def copy_fixture(base: Path) -> Path:
    root = base / "repo"
    watchcore = root / "integrations" / "atlas-watchcore"
    watchcore.mkdir(parents=True)
    for name in PINNED_FILES:
        src = SOURCE_ROOT / name if name in ("LICENSE", "NOTICE") else SOURCE_ROOT / "integrations" / "atlas-watchcore" / name
        dst = root / name if name in ("LICENSE", "NOTICE") else watchcore / name
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
    shutil.copy2(SOURCE_ROOT / "integrations" / "atlas-watchcore" / "NEONOXIDE_SOURCE_LOCK.json",
                 watchcore / "NEONOXIDE_SOURCE_LOCK.json")
    return root


class SourceLockTests(unittest.TestCase):
    def test_real_checkout(self):
        self.assertEqual(verify()["version"], "0.1.0")

    def test_source_tamper(self):
        with tempfile.TemporaryDirectory() as temp:
            repo = copy_fixture(Path(temp))
            p = repo / "integrations" / "atlas-watchcore" / "src" / "watchcore.cpp"
            p.write_bytes(p.read_bytes() + b"// tampered\n")
            with self.assertRaisesRegex(ValueError, "snapshot drift"):
                verify(repo)

    def test_license_tamper(self):
        with tempfile.TemporaryDirectory() as temp:
            repo = copy_fixture(Path(temp))
            (repo / "NOTICE").write_text("changed\n")
            with self.assertRaisesRegex(ValueError, "snapshot drift"):
                verify(repo)

    def test_missing_file(self):
        with tempfile.TemporaryDirectory() as temp:
            repo = copy_fixture(Path(temp))
            (repo / "integrations" / "atlas-watchcore" / "src" / "watchcore.cpp").unlink()
            with self.assertRaisesRegex(ValueError, "missing snapshot"):
                verify(repo)

    def test_mutable_commit_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            repo = copy_fixture(Path(temp))
            lock = repo / "integrations" / "atlas-watchcore" / "NEONOXIDE_SOURCE_LOCK.json"
            data = json.loads(lock.read_text())
            data["canonical_source_commit"] = "main"
            lock.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, "canonical_source_commit"):
                verify(repo)

    def test_origin_manifest_hash_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            repo = copy_fixture(Path(temp))
            lock = repo / "integrations" / "atlas-watchcore" / "NEONOXIDE_SOURCE_LOCK.json"
            data = json.loads(lock.read_text())
            data["source_manifest_sha256"] = "f" * 64
            lock.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, "source manifest digest"):
                verify(repo)

    def test_extra_path_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            repo = copy_fixture(Path(temp))
            lock = repo / "integrations" / "atlas-watchcore" / "NEONOXIDE_SOURCE_LOCK.json"
            data = json.loads(lock.read_text())
            data["files_sha256"]["../override"] = "0" * 64
            lock.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, "file set"):
                verify(repo)

    def test_duplicate_json_key_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            repo = copy_fixture(Path(temp))
            lock = repo / "integrations" / "atlas-watchcore" / "NEONOXIDE_SOURCE_LOCK.json"
            lock.write_text(lock.read_text().replace('"version": "0.1.0"', '"version": "0.1.0", "version": "0.1.0"'))
            with self.assertRaisesRegex(ValueError, "duplicate field"):
                verify(repo)

    def test_symlink_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            repo = copy_fixture(Path(temp))
            path = repo / "integrations" / "atlas-watchcore" / "src" / "watchcore.cpp"
            elsewhere = repo / "other.cpp"
            shutil.copy2(path, elsewhere)
            path.unlink()
            path.symlink_to(elsewhere)
            with self.assertRaisesRegex(ValueError, "symlink forbidden"):
                verify(repo)


if __name__ == "__main__":
    unittest.main()

"""Pure unit checks for the read-only IHB synthetic-profile gate."""
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ihb_preflight import Preflight, FILES


class PreflightTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.root = self.base / "approved"
        (self.root / "fixtures").mkdir(parents=True)
        self.private = self.base / "private"
        self.private.mkdir(mode=0o700)
        self.catalog = self.base / "catalog.json"
        self.values = {}
        for name in sorted(FILES):
            path = self.root / name
            path.write_text("synthetic approved " + name)
            self.values[name] = hashlib.sha256(path.read_bytes()).hexdigest()
        self.write_catalog()

    def write_catalog(self, files=None, image=None):
        data = {"schema": "tronix.ihb-preflight.v0",
                "files": self.values if files is None else files,
                "image_id": image or "sha256:" + "c" * 64}
        self.catalog.write_text(json.dumps(data))
        self.catalog_sha = hashlib.sha256(self.catalog.read_bytes()).hexdigest()

    def open_gate(self, sha=None):
        return Preflight(self.root, self.catalog,
                         self.catalog_sha if sha is None else sha,
                         self.private / "ihb.sock", os.getuid())

    def test_approved_catalog_and_files(self):
        self.open_gate().verify()

    def test_wrong_catalog_digest_is_denied(self):
        with self.assertRaisesRegex(ValueError, "catalog digest"):
            self.open_gate("a" * 64)

    def test_source_mutation_fails_closed(self):
        gate = self.open_gate()
        (self.root / "fixtures/buggy.c").write_text("changed")
        with self.assertRaisesRegex(ValueError, "source digest changed"):
            gate.verify()

    def test_source_symlink_fails_closed(self):
        fixture = self.root / "fixtures/safe.c"
        fixture.unlink()
        fixture.symlink_to(self.root / "fixtures/buggy.c")
        with self.assertRaisesRegex(ValueError, "source digest changed"):
            self.open_gate()

    def test_catalog_symlink_fails_closed(self):
        link = self.base / "catalog-link.json"
        link.symlink_to(self.catalog)
        with self.assertRaisesRegex(ValueError, "symlinks"):
            Preflight(self.root, link, self.catalog_sha,
                      self.private / "ihb.sock", os.getuid())

    def test_unapproved_catalog_file_denied(self):
        files = dict(self.values)
        files["extra.c"] = "e" * 64
        self.write_catalog(files=files)
        with self.assertRaisesRegex(ValueError, "catalog contents"):
            self.open_gate()

    def test_mutable_image_denied(self):
        self.write_catalog(image="ubuntu:latest")
        with self.assertRaisesRegex(ValueError, "catalog contents"):
            self.open_gate()

    def test_public_socket_parent_denied(self):
        os.chmod(self.private, 0o755)
        with self.assertRaisesRegex(ValueError, "private"):
            self.open_gate()

    def test_existing_socket_not_replaced(self):
        (self.private / "ihb.sock").write_text("sentinel")
        with self.assertRaisesRegex(ValueError, "existing socket"):
            self.open_gate()
        self.assertEqual((self.private / "ihb.sock").read_text(), "sentinel")


if __name__ == "__main__":
    unittest.main()

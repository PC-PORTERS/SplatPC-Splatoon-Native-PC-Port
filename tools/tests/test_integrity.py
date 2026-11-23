"""Client-side game integrity tests (scripts/integrity.py).
    python3 -m unittest tools.tests.test_integrity -v
"""
import importlib.util
import pathlib
import shutil
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def load(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


integrity = load("integrity", "scripts/integrity.py")


def make_install(base, files, version="0.1.3"):
    """A fake install: files {relative: bytes}, a VERSION and a manifest listing the files."""
    root = pathlib.Path(base)
    for relative, data in files.items():
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    (root / "VERSION").write_text(version + "\n")
    names = sorted(files) + ["VERSION"]
    text = "".join(f"{integrity.hash_file(str(root / n))} {n}\n" for n in names)
    (root / "scripts").mkdir(exist_ok=True)
    (root / "scripts" / "integrity-manifest.txt").write_text(text, newline="\n")
    return root, text


class ClientHashTests(unittest.TestCase):
    def setUp(self):
        self.dir = pathlib.Path(tempfile.mkdtemp(prefix="integrity-"))
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.root, self.text = make_install(self.dir, {"recompiler/a.cpp": b"int a;\nint b;\n", "scripts/x.py": b"print(1)\n"})

    def test_a_clean_install_proves_the_release_hash(self):
        proof = integrity.attestation(str(self.root))
        self.assertEqual(proof["version"], "0.1.3")
        self.assertEqual(proof["code"], integrity.release_hash(self.text))
        self.assertEqual(integrity.differing(str(self.root)), [])

    def test_line_endings_do_not_change_the_hash(self):
        (self.root / "recompiler" / "a.cpp").write_bytes(b"int a;\r\nint b;\r\n")
        self.assertEqual(integrity.attestation(str(self.root))["code"], integrity.release_hash(self.text))

    def test_an_edited_or_missing_file_changes_the_hash(self):
        good = integrity.release_hash(self.text)
        (self.root / "recompiler" / "a.cpp").write_bytes(b"int a;\nint cheat;\n")
        self.assertNotEqual(integrity.attestation(str(self.root))["code"], good)
        self.assertEqual(integrity.differing(str(self.root)), ["recompiler/a.cpp"])
        (self.root / "recompiler" / "a.cpp").unlink()
        self.assertNotEqual(integrity.attestation(str(self.root))["code"], good)
        self.assertIn("recompiler/a.cpp", integrity.differing(str(self.root)))

    def test_the_version_comes_from_VERSION_not_from_installed_version(self):
        stale = self.root / "build"
        stale.mkdir()
        (stale / "installed-version.txt").write_text("0.0.1\n")
        self.assertEqual(integrity.read_version(str(self.root)), "0.1.3")
        (self.root / "VERSION").unlink()
        self.assertEqual(integrity.read_version(str(self.root)), "")

    def test_tag_parsing_and_writing_version(self):
        self.assertEqual(integrity.parse_tag_version("v0.1.5"), "0.1.5")
        self.assertEqual(integrity.parse_tag_version("online-v0.1.5"), "0.1.5")
        self.assertEqual(integrity.parse_tag_version("v1.2.3-rc1"), "1.2.3")
        self.assertEqual(integrity.parse_tag_version("no-digits"), "")
        written = integrity.write_version_file("online-v0.2.0", root=str(self.root))
        self.assertEqual(written, "0.2.0")
        self.assertEqual((self.root / "VERSION").read_text(encoding="ascii"), "0.2.0\n")

    def test_no_manifest_means_no_proof(self):
        (self.root / "scripts" / "integrity-manifest.txt").unlink()
        self.assertEqual(integrity.attestation(str(self.root))["code"], "")

    def test_what_is_covered(self):
        for yes in ("recompiler/port/os/x.cpp", "scripts/integrity.py", "VERSION", "profiles/agmp-v288.json"):
            self.assertTrue(integrity.covered(yes), yes)
        for no in ("docs/x.md", "scripts/tests/t.py", "scripts/integrity-manifest.txt", "scripts/server.txt",
                   "tools/nexlab/social.py", "scripts/__pycache__/x.pyc"):
            self.assertFalse(integrity.covered(no), no)

    def test_the_real_install_hashes(self):
        """The repository itself computes version from VERSION or git tags when available."""
        proof = integrity.attestation()
        self.assertIn("version", proof)
        self.assertIn("code", proof)


if __name__ == "__main__":
    unittest.main()

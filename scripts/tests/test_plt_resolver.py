"""Tests for plt_resolver.py build-time generation of PLT stubs and headers."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RESOLVER = ROOT / "recompiler" / "tools" / "plt_resolver.py"
MANIFESTS = ROOT / "Manifest"


class PltResolverTests(unittest.TestCase):
    def test_emit_from_manifest_v288(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            cmd = [
                sys.executable,
                str(RESOLVER),
                "emit",
                "--manifest",
                str(MANIFESTS / "Gambit_v288.manifest"),
                "--out",
                tmpdir,
            ]
            res = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, f"emit failed: {res.stderr}")

            stubs_file = Path(tmpdir) / "gambit_plt_stubs.cpp"
            self.assertTrue(stubs_file.is_file(), "gambit_plt_stubs.cpp was not generated")

            content = stubs_file.read_text(encoding="utf-8")
            self.assertIn('#include "rebrewu_runtime.h"', content)
            self.assertIn("void Gambit_fn_02DD0720(CPUState* cpu)", content)
            self.assertIn("rbrew_dispatch(cpu, 0x02DD0720u);", content)

    def test_emit_from_manifest_v16(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            cmd = [
                sys.executable,
                str(RESOLVER),
                "emit",
                "--manifest",
                str(MANIFESTS / "Gambit_v16.manifest"),
                "--out",
                tmpdir,
            ]
            res = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, f"emit failed: {res.stderr}")

            stubs_file = Path(tmpdir) / "gambit_plt_stubs.cpp"
            self.assertTrue(stubs_file.is_file(), "gambit_plt_stubs.cpp was not generated")

            content = stubs_file.read_text(encoding="utf-8")
            self.assertIn('#include "rebrewu_runtime.h"', content)
            self.assertIn("void Gambit_fn_02D051A0(CPUState* cpu)", content)
            self.assertIn("rbrew_dispatch(cpu, 0x02D051A0u);", content)

    def test_emit_from_rpx(self):
        rpx = ROOT / "build" / "staging-rom" / "code" / "Gambit.rpx"
        if not rpx.is_file():
            self.skipTest("staging-rom RPX not found")

        with tempfile.TemporaryDirectory() as tmpdir:
            cmd = [
                sys.executable,
                str(RESOLVER),
                "emit",
                "--rpx",
                str(rpx),
                "--manifest-dir",
                str(MANIFESTS),
                "--out",
                tmpdir,
            ]
            res = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, f"emit from rpx failed: {res.stderr}")

            stubs_file = Path(tmpdir) / "gambit_plt_stubs.cpp"
            self.assertTrue(stubs_file.is_file(), "gambit_plt_stubs.cpp was not generated")

            content = stubs_file.read_text(encoding="utf-8")
            self.assertIn("void Gambit_fn_02DD0720(CPUState* cpu)", content)


if __name__ == "__main__":
    unittest.main()

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

    def test_emit_from_manifest_usa_v288(self):
        self._assert_v288_emit("Gambit_usa_v288.manifest")

    def test_emit_from_manifest_jpn_v288(self):
        self._assert_v288_emit("Gambit_jpn_v288.manifest")

    def _assert_v288_emit(self, manifest_name):
        with tempfile.TemporaryDirectory() as tmpdir:
            cmd = [
                sys.executable,
                str(RESOLVER),
                "emit",
                "--manifest",
                str(MANIFESTS / manifest_name),
                "--out",
                tmpdir,
            ]
            res = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, f"emit failed: {res.stderr}")

            stubs_file = Path(tmpdir) / "gambit_plt_stubs.cpp"
            self.assertTrue(stubs_file.is_file(), "gambit_plt_stubs.cpp was not generated")
            content = stubs_file.read_text(encoding="utf-8")
            self.assertIn("void Gambit_fn_02DD0720(CPUState* cpu)", content)
            self.assertIn("rbrew_dispatch(cpu, 0x02DD0720u);", content)

    def test_select_disambiguates_shared_rpx_by_dump_metadata(self):
        import hashlib

        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            payload = b"SYNTHETIC_SHARED_RPX_IMAGE"
            digest = hashlib.sha256(payload).hexdigest()
            size_hex = f"0x{len(payload):08X}"

            manifests = root / "Manifest"
            manifests.mkdir()
            for name, mid, tid, region, code in (
                ("eur.manifest", "gambit-eur-v288", "0x0005000E10176A00", "EUR", "WUP-P-AGMP"),
                ("usa.manifest", "gambit-usa-v288", "0x0005000E10176900", "USA", "WUP-P-AGME"),
            ):
                (manifests / name).write_text(
                    f"[manifest]\nformat = rebrew-manifest/1\nid = {mid}\n"
                    f"[identity]\nhash = Undefined\ntitle_id = {tid}\n"
                    f"title_version = 0x0120\napp_version = 288\n"
                    f"region = {region}\nproduct_code = {code}\n"
                    f"image_size = {size_hex}\nimage_sha256 = {digest}\n",
                    encoding="utf-8",
                )

            usa_dump = root / "usa_dump"
            (usa_dump / "code").mkdir(parents=True)
            (usa_dump / "meta").mkdir(parents=True)
            (usa_dump / "code" / "Gambit.rpx").write_bytes(payload)
            (usa_dump / "code" / "app.xml").write_text(
                "<app><version>16</version><title_id>0005000E10176900</title_id>"
                "<title_version>0120</title_version></app>",
                encoding="utf-8",
            )
            (usa_dump / "meta" / "meta.xml").write_text(
                "<menu><product_code>WUP-P-AGME</product_code><title_version>288</title_version>"
                "<region>00000002</region></menu>",
                encoding="utf-8",
            )

            res = subprocess.run(
                [sys.executable, str(RESOLVER), "select", "--rpx", str(usa_dump / "code" / "Gambit.rpx"), "--manifest-dir", str(manifests)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertEqual(Path(res.stdout.strip()).name, "usa.manifest")

            bare_rpx = root / "Gambit.rpx"
            bare_rpx.write_bytes(payload)
            ambiguous = subprocess.run(
                [sys.executable, str(RESOLVER), "select", "--rpx", str(bare_rpx), "--manifest-dir", str(manifests)],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(ambiguous.returncode, 0)
            self.assertIn("several manifests describe", ambiguous.stderr)


if __name__ == "__main__":
    unittest.main()

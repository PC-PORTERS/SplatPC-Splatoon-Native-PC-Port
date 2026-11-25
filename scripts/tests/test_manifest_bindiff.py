"""Tests for manifest_bindiff.py RPX binary diffing and manifest porting."""

import hashlib
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "recompiler" / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import manifest_bindiff
import plt_resolver


def _compress_rpl(data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + zlib.compress(data)


def _build_synthetic_rpx(text_bytes: bytes, text_addr: int = 0x02000020, sda: int = 0x10277580, sda2: int = 0x10008000) -> bytes:
    """Construct a minimal valid 32-bit big-endian Wii U ELF/RPX for tests."""
    shstr_raw = b"\x00.text\x00.fileinfo\x00.shstrtab\x00"
    text_comp = _compress_rpl(text_bytes)

    fileinfo = bytearray(0x60)
    struct.pack_into(">I", fileinfo, 0, manifest_bindiff.RPL_FILEINFO_MAGIC)
    struct.pack_into(">I", fileinfo, 36, sda)
    struct.pack_into(">I", fileinfo, 40, sda2)

    ehdr_size = 52
    shnum = 4
    shoff = ehdr_size
    data_start = ehdr_size + shnum * 40

    off_text = data_start
    off_fileinfo = off_text + len(text_comp)
    off_shstr = off_fileinfo + len(fileinfo)

    ehdr = struct.pack(
        ">16sHHIIIIIHHHHHH",
        b"\x7fELF\x01\x02\x01\x00" + b"\x00" * 8,
        0xFE01,
        20,
        1,
        text_addr,
        0,
        shoff,
        0,
        ehdr_size,
        0,
        0,
        40,
        shnum,
        3,
    )

    sh_null = struct.pack(">10I", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    sh_text = struct.pack(
        ">10I",
        1,
        1,
        manifest_bindiff.SHF_ALLOC | manifest_bindiff.SHF_EXECINSTR | manifest_bindiff.SHF_RPL_ZLIB,
        text_addr,
        off_text,
        len(text_comp),
        0,
        0,
        4,
        0,
    )
    sh_fileinfo = struct.pack(
        ">10I",
        7,
        manifest_bindiff.SHT_RPL_FILEINFO,
        0,
        0,
        off_fileinfo,
        len(fileinfo),
        0,
        0,
        4,
        0,
    )
    sh_shstr = struct.pack(">10I", 17, 3, 0, 0, off_shstr, len(shstr_raw), 0, 0, 1, 0)

    return ehdr + sh_null + sh_text + sh_fileinfo + sh_shstr + text_comp + bytes(fileinfo) + shstr_raw


class ManifestBinDiffTests(unittest.TestCase):
    def test_identical_rpx_ports_manifest_and_seals(self):
        # 16 unique PowerPC instructions
        words = [0x38600000 + i for i in range(16)]
        code = b"".join(struct.pack(">I", w) for w in words)
        rpx_bytes = _build_synthetic_rpx(code)
        rpx_sha = hashlib.sha256(rpx_bytes).hexdigest()

        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            eur_dir = root / "eur"
            usa_dir = root / "usa"
            for d, tid, pcode, reg in (
                (eur_dir, "0005000E10176A00", "WUP-P-AGMP", "00000004"),
                (usa_dir, "0005000E10176900", "WUP-P-AGME", "00000002"),
            ):
                (d / "code").mkdir(parents=True)
                (d / "meta").mkdir(parents=True)
                (d / "code" / "Gambit.rpx").write_bytes(rpx_bytes)
                (d / "code" / "app.xml").write_text(
                    f"<app><version>16</version><title_id>{tid}</title_id><title_version>0120</title_version></app>",
                    encoding="utf-8",
                )
                (d / "meta" / "meta.xml").write_text(
                    f"<menu><product_code>{pcode}</product_code><title_version>288</title_version><region>{reg}</region></menu>",
                    encoding="utf-8",
                )

            ref_manifest = root / "eur.manifest"
            ref_manifest.write_text(
                "# RebrewU manifest Splatoon (EUR, AGMP, update v288)\n"
                "[manifest]\n"
                "format      = rebrew-manifest/1\n"
                "id          = gambit-eur-v288\n"
                "title       = Splatoon (EUR, AGMP, update v288)\n"
                "generator   = test\n"
                "[identity]\n"
                "hash = 0x0000000000000000\n"
                "title_id        = 0x0005000E10176A00\n"
                "title_version   = 0x0120\n"
                "app_version     = 288\n"
                "region          = EUR\n"
                "product_code    = WUP-P-AGMP\n"
                "module_name     = Gambit\n"
                f"image_size      = 0x{len(rpx_bytes):08X}\n"
                f"image_sha256    = {rpx_sha}\n"
                "entry_point     = 0x02000020\n"
                "sda_base        = 0x10277580\n"
                "sda2_base       = 0x10008000\n"
                "[modules]\n"
                "hash = 0x0000000000000000\n"
                "coreinit = os/coreinit/coreinit_addrs.h\n"
                "[imports]\n"
                "hash = 0x0000000000000000\n"
                "coreinit!OSReport = 0x02000020\n"
                "[entries]\n"
                "hash = 0x0000000000000000\n"
                "0x02000020 = normal\n"
                "[hooks]\n"
                "hash = 0x0000000000000000\n"
                "0x02000024 = hook  # test hook\n"
                "[patches]\n"
                "hash = Undefined\n"
                "[globals]\n"
                "sda = 0x10277580\n"
                "[branch_tables]\n"
                "0x02000020 = 0x02000024 0x02000028\n",
                encoding="utf-8",
            )
            plt_resolver.seal_file(ref_manifest)

            out_manifest = root / "usa.manifest"
            rc = manifest_bindiff.main(
                [
                    "--ref-manifest",
                    str(ref_manifest),
                    "--ref-rpx",
                    str(eur_dir / "code" / "Gambit.rpx"),
                    "--target-rpx",
                    str(usa_dir / "code" / "Gambit.rpx"),
                    "--out",
                    str(out_manifest),
                ]
            )
            self.assertEqual(rc, 0)
            loaded = plt_resolver.load_manifest(out_manifest)
            self.assertTrue(plt_resolver.verify_hashes(loaded))
            self.assertEqual(loaded.id, "gambit-usa-v288")
            self.assertEqual(loaded.identity_string("region"), "USA")
            self.assertEqual(loaded.identity_string("product_code"), "WUP-P-AGME")
            self.assertEqual(loaded.identity_string("title_id"), "0x0005000E10176900")
            self.assertEqual(loaded.entries, [(0x02000020, "normal")])
            self.assertEqual(loaded.hooks, [(0x02000024, "hook")])

    def test_shifted_text_relocates_by_normalized_ppc_signature(self):
        # Build 12 distinct instructions so a 32-byte (8-insn) normalized signature matches uniquely
        # even after shifting code by +16 bytes and changing branch/addi immediates.
        insns_ref = [
            0x7C0802A6,  # mflr r0
            0x9421FFE0,  # stwu r1, -0x20(r1)
            0x90010024,  # stw r0, 0x24(r1)
            0x7C641B78,  # mr r4, r3
            0x7C852214,  # add r4, r5, r4
            0x7C832378,  # mr r3, r4
            0x38630001,  # addi r3, r3, 1
            0x48000010,  # b +0x10
            0x80010024,  # lwz r0, 0x24(r1)
            0x7C0803A6,  # mtlr r0
            0x38210020,  # addi r1, r1, 0x20
            0x4E800020,  # blr
        ]
        # Shift by 4 NOPs (+0x10) and tweak immediate in addi r3, r3, 2 and branch displacement
        insns_tgt = [0x60000000] * 4 + insns_ref[:6] + [0x38630002, 0x48000020] + insns_ref[8:]

        ref_bytes = _build_synthetic_rpx(b"".join(struct.pack(">I", w) for w in insns_ref))
        tgt_bytes = _build_synthetic_rpx(b"".join(struct.pack(">I", w) for w in insns_tgt))

        with tempfile.TemporaryDirectory() as tmpdir:
            ref_path = Path(tmpdir) / "ref.rpx"
            tgt_path = Path(tmpdir) / "tgt.rpx"
            ref_path.write_bytes(ref_bytes)
            tgt_path.write_bytes(tgt_bytes)

            ref_img = manifest_bindiff.RpxImage(ref_path)
            tgt_img = manifest_bindiff.RpxImage(tgt_path)
            diff = manifest_bindiff.diff_images(ref_img, tgt_img)
            self.assertFalse(diff["identical_image"])
            self.assertFalse(diff["identical_text"])

            match = ref_img.match_code_address(tgt_img, 0x02000020)
            self.assertIsNotNone(match)
            self.assertEqual(match["target"], 0x02000030)


if __name__ == "__main__":
    unittest.main()

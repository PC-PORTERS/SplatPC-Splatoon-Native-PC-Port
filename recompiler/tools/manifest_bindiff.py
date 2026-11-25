#!/usr/bin/env python3
"""Binary-diff tool to inspect RPX differences and port a manifest to a target RPX.

Compares a reference RPX and target RPX section-by-section (with optional
normalized PowerPC instruction matching for .text), and generates a sealed
target manifest from a reference manifest + target RPX.
"""

from __future__ import annotations

import argparse
import array
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import plt_resolver

SHF_ALLOC = 0x00000002
SHF_EXECINSTR = 0x00000004
SHF_RPL_ZLIB = 0x08000000
SHT_NOBITS = 8
SHT_RPL_FILEINFO = 0x80000004
RPL_FILEINFO_MAGIC = 0xCAFE0402


class RpxImage:
    """Minimal big-endian 32-bit Wii U ELF/RPX reader for binary diffing."""

    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)
        self.raw = self.path.read_bytes()
        if len(self.raw) < 52 or self.raw[:6] != b"\x7fELF\x01\x02":
            raise ValueError(f"Expected a 32-bit big-endian Wii U ELF/RPX: {self.path}")

        self.size = len(self.raw)
        self.sha256 = hashlib.sha256(self.raw).hexdigest()

        header = struct.unpack_from(">16sHHIIIIIHHHHHH", self.raw, 0)
        self.entry_point = header[4]
        shoff, entsize, count, stridx = header[6], header[11], header[12], header[13]
        if entsize < 40 or shoff + count * entsize > self.size or stridx >= count:
            raise ValueError(f"Malformed ELF section header table in {self.path}")

        raw_headers = [struct.unpack_from(">10I", self.raw, shoff + i * entsize) for i in range(count)]

        def read_section_bytes(hdr: tuple[int, ...]) -> bytes:
            sh_type, sh_flags, sh_offset, sh_size = hdr[1], hdr[2], hdr[4], hdr[5]
            if sh_type == SHT_NOBITS:
                return b""
            blob = self.raw[sh_offset : sh_offset + sh_size]
            if sh_flags & SHF_RPL_ZLIB:
                if len(blob) < 4:
                    raise ValueError(f"Truncated compressed section in {self.path}")
                expected = struct.unpack_from(">I", blob, 0)[0]
                out = zlib.decompress(blob[4:])
                if len(out) != expected:
                    raise ValueError(f"Decompressed size mismatch in {self.path}: {len(out)} != {expected}")
                return out
            return blob

        shstrtab = read_section_bytes(raw_headers[stridx])
        self.sections: dict[str, dict[str, object]] = {}
        self.section_order: list[str] = []
        self.sda_base: int | None = None
        self.sda2_base: int | None = None

        for idx, hdr in enumerate(raw_headers):
            sh_name, sh_type, sh_flags, sh_addr, _, sh_size = hdr[:6]
            data = read_section_bytes(hdr)
            name = shstrtab[sh_name:].split(b"\x00", 1)[0].decode("utf-8", errors="replace")
            if not name and idx == 0:
                name = "<null>"
            key = name if name not in self.sections else f"{name}#{idx}"
            self.section_order.append(key)
            self.sections[key] = {
                "name": name,
                "type": sh_type,
                "flags": sh_flags,
                "address": sh_addr,
                "mem_size": sh_size if sh_type == SHT_NOBITS else len(data),
                "data": data,
                "sha256": hashlib.sha256(data).hexdigest(),
            }
            if sh_type == SHT_RPL_FILEINFO and len(data) >= 44:
                magic = struct.unpack_from(">I", data, 0)[0]
                if magic == RPL_FILEINFO_MAGIC:
                    self.sda_base = struct.unpack_from(">I", data, 36)[0]
                    self.sda2_base = struct.unpack_from(">I", data, 40)[0]

        if ".text" not in self.sections:
            raise ValueError(f"Missing .text section in {self.path}")
        text_sec = self.sections[".text"]
        self.text_start = int(text_sec["address"])
        self.text_bytes = bytes(text_sec["data"])
        self.normalized_text = self._normalize_ppc(self.text_bytes)

    @staticmethod
    def _normalize_ppc(code: bytes) -> bytes:
        usable = len(code) - (len(code) % 4)
        normalized = array.array("I")
        for (word,) in struct.iter_unpack(">I", code[:usable]):
            op = word >> 26
            if op == 18:
                word &= 0xFC000003
            elif op == 16:
                word &= 0xFFFF0003
            elif op in (14, 15) or 32 <= op <= 55:
                word &= 0xFFFF0000
            normalized.append(word)
        return normalized.tobytes()

    def in_text(self, addr: int) -> bool:
        return self.text_start <= addr < self.text_start + len(self.text_bytes)

    def match_code_address(self, other: RpxImage, addr: int) -> dict[str, int | str] | None:
        if not self.in_text(addr):
            return None
        if self.text_bytes == other.text_bytes and self.text_start == other.text_start:
            return {"target": addr, "method": "identical_text", "signature_bytes": 0}
        off = addr - self.text_start
        for length in (128, 96, 64, 48, 32):
            if off + length > len(self.normalized_text):
                continue
            sig = self.normalized_text[off : off + length]
            pos = other.normalized_text.find(sig)
            if pos >= 0 and pos % 4 == 0 and other.normalized_text.find(sig, pos + 1) < 0:
                return {"target": other.text_start + pos, "method": "normalized_sig", "signature_bytes": length}
        return None


def diff_images(ref: RpxImage, target: RpxImage) -> dict[str, object]:
    """Compare two RPX images at raw-file and ELF-section granularity."""
    section_diffs = []
    all_keys = list(dict.fromkeys(ref.section_order + target.section_order))
    identical_sections = 0
    for key in all_keys:
        a = ref.sections.get(key)
        b = target.sections.get(key)
        if a is None:
            section_diffs.append({"section": key, "status": "added_in_target", "target_address": f"0x{b['address']:08X}"})
        elif b is None:
            section_diffs.append({"section": key, "status": "removed_in_target", "ref_address": f"0x{a['address']:08X}"})
        elif a["address"] == b["address"] and a["mem_size"] == b["mem_size"] and a["sha256"] == b["sha256"]:
            identical_sections += 1
        else:
            section_diffs.append(
                {
                    "section": key,
                    "status": "modified",
                    "ref_address": f"0x{a['address']:08X}",
                    "target_address": f"0x{b['address']:08X}",
                    "ref_size": a["mem_size"],
                    "target_size": b["mem_size"],
                }
            )

    return {
        "ref_path": str(ref.path),
        "target_path": str(target.path),
        "ref_size": ref.size,
        "target_size": target.size,
        "ref_sha256": ref.sha256,
        "target_sha256": target.sha256,
        "identical_image": ref.sha256 == target.sha256,
        "identical_text": ref.text_start == target.text_start and ref.text_bytes == target.text_bytes,
        "total_sections": len(all_keys),
        "identical_sections": identical_sections,
        "section_diffs": section_diffs,
    }


def _collect_imports_via_rebrewu(binary: str, rpx_path: Path) -> list[plt_resolver.Import]:
    imports = []
    for line in plt_resolver.run_rebrewu(binary, ["imports", str(rpx_path)]).splitlines():
        match = plt_resolver.IMPORT_RE.match(line.strip())
        if match:
            addr = int(match.group(3), 16)
            imports.append(plt_resolver.Import(match.group(1), match.group(2), addr, addr >= 0xC0000000))
    return imports


def _split_comment(line: str) -> tuple[str, str]:
    for idx, ch in enumerate(line):
        if ch == "#":
            return line[:idx], line[idx:]
        if ch == "/" and idx + 1 < len(line) and line[idx + 1] == "/":
            return line[:idx], line[idx:]
    return line, ""


def _build_target_identity(
    ref_manifest: plt_resolver.Manifest,
    target_rpx: RpxImage,
    args: argparse.Namespace,
) -> dict[str, str]:
    derived = plt_resolver.identity_from_image(target_rpx.path)
    identity = dict(ref_manifest.identity)

    for key in ("title_id", "title_version", "app_version", "region", "product_code"):
        if key in derived and derived[key]:
            identity[key] = derived[key]

    overrides = {
        "title_id": args.title_id,
        "title_version": args.title_version,
        "app_version": args.app_version,
        "region": args.region,
        "product_code": args.product_code,
    }
    for key, value in overrides.items():
        if value:
            identity[key] = value

    identity["module_name"] = ref_manifest.identity_string("module_name") or target_rpx.path.stem
    identity["image_size"] = f"0x{target_rpx.size:08X}"
    identity["image_sha256"] = target_rpx.sha256
    identity["entry_point"] = f"0x{target_rpx.entry_point:08X}"
    if target_rpx.sda_base is not None:
        identity["sda_base"] = f"0x{target_rpx.sda_base:08X}"
    if target_rpx.sda2_base is not None:
        identity["sda2_base"] = f"0x{target_rpx.sda2_base:08X}"
    return identity


def _relocate_sections_and_write(
    ref_manifest_path: Path,
    ref_manifest: plt_resolver.Manifest,
    ref_rpx: RpxImage,
    target_rpx: RpxImage,
    out_path: Path,
    args: argparse.Namespace,
) -> dict[str, object]:
    identical_image = ref_rpx.sha256 == target_rpx.sha256
    target_identity = _build_target_identity(ref_manifest, target_rpx, args)

    region = target_identity.get("region", "unknown")
    product_short = target_identity.get("product_code", "").rsplit("-", 1)[-1] or target_rpx.path.stem
    app_version = target_identity.get("app_version", "0")
    manifest_id = args.id or f"{target_identity.get('module_name', 'gambit').lower()}-{region.lower()}-v{app_version}"
    title = args.title or f"Splatoon ({region}, {product_short}, update v{app_version})"

    # Build import mapping when images differ (for globals pointing at PLT slots).
    target_imports_by_sym: dict[str, int] = {}
    if not identical_image:
        binary = plt_resolver.find_rebrewu(args.rebrewu)
        if not binary:
            raise SystemExit("Target RPX differs from reference RPX; rebrewu binary is required to scan target imports.")
        target_imports = _collect_imports_via_rebrewu(binary, target_rpx.path)
        target_imports_by_sym = {f"{item.module}!{item.symbol}": item.address for item in target_imports}
    else:
        target_imports_by_sym = {f"{item.module}!{item.symbol}": item.address for item in ref_manifest.imports}

    ref_addr_to_sym: dict[int, str] = {item.address: f"{item.module}!{item.symbol}" for item in ref_manifest.imports}

    def map_code_addr(addr: int) -> int | None:
        if identical_image:
            return addr
        matched = ref_rpx.match_code_address(target_rpx, addr)
        return int(matched["target"]) if matched else None

    def map_global(name: str, addr: int) -> tuple[int | None, str]:
        if identical_image:
            return addr, "identical_image"
        if name == "sda" and target_rpx.sda_base is not None:
            return target_rpx.sda_base, "fileinfo_sda"
        if addr in ref_addr_to_sym:
            sym = ref_addr_to_sym[addr]
            if sym in target_imports_by_sym:
                return target_imports_by_sym[sym], f"import:{sym}"
        if ref_rpx.in_text(addr):
            mapped = map_code_addr(addr)
            if mapped is not None:
                return mapped, "normalized_sig"
        # Check section-relative mapping (e.g. plt_begin/plt_end or unchanged data sections).
        for sec_name, sec in ref_rpx.sections.items():
            sec_addr = int(sec["address"])
            sec_size = int(sec["mem_size"])
            if sec_size > 0 and sec_addr <= addr <= sec_addr + sec_size and sec_name in target_rpx.sections:
                t_sec = target_rpx.sections[sec_name]
                if sec["sha256"] == t_sec["sha256"]:
                    return int(t_sec["address"]) + (addr - sec_addr), f"identical_section:{sec_name}"
        return None, "unmapped"

    unmapped: dict[str, list[str]] = {"entries": [], "hooks": [], "patches": [], "globals": [], "branch_tables": []}
    stats = {"entries_mapped": 0, "hooks_mapped": 0, "patches_mapped": 0, "globals_mapped": 0, "branch_tables_mapped": 0}

    ref_lines = ref_manifest_path.read_text(encoding="utf-8").splitlines()
    out_lines: list[str] = []
    current_section: str | None = None
    header_done = False

    for line in ref_lines:
        code_part, comment_part = _split_comment(line)
        bare = code_part.strip()

        if not header_done and current_section is None:
            if bare.startswith("["):
                header_done = True
            elif line.startswith("# RebrewU manifest"):
                out_lines.append(f"# RebrewU manifest {title}")
                continue

        if bare.startswith("[") and bare.endswith("]"):
            current_section = bare[1:-1].strip().lower()
            out_lines.append(line)
            continue

        if not bare or "=" not in bare or current_section is None:
            out_lines.append(line)
            continue

        key, raw_val = (part.strip() for part in bare.split("=", 1))
        if key.lower() == "hash":
            out_lines.append(line)
            continue

        suffix = ("  " + comment_part) if comment_part else ""

        if current_section == "manifest":
            if key == "id":
                out_lines.append(f"id          = {manifest_id}")
            elif key == "title":
                out_lines.append(f"title       = {title}")
            elif key == "generator":
                out_lines.append("generator   = recompiler/tools/manifest_bindiff.py")
            else:
                out_lines.append(line)
            continue

        if current_section == "identity":
            if key in target_identity:
                out_lines.append(f"{key:<15} = {target_identity[key]}")
            else:
                out_lines.append(line)
            continue

        if current_section == "imports":
            if key in target_imports_by_sym:
                addr = target_imports_by_sym[key]
                mode = " data" if addr >= 0xC0000000 else ""
                out_lines.append(f"{key:<60} = 0x{addr:08X}{mode}")
            else:
                out_lines.append(line)
            continue

        if current_section == "entries":
            addr = plt_resolver.parse_hex_u32(key)
            mapped = map_code_addr(addr) if addr is not None else None
            if mapped is not None:
                stats["entries_mapped"] += 1
                out_lines.append(f"0x{mapped:08X} = {raw_val}{suffix}")
            elif addr is not None:
                unmapped["entries"].append(f"0x{addr:08X}")
            continue

        if current_section == "hooks":
            addr = plt_resolver.parse_hex_u32(key)
            mapped = map_code_addr(addr) if addr is not None else None
            if mapped is not None:
                stats["hooks_mapped"] += 1
                out_lines.append(f"0x{mapped:08X} = {raw_val}{suffix}")
            elif addr is not None:
                unmapped["hooks"].append(f"0x{addr:08X}")
            continue

        if current_section == "patches":
            addr = plt_resolver.parse_hex_u32(key)
            mapped = map_code_addr(addr) if addr is not None else None
            if mapped is not None:
                stats["patches_mapped"] += 1
                out_lines.append(f"0x{mapped:08X} = {raw_val}{suffix}")
            elif addr is not None:
                unmapped["patches"].append(f"0x{addr:08X}")
            continue

        if current_section == "globals":
            addr = plt_resolver.parse_hex_u32(raw_val)
            if addr is not None:
                mapped, _ = map_global(key, addr)
                if mapped is not None:
                    stats["globals_mapped"] += 1
                    out_lines.append(f"{key:<20} = 0x{mapped:08X}{suffix}")
                else:
                    unmapped["globals"].append(f"{key}=0x{addr:08X}")
            else:
                out_lines.append(line)
            continue

        if current_section == "branch_tables":
            base_addr = plt_resolver.parse_hex_u32(key)
            mapped_base = map_code_addr(base_addr) if base_addr is not None else None
            targets = [plt_resolver.parse_hex_u32(tok) for tok in raw_val.split()]
            mapped_targets = [map_code_addr(t) if t is not None else None for t in targets]
            if mapped_base is not None and all(t is not None for t in mapped_targets):
                stats["branch_tables_mapped"] += 1
                formatted = " ".join(f"0x{t:08X}" for t in mapped_targets if t is not None)
                out_lines.append(f"0x{mapped_base:08X} = {formatted}{suffix}")
            elif base_addr is not None:
                unmapped["branch_tables"].append(f"0x{base_addr:08X}")
            continue

        out_lines.append(line)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(out_lines).rstrip("\n") + "\n", encoding="utf-8")
    sealed_sections = plt_resolver.seal_file(out_path)

    written_manifest = plt_resolver.load_manifest(out_path)
    if not plt_resolver.verify_hashes(written_manifest):
        raise SystemExit("Generated manifest failed hash verification: " + "; ".join(written_manifest.errors))
    problems = plt_resolver.verify_image(written_manifest, str(target_rpx.path))
    if problems:
        raise SystemExit("Generated manifest failed image verification: " + "; ".join(problems))

    return {
        "out": str(out_path),
        "id": written_manifest.id,
        "sealed_sections": sealed_sections,
        "identical_image": identical_image,
        "stats": stats,
        "unmapped": unmapped,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ref-rpx", required=True, type=Path, help="reference RPX image")
    parser.add_argument("--target-rpx", required=True, type=Path, help="target RPX image")
    parser.add_argument("--ref-manifest", type=Path, help="reference manifest to port to the target RPX")
    parser.add_argument("--out", type=Path, help="output manifest path (required unless --diff-only)")
    parser.add_argument("--diff-only", action="store_true", help="only compare the two RPXs without writing a manifest")
    parser.add_argument("--report", type=Path, help="optional JSON binary-diff report path")
    parser.add_argument("--id", help="override [manifest] id")
    parser.add_argument("--title", help="override [manifest] title")
    parser.add_argument("--title-id", help="override [identity] title_id")
    parser.add_argument("--title-version", help="override [identity] title_version")
    parser.add_argument("--app-version", help="override [identity] app_version")
    parser.add_argument("--region", help="override [identity] region")
    parser.add_argument("--product-code", help="override [identity] product_code")
    parser.add_argument("--rebrewu", help="optional path to rebrewu binary")
    parser.add_argument("--skip-ref-check", action="store_true", help="skip checking --ref-rpx against --ref-manifest")
    args = parser.parse_args(argv)

    ref_rpx = RpxImage(args.ref_rpx)
    target_rpx = RpxImage(args.target_rpx)
    diff = diff_images(ref_rpx, target_rpx)

    print(
        f"Binary diff: identical_image={diff['identical_image']} "
        f"identical_text={diff['identical_text']} "
        f"sections={diff['identical_sections']}/{diff['total_sections']}"
    )
    if diff["section_diffs"]:
        for item in diff["section_diffs"]:
            print(f"  section {item['section']}: {item['status']}")

    result: dict[str, object] = {"diff": diff}

    if not args.diff_only:
        if not args.ref_manifest or not args.out:
            raise SystemExit("--ref-manifest and --out are required unless --diff-only is set")
        ref_manifest = plt_resolver.load_manifest(args.ref_manifest)
        if not args.skip_ref_check:
            ref_problems = plt_resolver.verify_image(ref_manifest, str(ref_rpx.path))
            if ref_problems:
                raise SystemExit("Reference RPX does not match --ref-manifest: " + "; ".join(ref_problems))
        port_result = _relocate_sections_and_write(args.ref_manifest, ref_manifest, ref_rpx, target_rpx, args.out, args)
        result["manifest"] = port_result
        print(
            f"Wrote {port_result['out']} (id={port_result['id']}, "
            f"sealed={port_result['sealed_sections']} sections, stats={port_result['stats']})"
        )
        for sec, missing in port_result["unmapped"].items():
            if missing:
                print(f"warning: {len(missing)} unmapped {sec}: {', '.join(missing[:5])}", file=sys.stderr)

    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(f"Report written: {args.report}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

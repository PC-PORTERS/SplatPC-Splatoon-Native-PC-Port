#!/usr/bin/env python3
"""plt_resolver.py : generate a manifest, and the artifacts it produces.

Two deliberately separate jobs:

  scan    build the *base* of a manifest for one guest image by asking the
          recompiler for that image's import table. The recompiler owns the
          ELF/RPL decoding, so the address rules live in exactly one place.

  emit    generate the address headers and the PLT stub table from a manifest.
          Standard library only: the port build needs no C++ tool, and the
          generated files are not committed.

Also: select, list, verify, seal, show.

Typical use:

  # which manifest describes this image? (all Manifest/*.manifest are candidates)
  recompiler/tools/plt_resolver.py select --rpx /path/to/Gambit.rpx \\
      --manifest-dir Manifest

  # one-off, when adding a version or a region (needs a built recompiler)
  recompiler/tools/plt_resolver.py scan --rpx /path/to/Gambit.rpx \\
      --out Manifest/Gambit_usa_v16.manifest --id gambit-usa-v16 --region USA

  # every build, from anywhere
  recompiler/tools/plt_resolver.py emit \\
      --manifest Manifest/Gambit_v16.manifest --out recompiler/port

Format reference: Manifest/README.md
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

FORMAT = "rebrew-manifest/1"
HASHED_SECTIONS = ("identity", "modules", "imports", "entries", "hooks", "patches")
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1

PLT_AUTO = "os/gambit_plt_auto.cpp"



# Text helpers (must agree with recompiler/src/core/manifest/manifest.cpp)


def strip_comment(line):
    out = []
    i = 0
    while i < len(line):
        char = line[i]
        if char == "#":
            break
        if char == "/" and i + 1 < len(line) and line[i + 1] == "/":
            break
        out.append(char)
        i += 1
    return "".join(out)


def all_hex(text):
    return bool(text) and all(c in "0123456789abcdefABCDEF" for c in text)


def parse_hex_u32(text):
    value = text.strip()
    if len(value) > 2 and value[0] == "0" and value[1] in "xX":
        value = value[2:]
    value = value.rstrip("uU")
    if not value or len(value) > 8 or not all_hex(value):
        return None
    return int(value, 16)


def normalize_atom(raw):
    value = raw.strip()
    if len(value) > 2 and value[0] == "0" and value[1] in "xX":
        tail = value[2:].rstrip("uU")
        if all_hex(tail):
            return "0x" + tail.lower()
        return value
    if all_hex(value):
        return value.lower()
    return value


def normalize_value(raw):
    value = raw.strip()
    parts = value.split(None, 1)
    if len(parts) == 2:
        head, tail = parts[0], parts[1].strip()
        if parse_hex_u32(head) is not None and tail and len(tail.split(None, 1)) == 1:
            return normalize_atom(head) + " " + tail.lower()
    return normalize_atom(value)


def section_hash(pairs):
    ordered = sorted((k, normalize_value(v)) for k, v in pairs.items() if k != "hash")
    digest = FNV_OFFSET
    for key, value in ordered:
        for byte in key.encode("utf-8"):
            digest = ((digest ^ byte) * FNV_PRIME) & MASK64
        digest = ((digest ^ 0x1F) * FNV_PRIME) & MASK64
        for byte in value.encode("utf-8"):
            digest = ((digest ^ byte) * FNV_PRIME) & MASK64
        digest = ((digest ^ 0x0A) * FNV_PRIME) & MASK64
    return digest


def format_hash(value):
    return "0x%016X" % value



# Manifest model


class Import:
    __slots__ = ("module", "symbol", "address", "data")

    def __init__(self, module, symbol, address, data=False):
        self.module = module
        self.symbol = symbol
        self.address = address
        self.data = data


class Manifest:
    def __init__(self, path):
        self.path = Path(path)
        self.meta = {}
        self.identity = {}
        self.modules = {}
        self.imports = []
        self.entries = []
        self.hooks = []
        self.patches = []
        self.stored = {}
        self.computed = {}
        self.ignored = set()
        self.errors = []
        self.warnings = []

    @property
    def id(self):
        return self.meta.get("id", "").strip()

    def identity_string(self, key):
        return self.identity.get(key, "").strip()

    def identity_u32(self, key):
        value = parse_hex_u32(self.identity_string(key)) if self.identity.get(key) else None
        return value if value is not None else 0


def parse_manifest(text, path):
    m = Manifest(path)
    sections = {}
    current = None
    for number, raw in enumerate(text.splitlines(), 1):
        line = strip_comment(raw).strip()
        if not line:
            continue
        if line.startswith("["):
            if not line.endswith("]"):
                m.errors.append("line %d: unterminated section header" % number)
                continue
            current = line[1:-1].strip().lower()
            sections.setdefault(current, {})
            continue
        if "=" not in line:
            m.errors.append("line %d: expected 'key = value'" % number)
            continue
        if current is None:
            m.errors.append("line %d: entry outside any section" % number)
            continue
        key, value = line.split("=", 1)
        key, value = key.strip(), value.strip()
        if not key:
            m.errors.append("line %d: empty key" % number)
            continue
        if key in sections[current]:
            m.errors.append("line %d: duplicate key '%s' in [%s]" % (number, key, current))
            continue
        sections[current][key] = value

    m.meta = dict(sections.get("manifest", {}))
    m.identity = {k: v for k, v in sections.get("identity", {}).items() if k != "hash"}
    m.modules = {k: v.strip() for k, v in sections.get("modules", {}).items() if k != "hash"}

    for key, value in sections.get("imports", {}).items():
        if key == "hash":
            continue
        if "!" not in key:
            m.errors.append("[imports] key '%s' is not 'module!symbol'" % key)
            continue
        module, symbol = key.split("!", 1)
        parts = value.split(None, 1)
        address_text, is_data = parts[0], False
        if len(parts) == 2:
            mode = parts[1].strip().lower()
            if mode == "data":
                is_data = True
            else:
                m.errors.append("[imports] %s: mode must be 'data', got '%s'" % (key, mode))
                continue
        address = parse_hex_u32(address_text)
        if address is None:
            m.errors.append("[imports] %s: bad address '%s'" % (key, address_text))
            continue
        m.imports.append(Import(module.strip(), symbol.strip(), address, is_data))
    m.imports.sort(key=lambda i: (i.module, i.symbol))

    seen_entries = {}
    for key, value in sections.get("entries", {}).items():
        if key == "hash":
            continue
        address = parse_hex_u32(key)
        if address is None:
            m.errors.append("[entries] '%s' is not a 32-bit address" % key)
            continue
        mode = value.strip().lower()
        if mode not in ("normal", "force", "corroborate"):
            m.errors.append("[entries] %s: mode must be 'normal', 'force' or 'corroborate'" % key)
            continue
        if address in seen_entries:
            m.errors.append("[entries] %s duplicates %s" % (key, seen_entries[address]))
            continue
        seen_entries[address] = key
        m.entries.append((address, mode))
    m.entries.sort()

    seen_hooks = {}
    for key, value in sections.get("hooks", {}).items():
        if key == "hash":
            continue
        address = parse_hex_u32(key)
        if address is None:
            m.errors.append("[hooks] '%s' is not a 32-bit address" % key)
            continue
        if address & 3:
            m.errors.append("[hooks] %s: address is not 4-byte aligned" % key)
            continue
        if value.strip().lower() != "hook":
            m.errors.append("[hooks] %s: value must be 'hook'" % key)
            continue
        if address in seen_hooks:
            m.errors.append("[hooks] %s duplicates %s" % (key, seen_hooks[address]))
            continue
        seen_hooks[address] = key
        m.hooks.append((address, "hook"))
    m.hooks.sort()

    seen_patches = {}
    for key, value in sections.get("patches", {}).items():
        if key == "hash":
            continue
        address = parse_hex_u32(key)
        if address is None:
            m.errors.append("[patches] '%s' is not a 32-bit address" % key)
            continue
        if address & 3:
            m.errors.append("[patches] %s: address is not 4-byte aligned" % key)
            continue
        instruction = value.strip()
        if not instruction:
            m.errors.append("[patches] %s: missing instruction" % key)
            continue
        if address in seen_patches:
            m.errors.append("[patches] %s duplicates %s" % (key, seen_patches[address]))
            continue
        seen_patches[address] = key
        m.patches.append((address, instruction))
    m.patches.sort()

    for name in HASHED_SECTIONS:
        if name not in sections:
            continue
        pairs = sections[name]
        if "hash" in pairs and pairs["hash"].strip().lower() == "undefined":
            m.ignored.add(name)
            continue
        m.computed[name] = section_hash(pairs)
        if "hash" not in pairs:
            m.warnings.append("section [%s] has no hash line" % name)
            continue
        stored = parse_hex_u32(pairs["hash"]) if len(pairs["hash"].strip().rstrip("uU")) <= 10 else None
        if stored is None:
            text_value = pairs["hash"].strip()
            try:
                stored = int(text_value, 16) if text_value.lower().startswith("0x") else int(text_value, 16)
            except ValueError:
                m.errors.append("[%s] hash is not a 64-bit value" % name)
                continue
        m.stored[name] = stored
    return m


def load_manifest(path):
    text = Path(path).read_text(encoding="utf-8")
    return parse_manifest(text, path)


def verify_hashes(m):
    ok = not m.errors
    for name, computed in m.computed.items():
        if name in m.ignored:
            continue
        if name not in m.stored:
            m.errors.append("[%s] has no hash line (run 'seal')" % name)
            ok = False
        elif m.stored[name] != computed:
            m.errors.append("[%s] hash mismatch: file %s, content %s"
                            % (name, format_hash(m.stored[name]), format_hash(computed)))
            ok = False
    return ok


def verify_image(m, image_path):
    problems = []
    path = Path(image_path)
    if not path.is_file():
        return ["cannot stat image %s" % image_path]
    expected_size = m.identity_u32("image_size")
    if expected_size and path.stat().st_size != expected_size:
        problems.append("image size mismatch: image %d bytes, manifest %d"
                        % (path.stat().st_size, expected_size))
    expected_hash = m.identity_string("image_sha256").lower()
    if expected_hash:
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != expected_hash:
            problems.append("image sha256 mismatch: image %s, manifest %s"
                            % (actual, expected_hash))
    return problems


def discover_manifests(manifest_dirs=(), manifest_files=()):
    """Every *.manifest below <dir>/Manifest (or directly in <dir>)."""
    candidates = [Path(p) for p in manifest_files]
    for directory in manifest_dirs:
        base = Path(directory)
        if not base.is_dir():
            continue
        for candidate_dir in (base / "Manifest", base):
            if candidate_dir.is_dir():
                candidates.extend(candidate_dir.glob("*.manifest"))
    unique = {}
    for path in candidates:
        if not path.is_file():
            continue
        try:
            key = path.resolve()
        except OSError:
            key = path.absolute()
        unique[key] = path
    return [unique[key] for key in sorted(unique, key=lambda p: str(p))]


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def manifest_summary(m):
    text = m.id or "(no id)"
    tags = [m.identity_string(key) for key in ("title_id", "product_code", "region")]
    tags = [tag for tag in tags if tag]
    if tags:
        text += " [%s]" % " ".join(tags)
    size = m.identity_string("image_size")
    if size:
        text += " size=" + size
    digest = m.identity_string("image_sha256").lower()
    if digest:
        text += " sha256=" + digest
    return text


def select_manifest(image_path, manifests):
    """The manifest whose [identity] describes this image.

    An exact SHA-256 match wins over a size-only match. Several matches of the
    same strength are ambiguous: the caller must pick with --manifest.
    """
    image = Path(image_path)
    if not image.is_file():
        raise SystemExit("image not found: %s" % image_path)
    size = image.stat().st_size
    digest = sha256_file(image)
    exact, size_only, rejected, unreadable = [], [], [], []
    for path in manifests:
        try:
            m = load_manifest(path)
        except (OSError, UnicodeError) as exc:
            unreadable.append("%s: %s" % (path, exc))
            continue
        expected_size = m.identity_u32("image_size") if m.identity.get("image_size") else 0
        expected_hash = m.identity_string("image_sha256").lower()
        if expected_hash and expected_hash == digest:
            exact.append(m)
        elif not expected_hash and expected_size and expected_size == size:
            size_only.append(m)
        else:
            rejected.append(m)

    if len(exact) == 1:
        return exact[0]
    if not exact and len(size_only) == 1:
        return size_only[0]
    if len(exact) > 1 or len(size_only) > 1:
        clashing = exact if len(exact) > 1 else size_only
        raise SystemExit("several manifests describe %s; pass --manifest:\n  %s"
                         % (image_path, "\n  ".join(str(m.path) for m in clashing)))

    lines = ["no manifest describes %s (image size %d bytes, sha256 %s)"
             % (image_path, size, digest)]
    for m in rejected:
        lines.append("  %s: %s" % (m.path, manifest_summary(m)))
    for problem in unreadable:
        lines.append("  " + problem)
    lines.append("add a manifest for this image, or pass --manifest <file>")
    raise SystemExit("\n".join(lines))


def seal_file(path):
    text = Path(path).read_text(encoding="utf-8")
    parsed = parse_manifest(text, path)
    if parsed.errors:
        raise SystemExit("cannot seal: " + parsed.errors[0])
    lines = text.splitlines()
    output = []
    current = None
    pending = False

    def flush():
        nonlocal pending
        if pending and current in parsed.computed:
            output.append("hash = " + format_hash(parsed.computed[current]))
        pending = False

    for line in lines:
        bare = strip_comment(line).strip()
        if bare.startswith("[") and bare.endswith("]"):
            flush()
            output.append(line)
            current = bare[1:-1].strip().lower()
            if current in HASHED_SECTIONS and current not in parsed.stored and current not in parsed.ignored:
                pending = True
            continue
        if "=" in bare:
            key = bare.split("=", 1)[0].strip().lower()
            if key == "hash" and current in parsed.computed and current not in parsed.ignored:
                output.append("hash = " + format_hash(parsed.computed[current]))
                continue
        output.append(line)
    flush()
    Path(path).write_text("\n".join(output).rstrip("\n") + "\n", encoding="utf-8")
    return len(parsed.computed)



# Emission


def header_paths(m):
    return sorted({p for p in m.modules.values() if p})


def imports_by_address(m):
    seen = set()
    ordered = []
    for item in sorted(m.imports, key=lambda i: (i.address, i.module, i.symbol)):
        if item.address in seen:
            continue
        seen.add(item.address)
        ordered.append(item)
    return ordered


def banner_for(m):
    return ("// AUTO-GENERATED from manifest '%s' (%s) — DO NOT EDIT\n"
            "// Source of truth: the manifest. Regenerate rather than edit this file.\n"
            % (m.id, m.path.name))


def write_if_changed(path, text):
    path = Path(path)
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return True


def emit(m, out_dir):
    root = Path(out_dir)
    banner = banner_for(m)
    written = []

    for relative in header_paths(m):
        modules = sorted(mod for mod, header in m.modules.items() if header == relative)
        chunks = [banner, "#pragma once\n"]
        for module in modules:
            symbols = sorted((i for i in m.imports if i.module == module),
                             key=lambda i: (i.address, i.symbol))
            if not symbols:
                continue
            chunks.append("\n// --- %s ---\n" % module)
            for item in symbols:
                chunks.append("#define ADDR_%s 0x%08Xu\n" % (item.symbol, item.address))
        if write_if_changed(root / relative, "".join(chunks)):
            written.append((root / relative, True))

    chunks = [
        banner,
        "// Registers STUB for every PLT address so no call goes unhandled.\n",
        "// Module-specific register() functions run afterwards and override\n",
        "// the entries they implement.\n",
        "#include \"os_common.h\"\n\n",
        "void gambit_plt_auto_register(CPUState* cpu) {\n",
    ]
    for item in imports_by_address(m):
        if item.data:
            continue  # a data slot is not a callable thunk
        chunks.append("    rbrew_register_func(cpu, 0x%08Xu, STUB);  // %s::%s\n"
                      % (item.address, item.module, item.symbol))
    chunks.append("}\n")
    if write_if_changed(root / PLT_AUTO, "".join(chunks)):
        written.append((root / PLT_AUTO, True))
    return written



# Scan: build the base of a new manifest from an image


def find_rebrewu(explicit):
    if explicit:
        return explicit
    from_env = os.environ.get("REBREWU")
    if from_env:
        return from_env
    here = Path(__file__).resolve()
    for depth in range(1, 6):
        try:
            root = here.parents[depth]
        except IndexError:
            break
        for name in ("rebrewu", "rebrewu.exe"):
            candidate = root / "build" / "rebrewu" / "bin" / name
            if candidate.is_file():
                return str(candidate)
    return shutil.which("rebrewu")


def run_rebrewu(binary, args):
    result = subprocess.run([binary] + args, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit("rebrewu %s failed:\n%s" % (" ".join(args), result.stderr.strip()))
    return result.stdout


IMPORT_RE = re.compile(r"^([^!]+)!(\S+)\s+stub=0x([0-9A-Fa-f]+)$")

# Region letter carried by a Nintendo product code (AGMP -> trailing P -> EUR).
REGION_BY_LETTER = {
    "J": "JPN", "E": "USA", "P": "EUR", "D": "GER", "F": "FRA",
    "I": "ITA", "S": "ESP", "H": "NED", "K": "KOR", "W": "TWN",
    "R": "RUS", "A": "AUS", "T": "CHT",
}

INSPECT_RE = {
    "entry_point": re.compile(r"^Entry point: 0x([0-9A-Fa-f]+)", re.M),
    "sda_base": re.compile(r"^SDA: 0x([0-9A-Fa-f]+)", re.M),
    "sda2_base": re.compile(r"^SDA2: 0x([0-9A-Fa-f]+)", re.M),
    "module_name": re.compile(r"^Module: (\S+)", re.M),
}


def read_xml_field(path, tag):
    """One <tag>value</tag> from a Wii U metadata file, or None."""
    try:
        text = Path(path).read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return None
    match = re.search(r"<%s\b[^>]*>([^<]*)</%s>" % (tag, tag), text)
    return match.group(1).strip() if match else None


def identity_from_image(image):
    """Everything the image and its sibling metadata can state about itself.

    app.xml and cos.xml sit next to the RPX; meta/meta.xml is one level up.
    Each field is optional: a bare extracted RPX still scans, it just leaves
    the identity sparse for the caller to complete.
    """
    info = {}
    code_dir = image.parent
    app = code_dir / "app.xml"
    title_id = read_xml_field(app, "title_id")
    if title_id:
        info["title_id"] = "0x" + title_id.upper()
    title_version = read_xml_field(app, "title_version")
    if title_version:
        try:
            info["title_version"] = "0x%04X" % int(title_version, 16)
        except ValueError:
            pass
    version = read_xml_field(app, "version")
    if version and version.isdigit():
        info["app_version"] = version

    meta = code_dir.parent / "meta" / "meta.xml"
    product = read_xml_field(meta, "product_code")   # e.g. WUP-P-AGMP
    if product:
        info["product_code"] = product
        short = product.rsplit("-", 1)[-1]
        if short and short[-1] in REGION_BY_LETTER:
            info["region"] = REGION_BY_LETTER[short[-1]]
    if "app_version" not in info:
        meta_version = read_xml_field(meta, "title_version")
        if meta_version and meta_version.isdigit():
            info["app_version"] = meta_version
    for tag in ("longname_en", "shortname_en"):
        name = read_xml_field(meta, tag)
        if name:
            info["game_name"] = name
            break
    return info


def scan(args):
    binary = find_rebrewu(args.rebrewu)
    if not binary:
        raise SystemExit("rebrewu binary not found; build it or pass --rebrewu")
    image = Path(args.rpx)
    if not image.is_file():
        raise SystemExit("image not found: %s" % image)

    imports = []
    for line in run_rebrewu(binary, ["imports", str(image)]).splitlines():
        match = IMPORT_RE.match(line.strip())
        if not match:
            continue
        imports.append(Import(match.group(1), match.group(2), int(match.group(3), 16)))
    if not imports:
        raise SystemExit("no imports reported for %s" % image)

    inspect_output = run_rebrewu(binary, ["inspect", str(image)])
    fields = {}
    for key, pattern in INSPECT_RE.items():
        found = pattern.search(inspect_output)
        if found:
            fields[key] = found.group(1)

    derived = identity_from_image(image)
    identity = {
        "title_id": args.title_id or derived.get("title_id", ""),
        "title_version": args.title_version or derived.get("title_version", ""),
        "app_version": args.app_version or derived.get("app_version", ""),
        "region": args.region or derived.get("region", ""),
        "product_code": args.product_code or derived.get("product_code", ""),
        "game_name": args.title or derived.get("game_name", "") or
                     fields.get("module_name", image.stem),
    }
    if not identity["app_version"]:
        identity["app_version"] = "0"
    if not identity["region"]:
        identity["region"] = "unknown"
    product_short = identity["product_code"].rsplit("-", 1)[-1] or image.stem
    title = "%s (%s, %s, update v%s)" % (identity["game_name"], identity["region"],
                                          product_short, identity["app_version"])

    modules = sorted({i.module for i in imports})
    known_headers = {
        "coreinit": "os/coreinit/coreinit_addrs.h",
        "gx2": "os/gx2/gx2_addrs.h",
        "padscore": "os/padscore/padscore_addrs.h",
        "snd_core": "os/snd_core/snd_core_addrs.h",
        "vpad": "os/vpad/vpad_addrs.h",
    }
    default_header = "os/stubs/stubs_addrs.h"
    data_count = sum(1 for i in imports if i.address >= 0xC0000000)

    # Keep entries, hooks and patches a previous run collected: they cannot be
    # derived statically.
    preserved_entries = []
    preserved_hooks = []
    preserved_patches = []
    out_path = Path(args.out)
    if out_path.exists():
        previous = load_manifest(out_path)
        preserved_entries = previous.entries
        preserved_hooks = previous.hooks
        preserved_patches = previous.patches
        if previous.errors:
            print("warning: previous manifest has errors; entries, hooks and patches not preserved",
                  file=sys.stderr)
            preserved_entries = []
            preserved_hooks = []
            preserved_patches = []

    lines = []
    add = lines.append
    add("# ============================================================================")
    add("# RebrewU manifest — %s" % title)
    add("#")
    add("# ============================================================================")
    add("")
    add("[manifest]")
    add("format      = %s" % FORMAT)
    add("id          = %s" % args.id)
    add("title       = %s" % title)
    add("generator   = recompiler/tools/plt_resolver.py scan")
    add("notes       = Structural metadata only: import symbols, stub addresses, entry points and file digests. No game code or assets.")
    add("")
    add("[identity]")
    add("# Derived from the image itself and from app.xml / meta/meta.xml next to it.")
    add("hash            = 0x0000000000000000")
    if identity["title_id"]:
        add("title_id        = %s" % identity["title_id"])
    if identity["title_version"]:
        add("title_version   = %s" % identity["title_version"])
    add("app_version     = %s" % identity["app_version"])
    add("region          = %s" % identity["region"])
    if identity["product_code"]:
        add("product_code    = %s" % identity["product_code"])
    if fields.get("module_name"):
        add("module_name     = %s" % fields["module_name"])
    add("image_size      = 0x%08X" % image.stat().st_size)
    add("image_sha256    = %s" % hashlib.sha256(image.read_bytes()).hexdigest())
    if fields.get("entry_point"):
        add("entry_point     = 0x%08X" % int(fields["entry_point"], 16))
    if fields.get("sda_base"):
        add("sda_base        = 0x%08X" % int(fields["sda_base"], 16))
    if fields.get("sda2_base"):
        add("sda2_base       = 0x%08X" % int(fields["sda2_base"], 16))
    add("")
    add("[modules]")
    add("# module = header file it is emitted into (relative to recompiler/port).")
    add("# Change the target here to move a whole module group between headers.")
    add("hash = 0x0000000000000000")
    for module in modules:
        add("%-22s = %s" % (module, known_headers.get(module, default_header)))
    add("")
    add("[imports]")
    add("# module!symbol = PLT stub address [data]")
    add("# \"data\" marks a .dimport_ slot holding a guest data address: it gets an")
    add("# ADDR_ define but no PLT stub, because it is not a callable thunk.")
    add("# %d entries: the complete import table of %s (%d data, %d code)."
        % (len(imports), image.name, data_count, len(imports) - data_count))
    add("hash = 0x0000000000000000")
    for item in sorted(imports, key=lambda i: (i.module, i.symbol)):
        add("%-60s = 0x%08X%s" % (item.module + "!" + item.symbol, item.address,
                                  " data" if item.address >= 0xC0000000 else ""))
    add("")
    add("[entries]")
    add("# Guest entry points reached only through indirect dispatch (vtable calls,")
    add("# function-pointer tables): the static scanners cannot prove them.")
    add("# normal      = validated hint")
    add("# force       = hint that bypasses the code-address check in a coverage gap")
    add("# corroborate = external analyzer evidence only; never a discovery hint")
    if preserved_entries:
        add("# Preserved from the previous manifest.")
    add("hash = 0x0000000000000000")
    for address, mode in preserved_entries:
        add("0x%08X = %s" % (address, mode))
    add("")
    add("[hooks]")
    add("# Guest function entries wrapped so the port can attach opt-in host hooks.")
    add("# Without a registered hook, a wrapper calls the original body directly.")
    if preserved_hooks:
        add("# Preserved from the previous manifest.")
    add("hash = 0x0000000000000000")
    for address, mode in preserved_hooks:
        add("0x%08X = %s" % (address, mode))
    add("")
    add("[patches]")
    add("# 0xADDR = instruction, applied when this manifest is used to recompile")
    add("# the image. The instruction is PowerPC assembly (cmpwi, li, stw, b, ...);")
    add("# branch targets are absolute guest addresses.")
    if preserved_patches:
        add("# Preserved from the previous manifest.")
    add("hash = 0x0000000000000000")
    for address, instruction in preserved_patches:
        add("0x%08X = %s" % (address, instruction))
    add("")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines), encoding="utf-8")
    sealed = seal_file(out_path)
    print("Wrote %s: %d imports (%d data), %d entries, %d hooks, %d patches, %d hashed sections"
          % (out_path, len(imports), data_count, len(preserved_entries),
             len(preserved_hooks), len(preserved_patches), sealed))



# CLI


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Generate a manifest from a guest image, and its artifacts from a manifest.")
    sub = parser.add_subparsers(dest="command", required=True)

    emit_parser = sub.add_parser("emit", help="generate address headers and the PLT table")
    emit_parser.add_argument("--manifest", required=True)
    emit_parser.add_argument("--out", required=True)

    list_parser = sub.add_parser("list", help="list manifests, optionally ranked against an image")
    list_parser.add_argument("--manifest-dir", action="append", default=[])
    list_parser.add_argument("--rpx")

    select_parser = sub.add_parser("select", help="print the manifest that describes an image")
    select_parser.add_argument("--rpx", required=True)
    select_parser.add_argument("--manifest-dir", action="append", default=[])
    select_parser.add_argument("--manifest", action="append", default=[])

    verify_parser = sub.add_parser("verify", help="check section hashes and the image identity")
    verify_parser.add_argument("--manifest", required=True)
    verify_parser.add_argument("--rpx")

    seal_parser = sub.add_parser("seal", help="recompute and rewrite the hash lines")
    seal_parser.add_argument("--manifest", required=True)

    show_parser = sub.add_parser("show", help="summarise a manifest")
    show_parser.add_argument("--manifest", required=True)

    scan_parser = sub.add_parser("scan", help="build a manifest base from an image")
    scan_parser.add_argument("--rpx", required=True)
    scan_parser.add_argument("--out", required=True)
    scan_parser.add_argument("--id")
    scan_parser.add_argument("--title")
    scan_parser.add_argument("--title-id")
    scan_parser.add_argument("--title-version")
    scan_parser.add_argument("--app-version")   # default: from app.xml
    scan_parser.add_argument("--region")        # default: from the product code
    scan_parser.add_argument("--product-code")  # default: from meta/meta.xml
    scan_parser.add_argument("--rebrewu")

    args = parser.parse_args(argv)

    if args.command == "list":
        paths = discover_manifests(args.manifest_dir)
        if not paths:
            print("no manifest found; pass --manifest-dir <directory>", file=sys.stderr)
            return 1
        if args.rpx:
            image = Path(args.rpx)
            if not image.is_file():
                print("image not found: %s" % args.rpx, file=sys.stderr)
                return 1
            size = image.stat().st_size
            digest = sha256_file(image)
            for path in paths:
                try:
                    m = load_manifest(path)
                except (OSError, UnicodeError) as exc:
                    print("unreadable  %s  (%s)" % (path, exc))
                    continue
                expected_size = m.identity_u32("image_size") if m.identity.get("image_size") else 0
                expected_hash = m.identity_string("image_sha256").lower()
                if expected_hash and expected_hash == digest:
                    status = "MATCH"
                elif not expected_hash and expected_size and expected_size == size:
                    status = "size-only"
                else:
                    status = "no"
                print("%-9s  %s  %s" % (status, path, manifest_summary(m)))
        else:
            for path in paths:
                try:
                    m = load_manifest(path)
                except (OSError, UnicodeError) as exc:
                    print("unreadable  %s  (%s)" % (path, exc))
                    continue
                print("candidate  %s  %s" % (path, manifest_summary(m)))
        return 0

    if args.command == "select":
        paths = discover_manifests(args.manifest_dir, args.manifest)
        if not paths:
            print("no manifest found; pass --manifest-dir <directory>", file=sys.stderr)
            return 1
        selected = select_manifest(args.rpx, paths)
        print(selected.path)
        print("Selected manifest %s (%s)" % (selected.path, manifest_summary(selected)),
              file=sys.stderr)
        return 0

    if args.command == "emit":
        m = load_manifest(args.manifest)
        if not verify_hashes(m):
            for error in m.errors:
                print("manifest: " + error, file=sys.stderr)
            print("manifest: refusing to emit from an unsealed manifest", file=sys.stderr)
            return 1
        written = emit(m, args.out)
        for path, _ in written:
            print("Wrote %s" % path)
        return 0

    if args.command == "verify":
        m = load_manifest(args.manifest)
        ok = verify_hashes(m)
        for error in m.errors:
            print("manifest: " + error, file=sys.stderr)
        for warning in m.warnings:
            print("manifest: warning: " + warning, file=sys.stderr)
        if args.rpx:
            problems = verify_image(m, args.rpx)
            for problem in problems:
                print("manifest: " + problem, file=sys.stderr)
            ok = ok and not problems
        if not ok:
            return 1
        print("Manifest OK: %s (%d hashed sections, %d imports, %d entries, %d hooks, %d patches)"
              % (m.id, len(m.computed), len(m.imports), len(m.entries), len(m.hooks), len(m.patches)))
        return 0

    if args.command == "seal":
        print("Sealed %s (%d hashed sections)" % (args.manifest, seal_file(args.manifest)))
        return 0

    if args.command == "show":
        m = load_manifest(args.manifest)
        print("Manifest:    %s" % args.manifest)
        print("Id:          %s" % m.id)
        print("Title:       %s %s (%s)" % (m.identity_string("title_id"),
                                            m.identity_string("product_code"),
                                            m.identity_string("region")))
        print("App version: %s" % m.identity_string("app_version"))
        print("Entry point: 0x%08X" % m.identity_u32("entry_point"))
        print("Modules:     %d" % len(m.modules))
        print("Imports:     %d" % len(m.imports))
        corroborating = sum(1 for _, mode in m.entries if mode == "corroborate")
        print("Entries:     %d (%d hints, %d corroborating)" % (len(m.entries), len(m.entries) - corroborating, corroborating))
        print("Hooks:       %d" % len(m.hooks))
        print("Patches:     %d" % len(m.patches))
        print("Sections:    %d hashed" % len(m.computed))
        return 0

    if args.command == "scan":
        if not args.id:
            raise SystemExit("scan needs --id")
        scan(args)
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())

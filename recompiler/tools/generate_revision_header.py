"""Generate runtime/game_revision.h and runtime/game_branch_tables.h from a manifest (address metadata only).

The first header maps the addresses the port code still names by their base (v16) value to the
selected image: every import symbol present in both manifests moves with its stub address,
and the `[globals]` ids move the same way. Every `[globals]` id is also exposed as an
RBREW_GAME_<ID> constant. The second header lists the `[branch_tables]` of the image.
Both generated files are not committed (see .gitignore).
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RELOCATED_GLOBALS = ['heap_alloc_slot', 'heap_alloc_ex_slot', 'heap_free_slot', 'fopen_max_slot']


def sections(text):
    out, name = {}, None
    for line in text.splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        if line.startswith('[') and line.endswith(']'):
            name = line[1:-1]
            out.setdefault(name, {})
        elif name and '=' in line:
            key, value = (part.strip() for part in line.split('=', 1))
            out[name][key] = value
    return out


def load(path):
    parsed = sections(Path(path).read_text(encoding='utf-8'))
    imports = {k: int(v.split()[0], 16) for k, v in parsed.get('imports', {}).items() if '!' in k}
    globals_ = {k: int(v, 16) for k, v in parsed.get('globals', {}).items()}
    revision = int(parsed['identity']['app_version'])
    tables = {int(k, 16): [int(x, 16) for x in v.split()] for k, v in parsed.get('branch_tables', {}).items()}
    return revision, imports, globals_, tables


def render(base, update):
    base_revision, base_imports, base_globals, _ = base
    revision, imports, globals_, _ = update
    pairs = {}
    if revision != base_revision:
        for symbol, address in base_imports.items():
            if symbol in imports and imports[symbol] != address:
                pairs[address] = imports[symbol]
        for name in RELOCATED_GLOBALS:
            if name in base_globals and name in globals_ and base_globals[name] != globals_[name]:
                pairs[base_globals[name]] = globals_[name]
    lines = ['#pragma once', '#include <stdint.h>', '#ifndef RBREW_GAME_REVISION',
             f'#define RBREW_GAME_REVISION {revision}', '#endif',
             '// Generated from the manifest by recompiler/tools/generate_revision_header.py. Address metadata only.',
             'constexpr uint32_t rbrew_game_address(uint32_t address) {']
    if pairs:
        lines.append('    switch (address) {')
        lines += [f'    case 0x{a:08X}u: return 0x{b:08X}u;' for a, b in sorted(pairs.items())]
        lines += ['    default: return address;', '    }']
    else:
        lines.append('    return address;')
    lines.append('}')
    for key, value in globals_.items():
        lines.append(f'constexpr uint32_t RBREW_GAME_{key.upper()} = 0x{value:08X}u;')
    return '\n'.join(lines) + '\n'


def render_tables(update):
    tables = update[3]
    lines = ['#pragma once', '#include <stdint.h>',
             '// Generated from the manifest by recompiler/tools/generate_revision_header.py. Address metadata only.',
             'struct RbrewBranchTable {', '    uint32_t base;', '    unsigned count;', '    const uint32_t* targets;', '};']
    for index, (base, targets) in enumerate(sorted(tables.items())):
        body = ', '.join(f'0x{x:08X}u' for x in targets)
        lines.append(f'inline constexpr uint32_t kRbrewBranchTable{index}[] = {{{body}}};')
    lines.append('inline constexpr RbrewBranchTable kRbrewBranchTables[] = {')
    for index, (base, targets) in enumerate(sorted(tables.items())):
        lines.append(f'    {{0x{base:08X}u, {len(targets)}, kRbrewBranchTable{index}}},')
    lines.append('    {0, 0, nullptr},')
    lines.append('};')
    lines.append(f'inline constexpr unsigned kRbrewBranchTableCount = {len(tables)};')
    return '\n'.join(lines) + '\n'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', required=True, help='manifest of the selected image')
    parser.add_argument('--base', help='manifest of the base revision (default: the v16 manifest next to it)')
    parser.add_argument('--out-dir', default=str(ROOT / 'recompiler/port/runtime'))
    args = parser.parse_args(argv)
    manifest = Path(args.manifest)
    base_path = Path(args.base) if args.base else None
    if base_path is None:
        for candidate in sorted(manifest.parent.glob('*.manifest')):
            if load(candidate)[0] == 16:
                base_path = candidate
                break
    if base_path is None:
        sys.exit('no base (v16) manifest found next to ' + str(manifest))
    update = load(manifest)
    for name, text in (('game_revision.h', render(load(base_path), update)), ('game_branch_tables.h', render_tables(update))):
        out = Path(args.out_dir) / name
        if not out.exists() or out.read_text(encoding='utf-8') != text:
            out.write_text(text, encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()

# Manifest per-image guest data

> A manifest is just a description of the Game version

> The format is similar to [TOMTL](https://en.wikipedia.org/wiki/TOML)

The address headers (`port/os/**/*_addrs.h`) and the PLT stub table
(`port/os/gambit_plt_auto.cpp`) are **generated from the manifest** and must not be
edited by hand.

## Automatic selection

Every `Manifest/*.manifest` is a candidate. The recompiler hashes the RPX and
picks the manifest whose `[identity]` matches it, so nothing hardcodes a
filename and a new version or region only needs a new manifest file:

```bash
rebrewu recompile --rpx /path/to/code/Gambit.rpx --output build/generated/Gambit
rebrewu manifest list --rpx /path/to/code/Gambit.rpx   # what matches this image?
rebrewu manifest show --rpx /path/to/code/Gambit.rpx   # selected manifest
```

`--manifest <file>` forces one manifest, and `--manifest-dir <dir>` adds a
directory to the discovery roots (the working directory, its parents and the
executable directory are always searched). The CMake build accepts
`-DRBREW_RPX=<image>` (or `-DRBREW_MANIFEST=<file>`) and performs the same
selection before generating the address headers.

## Sections

| Section | Hashed | Purpose |
|---|---|---|
| `[manifest]` | no | metadata: id, format, notes. Not covered by any hash. |
| `[identity]` | yes | title id, version, region, image size, image SHA-256, entry point. |
| `[modules]` | yes | which header file each OS module's addresses are emitted into. |
| `[imports]` | yes | `module!symbol = address [data]`, the PLT stub map. The `data` marker means a `.dimport_` slot holding a guest data address: it gets an `ADDR_` define but no PLT stub. |
| `[entries]` | yes | guest entry points. `normal`/`force` are discovery hints; `force` bypasses the code-address validity check. `corroborate` is external analyzer evidence, used only when the RPX also proves a reference and a non-fallthrough separator. |
| `[hooks]` | yes | `address = hook`, guest function entries that get an opt-in host-hook wrapper. |
| `[patches]` | yes | `address = instruction`, applied to the guest code before recompilation. |

## The per-section hash

Each hashed section carries its own `hash = 0x...` line. The value is FNV-1a 64 over
that **section's own key/value pairs only**, with:

- pairs sorted by key, so line order never matters,
- values normalized (`0x02d09a00`, `0x02D09A00` and `0x02D09A00u` are identical),
- comments and blank lines stripped,
- the `hash` line itself excluded.

A section can opt out of verification by writing `hash = Undefined`
(case-insensitive). Such a section is ignored by `verify`, `emit` and `seal`,
which preserves the line as written.

Consequences, and this is the point:

- adding a comment, blank line, or reordering entries changes nothing;
- **adding a new section** (a `[quirks]` table, notes, a second module group) changes
  nothing in the existing sections' hashes;
- adding one import changes only `[imports]`, and the mismatch names the section.

So a contributor can extend a manifest without invalidating it, and a real data
change is reported precisely instead of silently drifting.

## Adding a version or a region

1. Copy the closest manifest and rename it, e.g. `Gambit_usa_v16.manifest`. No
   registration step is needed: every `*.manifest` in `Manifest/` is discovered
   automatically and selected by the image's `[identity]`.
2. Update `[identity]` (`title_id`, `title_version`, `region`, `image_size`,
   `image_sha256`, `entry_point`, SDA bases) from that image's `app.xml`/`cos.xml`.
3. Refresh `[imports]`, `[entries]` and `[hooks]` — imports are derived from the
   image's ELF symbol and relocation tables; entries and hooks are address
   metadata from an execution log or analyzer. Keep hook addresses in `[hooks]`
   so Setup needs only the manifest.
4. `rebrewu manifest seal <file>` then
   `rebrewu manifest emit <file> --emit-dir recompiler/port`.
5. Build.

No C++ change is required for either step, which is the whole point of the split.

## Entries and hooks

`[entries]` keeps every guest entry address in the manifest, so there is no
second boundary-address file to maintain:

```ini
[entries]
hash = 0x...
0x02043938 = normal       # validated discovery hint
0x028C1C60 = force        # hint in a coverage gap
0x0200C6F0 = corroborate  # analyzer evidence, checked against the RPX
```

`[hooks]` lists the guest functions that receive an opt-in host-hook wrapper.
Without a registered host hook, the wrapper calls the original guest body:

```ini
[hooks]
hash = 0x...
0x026227C8 = hook  # player motion object constructor
```

## Patches

`[patches]` replaces instructions in the guest image before the recompiler
analyzes it, so the generated C++ and the optional PPC fallback image both
contain the patched code. Each entry is a 4-byte-aligned guest address and the
modified PowerPC instruction:

```ini
[patches]
hash = 0x...
# Disable a comparison used by a gameplay check.
0x0261C918 = cmpwi r6, 1
# Force a function to return immediately.
0x0261CA10 = blr
```

The instruction is assembled by the recompiler. Common integer, branch,
load/store and floating-point forms are supported (`li`, `lis`, `mr`, `nop`,
`blr`, `b`/`bl`, `beq`/`bne`, `cmpwi`, `lwz`/`stw`, `rlwinm`, ...). Branch
targets are written as absolute guest addresses. Relative `b`/`bc` targets must
fit their displacement field; use `ba`/`bca` for an absolute branch, whose
target must fit the sign-extended LI/BD field. For anything the assembler does
not cover, `.word 0x...` inserts a raw instruction word.

An invalid patch fails the build with its address and operand error; nothing is
written partially. Run `rebrewu manifest verify --manifest <file>` to assemble
all patches without recompiling the full image.

## Why the image hash is checked

The generated PLT table registers a stub for every address. If the addresses came
from a different build, those stubs land on the wrong functions and the port
returns wrong values **silently** instead of failing. `[identity]` exists so a
mismatch is an explicit error at build time.

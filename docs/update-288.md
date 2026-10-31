# Experimental EUR revision 288

Separate experimental build for the locally supplied EUR update RPX with SHA-256
`06684edefe90fea2de20430eb0e68500995980fef154caa88e4dcb59cdb7cd65`. The installer checks exact
executable hashes instead of relying on the v16/288 labels in the metadata. Its import addresses
come from `Manifest/Gambit_v288.manifest`, which the build selects for this image.

## Status and limits

- Experimental: expect crashes, graphical bugs and frame-rate drops. Tested on one PC (NVIDIA GPU) only.
- The game starts, completes the tutorial and reaches the plaza.
- The update's content files are copied into `Game/content`; `Game/Splatoon-v288.exe` is created next to the base executable.
- Update content entries are copied over a copy of the base content in `build/merged-v288`, then installed into `Game/content`.
- No Nintendo files are added to the repository.
- Plaza game speed is regulated on revision 288 by `plaza_frame_rate.h` (PR-5): a measurement
  run gave about 59.9 logic steps/s between 35 and 60 FPS, with brief dips during loading.
- A message-broadcast loop guard (PR-6) fixes a freeze seen when touching the GamePad screen;
  confirmed in game.
- Tutorial enemy ink stamps are no longer dropped while their shaders compile on a cold pipeline
  cache (PR-7).

## Quick start

You need the base v16 dump and the update 288 dump (folders `code`, `content`, `meta`), Visual
Studio 2022 C++ tools, Python, and about 10 GB free. Do not use a save you cannot afford to reset.

1. Extract the zip.
2. Run `Setup.bat` and select the base dump.
3. After the base install, `Setup.bat` opens the update dump picker: select the revision 288 dump.
4. Start `Game\Splatoon-v288.exe`.

You can also run `scripts\Install-Update.ps1` on its own at any time.

## Reporting a problem

Say which step failed, your GPU, and the exact error text. Do not attach game content, dumps,
saves or generated files.

## Build

Windows, Visual Studio 2022 (or 2026) C++ tools and Python are required. Reuse a prepared upstream
checkout's libraries by giving its directory as `DependencyRoot`.

```powershell
.\scripts\Build-Update.ps1 -BaseDump 'D:\Owned\Splatoon Base' -UpdateDump 'D:\Owned\Splatoon Update' -DependencyRoot 'D:\SplatoonPort'
```

Both input hashes are checked before translation. Base content is copied into
`build/merged-v288`, then update content overlays it. Original dumps are not changed. The update's
content files are then copied into `Game/content`, and the runtime is packaged as
`Game/Splatoon-v288.exe`.

Use `-Optimize` to compile optimized guest code. This disables verbose runtime diagnostics by
default; add `-Diagnostics` when needed. Start `Game/Splatoon-v288.exe` directly.

## Validation

```powershell
py -3.12 recompiler/tools/test_revision_profile.py
```

The profile generator is `recompiler/tools/generate_revision_header.py`. `runtime/game_revision.h`
is reproducible from `profiles/agmp-v288.json`. The test also checks that every import shared by
the two manifests sits where that map sends it, and that the 288 manifest describes this RPX.

`compare_revisions.py` can regenerate a private comparison report from two owned RPX files using
`--base`, `--update`, `--root` and `--recompiler`. Normalized signatures only suggest candidates;
RebrewU validates references and separators. `--discover-referenced-entries` opts into
corroborated reference discovery; the base build default is unchanged. The compiler test option
`REBREWU_BUILD_PROFILE_TESTS=ON` adds reference-discovery and guarded inline-table
acceptance/rejection tests.

## Source and private outputs

The update contains authored port code and address metadata only. `build/` and `Game/` are
ignored. Never commit RPX files, generated game C++, merged content, saves or diagnostics.
Existing third-party license notices are preserved.

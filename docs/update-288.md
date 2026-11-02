# Experimental EUR revision 288

Experimental build for the locally supplied EUR update RPX with SHA-256
`06684edefe90fea2de20430eb0e68500995980fef154caa88e4dcb59cdb7cd65`. Setup selects the
matching `Manifest/Gambit_v288.manifest` from the merged ROM's executable, so the build uses that
image's import addresses. The update is installed as a complete merged ROM, not as a second
executable next to the base game.

## Status and limits

- Experimental: expect crashes, graphical bugs and frame-rate drops. Tested on one PC (NVIDIA GPU) only.
- The game starts, completes the tutorial and reaches the plaza.
- The merged ROM contains the base v16 content overlaid by the update's content.
- No Nintendo files are added to the repository.
- Plaza game speed is regulated on revision 288 by `plaza_frame_rate.h` (PR-5): a measurement
  run gave about 59.9 logic steps/s between 35 and 60 FPS, with brief dips during loading.
- A message-broadcast loop guard (PR-6) fixes a freeze seen when touching the GamePad screen;
  confirmed in game.
- Tutorial enemy ink stamps are no longer dropped while their shaders compile on a cold pipeline
  cache (PR-7).

## Quick start

You need the base v16 dump and the update 288 dump (folders `code`, `content`, `meta`), Python 3,
the normal setup tools, and about 10 GB free. Do not use a save you cannot afford to reset.

1. Extract the zip.
2. Merge the two dumps from the repository folder:
   ```bash
   python scripts/update_combinator.py --base "D:/Owned/Splatoon v16" --update "D:/Owned/Splatoon 288" --output "D:/Owned/Splatoon v288 combined"
   ```
   On Linux use `python3`. Add `--force` to replace an existing merged folder.
3. Run `Setup.bat` and select the merged folder. On Linux run
   `./setup.sh --dump "D:/Owned/Splatoon v288 combined"`.
4. Start `Game\Splatoon.exe` on Windows or `./Game/Splatoon` on Linux.

The merged folder is a normal Wii U dump structure, so Setup builds it directly. The original
dumps are not modified. Keep the merged folder private; it contains Nintendo content.

## Reporting a problem

Say which step failed, your GPU, and the exact error text. Do not attach game content, dumps,
saves or generated files.

## Build

Windows needs Visual Studio 2022 (or 2026) C++ tools; Linux needs the packages listed in the README.
Both need Python 3. Update-combinator uses only the Python standard library and runs on either OS.

```bash
python scripts/update_combinator.py \
  --base "D:/Owned/Splatoon v16" \
  --update "D:/Owned/Splatoon 288" \
  --output "D:/Owned/Splatoon v288 combined"
```

The tool copies the base dump first and overlays the update dump. Original dumps are not changed.
It refuses to write into a non-empty folder unless `--force` is given. Then run Setup on the merged
folder; Setup reads the revision from the selected manifest and compiles the matching game.

## Validation

```bash
python recompiler/tools/test_revision_profile.py
```

The profile generator is `recompiler/tools/generate_revision_header.py`. `runtime/game_revision.h`
is generated from the manifests (`Manifest/*.manifest`) at configure time and is not committed. The
test also checks that every import shared by the two manifests sits where that map sends it, and
that the 288 manifest describes this RPX.

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

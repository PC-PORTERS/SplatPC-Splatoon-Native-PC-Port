# Linux

Status: **partially tested.** On Ubuntu 24.04 (WSL) the game builds, starts, signs up on the public server and creates a character. Gameplay on real hardware (Vulkan on a GPU) is **not tested yet**: the WSL test only had software rendering, where the tutorial loads forever. Reports from real Linux machines are welcome.

## What you need

- 64-bit Linux, a Vulkan 1.3 GPU with up-to-date drivers, 16 GB RAM (32 GB recommended), about 40 GB free while building.
- A build toolchain (CMake, Ninja, a C++20 compiler, clang), Python 3.10+ (`python3-tk` for the sign-in window), `git`.
- **Your own** Splatoon dump (`code`, `content`, `meta`); with the 288 update, merge it first with `scripts/update_combinator.py` (see [update-288.md](update-288.md)).

## Build

```bash
./setup.sh --dump "/path/to/dump"     # builds the recompiler, recompiles your copy of the game, makes Game/
./build.sh                            # later: incremental rebuild (setup.sh must have run once)
```

`setup.sh` can be re-run: finished steps are skipped, and your save and shader cache are kept. `--clean` rebuilds everything. The first build takes about 20 minutes on a fast machine.

## Play

```bash
./Play-Online.sh                      # sign-in window (pseudo, language), then the online game
./Play-Online.sh --cli --pseudo Ana   # same, in the terminal
./Play-Online.sh --offline            # no server
```

It does what `Play-Online.bat` does on Windows: version check against the server, sign-up on first launch, friends relay, then the game. It needs Python 3.10+ (`sudo apt install python3 python3-tk`).

## Builder (graphical)

The AppImage / binary from the Releases page does the same steps with a wizard, see [BUILDER.md](BUILDER.md). Its **Play** button goes through `Splatoon.sh`, which calls the login script when online is on.

## Known limits

- **AppImage and online:** the AppImage folder is read-only, but the game needs to write `settings.ini` and `online_identity.ini` next to the program. Online play works from a normal folder (the build output or the repository's `Game/`), not from inside the AppImage.
- **Software rendering (lavapipe, WSL):** the game starts but is far too slow to play.
- **Tests:** the Python suite passes on Linux; the Go tests pass except `TestCreateAppImage` when `appimagetool` is missing.

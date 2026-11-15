# SplatPC Builder

The Builder is a small graphical wizard (Go + Fyne, in `Builder/`) that does what `Setup.bat` / `setup.sh` do: it checks the tools, takes **your** dump, builds the game and writes its settings. It ships no Nintendo content.

## Steps

1. **Prerequisites** – checks the compilers and tools and offers the install command for what is missing (`SplatPC-Builder --check` prints the same report in a terminal).
2. **ROMs** – your base dump (`code`, `content`, `meta`), then **Add update or mod...** for an official update (e.g. 288, matched with its manifest) or a resource mod. Everything is layered into a merged folder; your files are never changed.
3. **Configuration** – pseudo, language, region, graphics preset, resolution, output folder, **online** (on by default).
4. **Build** – recompiles the game. Re-running keeps your save and shader cache.
5. **Done** – summary and a **Play** button.

## Online in the Builder

- The **Online** box is on by default and the server field is pre-filled: the repository's `scripts/server.txt` / `scripts/server.public.txt` if found, else the public server.
- **Launchers written by the build:** `Launch_Splatoon.bat` (Windows) and `Splatoon.sh` (Linux). With online on and the game inside the repository's `Game/` folder, both go through the login script (`scripts/Splatoon-Login.ps1` or `scripts/splatoon_login.py`): version check, sign-up, pseudo, friends relay. Elsewhere, or with `--offline`, the game starts directly. The pseudo is stripped of characters that could break out of the `.bat` / shell line.
- **Play** in the wizard uses the same launcher, so a player gets the same behaviour from the wizard and from the shortcut.

## Build the Builder

```bash
cd Builder
go vet ./...
go test ./...          # settings, engine, embedded, prereq, roms
go build .             # the root package needs cgo and a C compiler (Fyne)
```

`go generate` bundles the repository files the standalone Builder embeds (`scripts/bundle_builder_assets.py`). The root package (Fyne) only builds with a C compiler: on Windows install MinGW/gcc, on Linux the OpenGL and X11 development packages.

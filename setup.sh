#!/usr/bin/env bash
#
# Splatoon 1 PC Port - one-click Linux setup (CMake flow).
#
# Builds the recompiler, recompiles YOUR OWN copy of the game into a native
# Linux program, and creates a ready-to-play game folder. Nothing from Nintendo
# is downloaded or shipped by this project: the game files always come from the
# dump you pass. Run it from a terminal:  ./setup.sh --dump "/path/to/dump"
#
# Re-running is safe: finished steps are skipped, your save and shader cache are
# kept. Use --clean to rebuild everything from scratch. Windows users keep
# using Setup.bat instead.

set -euo pipefail

# ---------------------------------------------------------------------------
# Fixed inputs
# ---------------------------------------------------------------------------
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
RECOMPILER="$ROOT/recompiler"
WORK="$ROOT/build"
DOWNLOADS="$WORK/downloads"
DEPS="$WORK/deps"

# Every Manifest/*.manifest describes one supported image; the RPX selects it.
MANIFEST_DIR="$ROOT/Manifest"
MINIMUM_FUNCTION_COUNT=96300

# Open-source build dependencies (official upstream releases).
SDL_URL='https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.zip'
GLSLANG_URL='https://github.com/KhronosGroup/glslang/archive/refs/tags/16.4.0.tar.gz'
VOLK_URL='https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz'

# Cemu's shared fonts are Noto Sans replacements under SIL OFL 1.1, not
# Nintendo's fonts. Pin the exact upstream bytes for unattended setup.
FONT_BASE_URL='https://raw.githubusercontent.com/cemu-project/Cemu/cc6fea125a5d4f559c662d13c5f4af0b6dd0876f/bin/resources/sharedFonts'
FONT_NAMES=(CafeCn.ttf CafeKr.ttf CafeStd.ttf CafeTw.ttf)
FONT_SHA256=(
    6afd751a9bb76bc788efb50a138c6f3765624ee84d8d8203be04d871ade40a6a
    c3c326f6ce1899e391dc4996b10a0c30558c5b8f36dfbaa418976862b3e2dffb
    59e2494727b8e7a7bbf48c8749f2dbc07d438c9fabeaa9847a08ec690f9888a1
    c8f9e16345dbc3b6e165fe12435486a748e730ebe6002b5d713efe73d171d51c
)
MII_NAMES=(FFLResHigh.dat FFLResHighLG.dat FFLResMiddle.dat FFLResMiddleLG.dat)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
STEP_NUMBER=0
TOTAL_START=$(date +%s)
declare -A STAGE_TIMES=()

usage() {
    cat <<'EOF'
Usage: ./setup.sh [options]

  --dump PATH       Wii U dump folder (contains code/, content/, meta/)
  --output PATH     install folder for the playable game (default: ./Game)
  --mlc PATH        optional local system-resource source; never prompted
  --clean           delete previous build outputs first
  --non-interactive never prompt; fail if input would be required
  --jobs N          parallel compile jobs (default: CPU/memory based)
  -h, --help        show this help
EOF
}

info() { printf '      %s\n' "$*"; }
good()  { printf '      %s\n' "$*"; }
warn()  { printf '      %s\n' "$*" >&2; }
fail()  {
    printf '\nSETUP FAILED: %s\n' "$*" >&2
    printf 'If you cannot fix this, open an issue on GitHub and paste this terminal''s text.\n' >&2
    exit 1
}
step() {
    STEP_NUMBER=$((STEP_NUMBER + 1))
    printf '\n[%d/8] %s\n' "$STEP_NUMBER" "$*"
    STAGE_START=$(date +%s)
}
stage_done() { STAGE_TIMES["$1"]=$(( $(date +%s) - STAGE_START )); }
check_exit() { [ "$1" -eq 0 ] || fail "$2 (exit code $1)."; }

sha256() { sha256sum -- "$1" | awk '{print $1}'; }

# Prints the manifest in Manifest/ whose [identity] describes this image.
select_manifest() { # <rpx>
    python3 "$RECOMPILER/tools/plt_resolver.py" select \
        --rpx "$1" --manifest-dir "$MANIFEST_DIR"
}

download() { # <url> <destination>
    local url="$1" dest="$2"
    [ -f "$dest" ] && return 0
    info "Downloading $(basename "$dest") ..."
    curl -fL --retry 3 --retry-delay 2 --connect-timeout 20 -o "$dest.part" "$url" \
        || fail "Could not download $url (internet/DNS/firewall?). Run setup.sh again to retry."
    mv -f "$dest.part" "$dest"
}

ensure_folder() { mkdir -p -- "$1"; }

# Locate a CMake package config file (<name>Config.cmake) in the usual prefixes.
find_package_config() { # <name> -> prints directory, fails if absent
    local name="$1" dir
    for dir in \
        "$WORK/deps/$name/lib/cmake/$name" \
        "$WORK/deps/$name/share/cmake/$name" \
        "/usr/lib/cmake/$name" \
        "/usr/lib64/cmake/$name" \
        "/usr/share/cmake/$name" \
        "/usr/local/lib/cmake/$name" \
        "/usr/local/share/cmake/$name" \
        "/usr/lib/x86_64-linux-gnu/cmake/$name"; do
        if [ -f "$dir/${name}Config.cmake" ] || [ -f "$dir/${name}-config.cmake" ]; then
            printf '%s\n' "$dir"
            return 0
        fi
    done
    return 1
}

# Search game/system resource files under known roots; prints the directory
# that contains every requested name, fails if incomplete anywhere.
find_system_resources() { # <relative/dir> <name>...
    local rel="$1"
    shift
    local roots=() start parent base suffix dir ok name
    roots+=("$OUTPUT/mlc01")
    [ -n "$MLC" ] && roots+=("$MLC")
    [ -n "${RBREW_SYSTEM_MLC_DIR:-}" ] && roots+=("$RBREW_SYSTEM_MLC_DIR")
    [ -n "${RBREW_MLC_DIR:-}" ] && roots+=("$RBREW_MLC_DIR")
    # Locate an MLC or dumped wiiu-system/mlc beside the game/repository.
    for start in "$GAME_DUMP" "$ROOT"; do
        parent="$start"
        for _ in 1 2 3 4 5; do
            roots+=("$parent")
            [ "$parent" = "/" ] && break
            parent="$(dirname -- "$parent")"
        done
    done
    # Cemu's default Linux locations, plus a configured custom MLC.
    local xdg_data="${XDG_DATA_HOME:-$HOME/.local/share}"
    roots+=("$xdg_data/Cemu" "$HOME/.config/Cemu" "$HOME/.cemu")
    local settings="$HOME/.config/Cemu/settings.xml" custom=""
    if [ -f "$settings" ]; then
        custom="$(sed -n 's:.*<mlc_path>\(.*\)</mlc_path>.*:\1:p' "$settings" | head -n 1)"
        if [ -n "$custom" ]; then
            case "$custom" in /*) ;; *) custom="$HOME/.config/Cemu/$custom" ;; esac
            roots+=("$custom")
        fi
    fi
    for base in "${roots[@]}"; do
        for suffix in "" "mlc01" "mlc" "wiiu-system/mlc"; do
            dir="$base${suffix:+/$suffix}/$rel"
            ok=1
            for name in "$@"; do
                if [ ! -s "$dir/$name" ]; then ok=0; break; fi
            done
            if [ "$ok" = 1 ]; then
                printf '%s\n' "$dir"
                return 0
            fi
        done
    done
    return 1
}

copy_resource_files() { # <from> <to> <name>...
    local from="$1" to="$2"
    shift 2
    ensure_folder "$to"
    local name
    for name in "$@"; do
        if [ "$(readlink -f -- "$from/$name")" != "$(readlink -f -- "$to/$name")" ]; then
            cp -f -- "$from/$name" "$to/$name"
        fi
    done
}

initialize_wiiu_folders() { # <game folder>
    local folder
    for folder in \
        save/common \
        mlc01/sys \
        mlc01/usr/title/00050000 \
        mlc01/usr/title/0005000c \
        mlc01/usr/title/0005000e \
        mlc01/usr/save/00050000 \
        mlc01/usr/boss \
        mlc01/usr/save/00050010/1004a000/user/common/db \
        mlc01/usr/save/00050010/1004a100/user/common/db \
        mlc01/usr/save/00050010/1004a200/user/common/db \
        mlc01/sys/title/0005001b/10042400/content \
        mlc01/sys/title/0005001b/10056000/content \
        mlc01/sys/title/0005001b/1005c000/content; do
        ensure_folder "$1/$folder"
    done
}

prepare_wiiu_resources() {
    initialize_wiiu_folders "$OUTPUT"

    # Shared fonts -------------------------------------------------------
    local font_rel='sys/title/0005001b/10042400/content'
    local font_dest="$OUTPUT/mlc01/$font_rel"
    local font_src=""
    font_src="$(find_system_resources "$font_rel" "${FONT_NAMES[@]}")" || true
    if [ -n "$font_src" ]; then
        copy_resource_files "$font_src" "$font_dest" "${FONT_NAMES[@]}"
        good "Shared fonts ready: $font_src"
    else
        info 'Installing the same Noto replacement fonts supplied with Cemu ...'
        local font_cache="$DOWNLOADS/shared-fonts"
        ensure_folder "$font_cache"
        local i file
        for i in "${!FONT_NAMES[@]}"; do
            file="$font_cache/${FONT_NAMES[$i]}"
            download "$FONT_BASE_URL/${FONT_NAMES[$i]}" "$file"
            if [ "$(sha256 "$file")" != "${FONT_SHA256[$i]}" ]; then
                rm -f -- "$file"
                fail "The replacement font ${FONT_NAMES[$i]} failed its integrity check. Run setup.sh again to retry."
            fi
        done
        copy_resource_files "$font_cache" "$font_dest" "${FONT_NAMES[@]}"
        ensure_folder "$OUTPUT/licenses"
        cp -f -- "$ROOT/LICENSES/cemu-shared-fonts-OFL-1.1.txt" "$OUTPUT/licenses/"
        good 'Noto replacement fonts installed.'
    fi

    # Mii graphics (optional; never invented) ----------------------------
    local mii_rel='sys/title/0005001b/10056000/content'
    local mii_src=""
    mii_src="$(find_system_resources "$mii_rel" "${MII_NAMES[@]}")" || true
    if [ -n "$mii_src" ]; then
        copy_resource_files "$mii_src" "$OUTPUT/mlc01/$mii_rel" "${MII_NAMES[@]}"
        good "Local Mii graphics found automatically: $mii_src"
    else
        warn 'No local Mii graphics resources found; some Mii graphics may be unavailable.'
    fi
    good 'Wii U folders initialized automatically; no saves or accounts imported.'
}

generated_module_complete() { # <generated dir>
    local dir="$1" name registered
    for name in Gambit.h Gambit_data.cpp Gambit_register.cpp; do
        [ -f "$dir/$name" ] || return 1
    done
    registered="$(grep -c '^\s*rbrew_register_func(' "$dir/Gambit_register.cpp" || true)"
    [ "$registered" -ge "$MINIMUM_FUNCTION_COUNT" ]
}

# ---------------------------------------------------------------------------
# Arguments
# ---------------------------------------------------------------------------
GAME_DUMP=""
OUTPUT=""
MLC=""
CLEAN=0
NONINTERACTIVE=0
JOBS="${JOBS:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        --dump|-GameDump)       GAME_DUMP="${2:-}"; shift 2 ;;
        --output|-Output)       OUTPUT="${2:-}"; shift 2 ;;
        --mlc|-Mlc)             MLC="${2:-}"; shift 2 ;;
        --clean|-Clean)         CLEAN=1; shift ;;
        --non-interactive)      NONINTERACTIVE=1; shift ;;
        --jobs|-Jobs)           JOBS="${2:-}"; shift 2 ;;
        -h|--help)              usage; exit 0 ;;
        *)                      usage >&2; fail "Unknown option: $1" ;;
    esac
done

printf '============================================================\n'
printf '   Splatoon 1 PC Port - Linux setup (CMake)\n'
printf '   Uses only YOUR OWN game files.\n'
printf '============================================================\n'

# ---------------------------------------------------------------------------
step 'Checking the tools needed to build'
for tool in cmake python3 curl sha256sum tar; do
    command -v "$tool" >/dev/null 2>&1 || fail "'$tool' is missing. Install it with your package manager and run setup.sh again."
done
command -v g++ >/dev/null 2>&1 || command -v clang++ >/dev/null 2>&1 \
    || fail 'No C++ compiler found (install gcc or clang) and run setup.sh again.'

CMAKE_VERSION="$(cmake --version | head -n 1 | awk '{print $3}')"
[ "$(printf '%s\n' '3.20' "$CMAKE_VERSION" | sort -V | head -n 1)" = '3.20' ] \
    || fail "CMake $CMAKE_VERSION is too old; 3.20 or newer is required."
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 12) else 1)' \
    || fail "Python $(python3 --version | awk '{print $2}') is too old; 3.12 or newer is required (tar file filters)."

GENERATOR=""
if command -v ninja >/dev/null 2>&1; then
    GENERATOR='Ninja'
else
    GENERATOR='Unix Makefiles'
    warn 'ninja not found; falling back to Make. Installing ninja speeds up the build.'
fi

# Libraries the game links from the distribution.
if ! pkg-config --exists zlib 2>/dev/null && [ ! -f /usr/include/zlib.h ] && [ ! -f /usr/local/include/zlib.h ]; then
    fail 'zlib development headers not found (install zlib / zlib-devel).'
fi
if ! pkg-config --exists libcurl 2>/dev/null && [ ! -f /usr/include/curl/curl.h ] && [ ! -f /usr/local/include/curl/curl.h ]; then
    fail 'libcurl development headers not found (install libcurl / curl-devel).'
fi
if ! find_package_config VulkanHeaders >/dev/null; then
    fail 'CMake package "VulkanHeaders" not found (install vulkan-headers / Vulkan-Headers, e.g. "sudo pacman -S vulkan-headers").'
fi
if [ ! -f /usr/include/X11/Xlib.h ] && [ ! -f /usr/include/wayland-client.h ]; then
    warn 'No X11/Wayland development headers found; SDL2 may build without a window backend and the game may not open a window.'
fi

good "CMake $CMAKE_VERSION · generator: $GENERATOR · Python $(python3 --version | awk '{print $2}')"
good "Compiler: $( ${CXX:-} --version 2>/dev/null | head -n 1 || (g++ --version 2>/dev/null || clang++ --version 2>/dev/null) | head -n 1 )"

# Parallel jobs: default to the CPU count, capped so one job cannot exhaust RAM.
if [ -z "$JOBS" ]; then
    JOBS="$(nproc 2>/dev/null || echo 4)"
    avail_kb="$(awk '/MemAvailable/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
    mem_jobs=$(( avail_kb / (3 * 1024 * 1024) ))
    [ "$mem_jobs" -lt 1 ] && mem_jobs=1
    [ "$JOBS" -gt "$mem_jobs" ] && JOBS="$mem_jobs"
fi
info "Compile jobs: $JOBS (override with --jobs N)"

if [ "$CLEAN" = 1 ] && [ -d "$WORK" ]; then
    info 'Removing previous build outputs (--clean) ...'
    rm -rf -- "$WORK"
fi
ensure_folder "$DOWNLOADS"
stage_done '1. Toolchain & environment check'

# ---------------------------------------------------------------------------
step 'Finding your game files'
if [ -z "$GAME_DUMP" ]; then
    # Check known/common locations automatically before prompting.
    local_candidate=""
    for local_candidate in \
        "$HOME"/Downloads/* \
        "$ROOT/dump" "$ROOT/game-dump"; do
        if [ -f "$local_candidate/code/Gambit.rpx" ] \
            && select_manifest "$local_candidate/code/Gambit.rpx" >/dev/null 2>&1; then
            GAME_DUMP="$local_candidate"
            good "Automatically detected valid game dump: $GAME_DUMP"
            break
        fi
    done
fi
if [ -z "$GAME_DUMP" ]; then
    if [ "$NONINTERACTIVE" = 1 ] || [ ! -t 0 ]; then
        fail 'No game dump given. Pass --dump "/path/to/Splatoon dump".'
    fi
    info 'Select the folder of your Splatoon dump. It must contain the folders code, content and meta.'
    printf '      Dump folder: '
    IFS= read -r GAME_DUMP || fail 'No game folder was selected.'
fi
GAME_DUMP="${GAME_DUMP%\"}"; GAME_DUMP="${GAME_DUMP#\"}"
GAME_DUMP="$(readlink -f -- "$GAME_DUMP")" || true
[ -n "$GAME_DUMP" ] && [ -d "$GAME_DUMP" ] || fail "The game dump folder does not exist: $GAME_DUMP"
for folder in code content meta; do
    [ -d "$GAME_DUMP/$folder" ] \
        || fail "Your dump is missing the $folder folder. Select the folder that contains code, content and meta; its name does not matter."
done
rpx="$GAME_DUMP/code/Gambit.rpx"
[ -f "$rpx" ] || fail 'Your dump is missing code/Gambit.rpx.'
[ -f "$GAME_DUMP/meta/meta.xml" ] || fail 'Your dump is missing meta/meta.xml.'
[ -d "$MANIFEST_DIR" ] || fail "The Manifest folder is missing: $MANIFEST_DIR. Extract the complete repository, including Manifest, and retry."
info 'Checking the game version (this takes a few seconds) ...'
rpx_hash="$(sha256 "$rpx")"
SELECTED_MANIFEST="$(select_manifest "$rpx")" || {
    fail "Unsupported game version. None of the manifests in $MANIFEST_DIR describes your code/Gambit.rpx (SHA-256 $rpx_hash). Add a manifest for this image, or use a supported dump."
}
GAME_REVISION="$(awk -F= '/^[[:space:]]*app_version[[:space:]]*=/{gsub(/[[:space:]]|#.*/, "", $2); print $2; exit}' "$SELECTED_MANIFEST")"
[ -n "$GAME_REVISION" ] || fail "The selected manifest has no app_version: $SELECTED_MANIFEST"
good "Matched manifest: $SELECTED_MANIFEST (title version $GAME_REVISION)"

if [ -z "$OUTPUT" ]; then OUTPUT="$ROOT/Game"; fi
OUTPUT="$(readlink -f -- "$(dirname -- "$OUTPUT")")/$(basename -- "$OUTPUT")"
info "The game will be installed to: $OUTPUT"
prepare_wiiu_resources
stage_done '2. Game validation & resources'

# ---------------------------------------------------------------------------
step 'Building the window and shader-compiler libraries (SDL2, glslang)'
SDL_ZIP="$DOWNLOADS/SDL2-2.32.10.zip"
GLSLANG_ARCHIVE="$DOWNLOADS/glslang-16.4.0.tar.gz"
download "$SDL_URL" "$SDL_ZIP"
download "$GLSLANG_URL" "$GLSLANG_ARCHIVE"
SDL_SOURCE="$DOWNLOADS/SDL2-2.32.10"
if [ ! -d "$SDL_SOURCE" ]; then
    info 'Extracting SDL2 source archive ...'
    python3 -c 'import zipfile, sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$SDL_ZIP" "$DOWNLOADS"
fi
python3 "$RECOMPILER/tools/build_vulkan_dependencies.py" \
    --sdl-source "$SDL_SOURCE" \
    --glslang-archive "$GLSLANG_ARCHIVE" \
    --build-root "$WORK/deps-build" \
    --prefix "$DEPS" \
    --generator "$GENERATOR" \
    --cmake "$(command -v cmake)" \
    --cache-dir "$WORK/cache/deps" \
    || fail 'Building SDL2/glslang'
good 'Libraries ready.'
stage_done '3. SDL2 & glslang (Vulkan dependency profile)'

# ---------------------------------------------------------------------------
step 'Installing the Vulkan meta-loader (volk)'
VOLK_CONFIG=""
if VOLK_CONFIG="$(find_package_config volk)"; then
    good "volk package found: $VOLK_CONFIG"
else
    VOLK_TARBALL="$DOWNLOADS/volk-1.4.363.tar.gz"
    download "$VOLK_URL" "$VOLK_TARBALL"
    VOLK_SOURCE="$WORK/volk-source"
    if [ ! -f "$VOLK_SOURCE/CMakeLists.txt" ]; then
        rm -rf -- "$VOLK_SOURCE"
        ensure_folder "$VOLK_SOURCE"
        tar -xzf "$VOLK_TARBALL" -C "$VOLK_SOURCE" --strip-components=1
    fi
    cmake -S "$VOLK_SOURCE" -B "$WORK/volk-build" -G "$GENERATOR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DVOLK_INSTALL=ON \
        -DCMAKE_INSTALL_PREFIX="$WORK/deps/volk" \
        || fail 'Configuring volk'
    cmake --build "$WORK/volk-build" --target install --parallel "$JOBS" \
        || fail 'Building volk'
    VOLK_CONFIG="$(find_package_config volk)" || fail 'volk did not install a CMake package.'
    good "volk installed: $VOLK_CONFIG"
fi
VOLK_PREFIX="$(dirname "$(dirname "$(dirname -- "$VOLK_CONFIG")")")"
stage_done '4. volk (Vulkan meta-loader)'

# ---------------------------------------------------------------------------
step 'Building the recompiler (RebrewU)'
REBREWU_BUILD="$WORK/rebrewu"
cmake -S "$RECOMPILER" -B "$REBREWU_BUILD" -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DREBREWU_BUILD_TESTS=OFF \
    || fail 'Configuring the recompiler'
cmake --build "$REBREWU_BUILD" --target rebrewu --parallel "$JOBS" \
    || fail 'Building the recompiler'
REBREWU="$REBREWU_BUILD/bin/rebrewu"
[ -x "$REBREWU" ] || fail 'rebrewu was not produced.'
good "Recompiler: $REBREWU"
stage_done '5. RebrewU recompiler build'

# ---------------------------------------------------------------------------
step 'Recompiling your Gambit.rpx into C++ (a few minutes)'
GENERATED="$WORK/generated/Gambit"
STAMP="$WORK/generated/source.sha256"
# The manifest carries the entry boundaries, hook list and patches. The RPX
# hash above selects its supported version; RebrewU still validates each
# boundary against the RPX. Include the tool and manifest hashes in the stamp
# so changes regenerate.
current="$rpx_hash:$(sha256 "$REBREWU"):$(sha256 "$SELECTED_MANIFEST")"
previous=""
[ -f "$STAMP" ] && previous="$(cat -- "$STAMP")"
if ! generated_module_complete "$GENERATED" || [ "$current" != "$previous" ]; then
    # Safe incremental regeneration: CppEmitter preserves unchanged file
    # timestamps and archives obsolete parts safely.
    ensure_folder "$GENERATED"
    "$REBREWU" recompile --rpx "$rpx" --output "$GENERATED" \
        --manifest "$SELECTED_MANIFEST" \
        || fail 'Recompiling Gambit.rpx'
    generated_module_complete "$GENERATED" \
        || fail 'The recompiler did not produce the complete supported-game function table. Installation stopped before building an incomplete game.'
    printf '%s\n' "$current" > "$STAMP"
fi
good 'Game code recompiled (stays on your PC, inside build/generated).'
stage_done '6. RPX decompilation to C++'

# ---------------------------------------------------------------------------
step 'Building the game (this is the long part: 15-60 minutes)'
GAME_BUILD="$WORK/game"
SDL2_CONFIG="$(find "$DEPS/sdl2-vulkan" -name SDL2Config.cmake -print -quit)"
[ -n "$SDL2_CONFIG" ] || fail 'SDL2Config.cmake not found in the dependency profile.'
SDL2_DIR="$(dirname -- "$SDL2_CONFIG")"
GLSLANG_DIR="$DEPS/glslang-vulkan/lib/cmake/glslang"
[ -f "$GLSLANG_DIR/GlslangVulkanOnly.cmake" ] || fail 'glslang dependency profile marker missing.'

cmake -S "$RECOMPILER/port" -B "$GAME_BUILD" -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DRBREW_GRAPHICS_BACKEND=Vulkan \
    -DRBREW_GAME_REVISION="$GAME_REVISION" \
    -DRBREW_RPX="$rpx" \
    -DGAMBIT_GENERATED_DIR="$GENERATED" \
    -DSDL2_DIR="$SDL2_DIR" \
    -Dglslang_DIR="$GLSLANG_DIR" \
    -DCMAKE_PREFIX_PATH="$VOLK_PREFIX" \
    -DRBREW_BUILD_GAME=ON \
    -DRBREW_ENABLE_COVERAGE=OFF \
    -DRBREW_ENABLE_PPC_FALLBACK=OFF \
    -DRBREW_DIAGNOSTIC_RUNTIME=OFF \
    -DRBREW_TEXTURE_DIAGNOSTICS=OFF \
    || fail 'Configuring the game'
cmake --build "$GAME_BUILD" --target Gambit --parallel "$JOBS" \
    || fail 'Building the game'
EXE=""
EXE="$(find "$GAME_BUILD" -maxdepth 2 -type f -name Splatoon -print -quit)"
[ -n "$EXE" ] || fail 'The Splatoon executable was not produced.'
good "Built: $EXE"
stage_done '7. Game compilation & linking'

# ---------------------------------------------------------------------------
step 'Creating the game folder'
ensure_folder "$OUTPUT"
initialize_wiiu_folders "$OUTPUT"
cp -f -- "$EXE" "$OUTPUT/Splatoon"
chmod +x "$OUTPUT/Splatoon"
# Any shared libraries the build placed next to the executable (normally none).
while IFS= read -r -d '' lib; do
    cp -f -- "$lib" "$OUTPUT/"
done < <(find "$(dirname -- "$EXE")" -maxdepth 1 -name '*.so*' -print0 2>/dev/null)
info 'Copying game content (about 1.7 GB) ...'
cp -au -- "$GAME_DUMP/content/." "$OUTPUT/content/"
cp -au -- "$GAME_DUMP/meta/." "$OUTPUT/meta/"
good 'Wii U folders ready. No existing game or Mii saves were imported.'
good 'A new installation starts fresh; progress already in this output folder is kept on updates.'
stage_done '8. Game folder packaging'

# ---------------------------------------------------------------------------
printf '\n============================================================\n'
printf '   Setup complete!\n'
printf '   Play: %s\n' "$OUTPUT/Splatoon"
printf '============================================================\n'

TOTAL_ELAPSED=$(( $(date +%s) - TOTAL_START ))
printf '\n------------------------------------------------------------\n'
printf '   BUILD PERFORMANCE REPORT (Total: %ss)\n' "$TOTAL_ELAPSED"
printf '%s\n' '------------------------------------------------------------'
while IFS= read -r name; do
    printf '   %-52s : %6s s\n' "$name" "${STAGE_TIMES[$name]}"
done < <(printf '%s\n' "${!STAGE_TIMES[@]}" | sort)
printf '%s\n' '------------------------------------------------------------'
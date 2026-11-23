# SplatPC C++ Modding API

SplatPC includes a standalone native C/C++ plugin loader and exLaunch-style trampoline hooking API designed so modded gameplay features can live outside the vanilla Wii U base port.

When no mods are installed, SplatPC loads nothing, allocates no trampolines, and hides the **Mods** entry from the ESC menu.

---

## 1. Installing & Loading Mods

Place compiled shared libraries next to the Splatoon executable:

- Windows: `mods/*.dll`
- Linux: `mods/*.so`

Environment variables:

- `RBREW_MODS_DIR=/custom/path`: override the mod scan directory.
- `RBREW_NO_MODS=1`: disable loading all mods for troubleshooting.

Mods are loaded in ascending filename order and shut down in reverse order.

---

## 2. SDK Headers (Zero Host Linkage Required)

Mods only need the two pure C/C++ headers in `recompiler/port/include/`:

- `splat_mod_api.h`: ABI version (`SPLATMOD_ABI_VERSION == 1`), `SplatModInfo`, `SplatModEarlyHost`, `SplatModHost`, `SplatModUi`, big-endian guest memory helpers (`splatmod_read32`, `splatmod_write32`, etc.), and trampoline macros.
- `splat_cpu_state.h`: `CPUState` and `HostFunc` definitions.

Plugins do not link `gambit_runtime`, Vulkan, SDL2, or Dear ImGui, avoiding C++/CRT ABI mismatches across compilers.

---

## 3. Plugin Lifecycle Entrypoints

Export these C symbols with `SPLATMOD_EXPORT`:

- **Required** `const SplatModInfo* splatmod_info(void)`
  - Return a static `SplatModInfo` with `abi_version = SPLATMOD_ABI_VERSION`, a unique `id` (e.g. `"author.my_mod"`), `name`, `version`, and optional `author`.
- **Optional** `int splatmod_early_init(const SplatModEarlyHost* host, int argc, char** argv)`
  - Runs at startup right after `rbrew_settings::load()`, before Vulkan or guest memory allocation (useful for CLI modes or headless helpers).
  - Return `-1` to continue normal game launch, or `>= 0` to exit immediately with that exit code.
- **Required** `bool splatmod_init(const SplatModHost* host)`
  - Runs after the guest CPU, arena, OS thunks, and built-in hooks are initialized.
- **Optional** `void splatmod_shutdown(void)`
  - Called on exit before mods are unloaded.

---

## 4. Hooking Guest & Native Functions

### Guest Function Hooks (`SPLAT_DEFINE_GUEST_HOOK`)

`host->hook_guest(guest_addr, callback, &orig)` supports every PowerPC guest address uniformly across three host tiers:

1. **Manifest-hooked functions**: dynamically stacked via `rbrew_set_guest_hook`, preserving any built-in host hooks underneath mods.
2. **All other recompiled `Gambit_fn_XXXXXXXX` functions**: hooked automatically at runtime via x86-64 near-trampolines without modifying `.manifest` files or recompiling the game.
3. **Host OS / `dispatch_table`-only addresses**: wrapped transparently via JIT dispatch bridges.

Multiple mods can stack hooks on the same address and unhook in any order.

```cpp
#include "splat_mod_api.h"

static const SplatModHost* g_host = nullptr;

SPLAT_DEFINE_GUEST_HOOK(PlayerSetupHook)
void PlayerSetupHook::Callback(CPUState* cpu, uint32_t /*guest_addr*/) {
    // Call the previous hook in the chain / original guest function
    Orig(cpu);
    // Inspect or mutate registers/memory via cpu->r[] and cpu->mem
}

extern "C" SPLATMOD_EXPORT bool splatmod_init(const SplatModHost* host) {
    g_host = host;
    return PlayerSetupHook::InstallAt(host, 0x023FED60u);
}

extern "C" SPLATMOD_EXPORT void splatmod_shutdown(void) {
    PlayerSetupHook::Uninstall(g_host);
}
```

### Native Function Hooks (`SPLAT_DEFINE_NATIVE_HOOK`)

Hook any native C/C++ function pointer inside the host process:

```cpp
SPLAT_DEFINE_NATIVE_HOOK(MyNativeHook, int, (int a, int b))
int MyNativeHook::Callback(int a, int b) {
    return Orig(a + 1, b);
}

// Install inside splatmod_init:
MyNativeHook::InstallAtFn(host, &some_native_function);
```

### Calling Guest Functions (`host->call_guest`)

Invoke any mapped guest address (including passing up to 8 GPR arguments `r3..r10` and reading `r3` back) while preserving caller `r3..r10` and `lr`:

```cpp
const uint32_t args[2] = {arg0, arg1};
uint32_t ret_r3 = 0;
host->call_guest(cpu, 0x023FED60u, args, 2, &ret_r3);
```

---

## 5. Optional Host Extensions

All host extension points gate on relaxed atomic flags, imposing zero runtime overhead when unused:

- **Controller & Input**: `add_vpad_hook`, `add_kpad_provider` (injects Wii U Pro Controller data into `WPADProbe`/`KPADReadEx`), `set_relative_mouse_in_gamepad`
- **Audio Tap**: `add_audio_tap` (receives stereo 32 kHz float frames)
- **Vulkan Frame Capture**: `set_frame_capture` (non-blocking 2-slot async GPU-to-host readback of TV or GamePad view)
- **Rendering & Window Controls**: `set_dual_view_active`, `set_suppress_cinema_hud`, `set_window_title`, `request_screen_mode`, `current_screen_mode`, `request_quit`
- **ESC -> Mods ImGui Menu (`SplatModUi`)**: `add_menu_page` exposes a C-ABI UI builder (`text`, `button`, `checkbox`, `slider_int`, `input_text`, `same_line`, `separator`, etc.) rendered inside the native ESC menu.

---

## 6. Minimal CMake Mod Example

```cmake
cmake_minimum_required(VERSION 3.18)
project(my_splat_mod CXX)
set(CMAKE_CXX_STANDARD 20)

add_library(my_splat_mod MODULE src/mod.cpp)
target_include_directories(my_splat_mod PRIVATE path/to/recompiler/port/include)
if(MINGW)
    target_link_options(my_splat_mod PRIVATE -static)
endif()
```

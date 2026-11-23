#pragma once

// Public C/C++ Modding SDK for SplatPC.
// Place compiled mod shared libraries (*.dll on Windows, *.so on Linux) in the
// `mods/` directory next to the Splatoon executable (or in $RBREW_MODS_DIR).

#include "splat_cpu_state.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SPLATMOD_ABI_VERSION 1u

#ifdef __cplusplus
#if defined(_WIN32)
#define SPLATMOD_EXPORT extern "C" __declspec(dllexport)
#else
#define SPLATMOD_EXPORT extern "C" __attribute__((visibility("default")))
#endif
#else
#if defined(_WIN32)
#define SPLATMOD_EXPORT __declspec(dllexport)
#else
#define SPLATMOD_EXPORT __attribute__((visibility("default")))
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SplatModLogLevel {
    SPLATMOD_LOG_INFO = 0,
    SPLATMOD_LOG_WARN = 1,
    SPLATMOD_LOG_ERROR = 2,
    SPLATMOD_LOG_DEBUG = 3,
} SplatModLogLevel;

typedef enum SplatModMenuResult {
    SPLATMOD_MENU_STAY = 0,
    SPLATMOD_MENU_BACK = 1,
    SPLATMOD_MENU_QUIT = 2,
} SplatModMenuResult;

typedef enum SplatModScreenMode {
    SPLATMOD_SCREEN_TV = 0,
    SPLATMOD_SCREEN_GAMEPAD = 1,
} SplatModScreenMode;

typedef struct SplatModInfo {
    uint32_t abi_version; // Must match SPLATMOD_ABI_VERSION
    const char* id;       // Short unique id, e.g. "author.mod_name"
    const char* name;     // Display name shown in the ESC -> Mods menu
    const char* version;  // Mod version string
    const char* author;   // Optional author string
} SplatModInfo;

typedef struct SplatModProPad {
    uint32_t hold;
    float lx;
    float ly;
    float rx;
    float ry;
} SplatModProPad;

// Minimal C-ABI UI builder exposed to mod menu pages (no ImGui linkage needed).
typedef struct SplatModUi {
    void* user;
    void (*text)(void* user, const char* text);
    void (*text_colored)(void* user, float r, float g, float b, float a, const char* text);
    void (*text_wrapped)(void* user, const char* text);
    void (*separator)(void* user);
    void (*separator_text)(void* user, const char* label);
    void (*spacing)(void* user);
    void (*same_line)(void* user);
    bool (*button)(void* user, const char* label, float width);
    bool (*checkbox)(void* user, const char* label, bool* value);
    bool (*slider_int)(void* user, const char* label, int* value, int min_val, int max_val, const char* format);
    bool (*input_text)(void* user, const char* label, const char* hint, char* buf, size_t buf_size);
    void (*begin_disabled)(void* user, bool disabled);
    void (*end_disabled)(void* user);
    void (*set_clipboard_text)(void* user, const char* text);
} SplatModUi;

typedef uint64_t SplatModHookHandle;

// Guest hook invoked with Wii U CPUState and the hooked guest address.
typedef void (*SplatModGuestHook)(CPUState* cpu, uint32_t guest_addr);
typedef void (*SplatModGuestOrig)(CPUState* cpu);

typedef void (*SplatModVpadCallback)(uint8_t* mem, void* user);
typedef bool (*SplatModKpadProvider)(uint32_t chan, SplatModProPad* out_pad, void* user);
typedef void (*SplatModAudioCallback)(const float* stereo_frames, uint32_t frame_count, void* user);
typedef bool (*SplatModCaptureQuery)(uint32_t* out_view, uint32_t* out_width, uint32_t* out_height, void* user);
typedef void (*SplatModCaptureSubmit)(const uint8_t* rgba, uint32_t width, uint32_t height, void* user);
typedef int (*SplatModMenuPageCallback)(const SplatModUi* ui, void* user);
typedef const char* (*SplatModMenuStatusCallback)(void* user);

typedef struct SplatModEarlyHost {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t game_revision;
    void (*log)(SplatModLogLevel level, const char* tag, const char* fmt, ...);
    const char* (*mods_dir)(void);
} SplatModEarlyHost;

typedef struct SplatModHost {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t game_revision;

    void (*log)(SplatModLogLevel level, const char* tag, const char* fmt, ...);
    const char* (*mods_dir)(void);

    // Function hooking & guest calls (safe to stack and unhook in any order).
    SplatModHookHandle (*hook_guest)(uint32_t guest_addr, SplatModGuestHook hook, SplatModGuestOrig* out_orig);
    bool (*unhook_guest)(SplatModHookHandle handle);
    SplatModHookHandle (*hook_native)(void* target, void* detour, void** out_orig);
    bool (*unhook_native)(SplatModHookHandle handle);

    bool (*call_guest)(CPUState* cpu, uint32_t guest_addr, const uint32_t* args, uint32_t arg_count, uint32_t* out_r3);
    int (*active_scene_type)(uint8_t* mem, int* out_scene_id);
    bool (*memory_accessible)(const uint8_t* mem, uint32_t addr, uint64_t size);
    void (*write_memory)(uint8_t* mem, uint32_t addr, const void* bytes, uint32_t size);

    // Optional host extension hooks (zero runtime overhead when not registered).
    SplatModHookHandle (*add_menu_page)(const char* title, SplatModMenuPageCallback draw,
                                        SplatModMenuStatusCallback status, void* user);
    SplatModHookHandle (*add_vpad_hook)(SplatModVpadCallback cb, void* user);
    SplatModHookHandle (*add_kpad_provider)(SplatModKpadProvider provider, void* user);
    SplatModHookHandle (*add_audio_tap)(SplatModAudioCallback cb, void* user);
    SplatModHookHandle (*set_frame_capture)(SplatModCaptureQuery query, SplatModCaptureSubmit submit, void* user);
    bool (*unhook_extension)(SplatModHookHandle handle);

    void (*set_dual_view_active)(bool active);
    void (*set_suppress_cinema_hud)(bool suppress);
    void (*set_relative_mouse_in_gamepad)(bool enable);
    void (*set_window_title)(const char* title);
    void (*request_screen_mode)(int screen_mode);
    int (*current_screen_mode)(void);
    void (*request_quit)(void);
} SplatModHost;

// Plugin entrypoints exported by each mod library:
//   Required: splatmod_info, splatmod_init
//   Optional: splatmod_early_init (return >= 0 to exit process early with that code, -1 to continue)
//             splatmod_shutdown
typedef const SplatModInfo* (*SplatModInfoFn)(void);
typedef int (*SplatModEarlyInitFn)(const SplatModEarlyHost* host, int argc, char** argv);
typedef bool (*SplatModInitFn)(const SplatModHost* host);
typedef void (*SplatModShutdownFn)(void);

// Big-endian guest memory access helpers (for mods reading/writing via cpu->mem).
static inline uint8_t splatmod_read8(const uint8_t* mem, uint32_t addr) {
    return mem[addr];
}

static inline uint16_t splatmod_read16(const uint8_t* mem, uint32_t addr) {
    uint8_t b[2];
    memcpy(b, mem + addr, 2);
    return (uint16_t)(((uint16_t)b[0] << 8) | (uint16_t)b[1]);
}

static inline uint32_t splatmod_read32(const uint8_t* mem, uint32_t addr) {
    uint8_t b[4];
    memcpy(b, mem + addr, 4);
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static inline uint64_t splatmod_read64(const uint8_t* mem, uint32_t addr) {
    return ((uint64_t)splatmod_read32(mem, addr) << 32) | (uint64_t)splatmod_read32(mem, addr + 4);
}

static inline float splatmod_read_f32(const uint8_t* mem, uint32_t addr) {
    uint32_t bits = splatmod_read32(mem, addr);
    float value;
    memcpy(&value, &bits, 4);
    return value;
}

static inline void splatmod_write8(uint8_t* mem, uint32_t addr, uint8_t value) {
    mem[addr] = value;
}

static inline void splatmod_write16(uint8_t* mem, uint32_t addr, uint16_t value) {
    uint8_t b[2] = {(uint8_t)(value >> 8), (uint8_t)value};
    memcpy(mem + addr, b, 2);
}

static inline void splatmod_write32(uint8_t* mem, uint32_t addr, uint32_t value) {
    uint8_t b[4] = {(uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value};
    memcpy(mem + addr, b, 4);
}

static inline void splatmod_write64(uint8_t* mem, uint32_t addr, uint64_t value) {
    splatmod_write32(mem, addr, (uint32_t)(value >> 32));
    splatmod_write32(mem, addr + 4, (uint32_t)value);
}

static inline void splatmod_write_f32(uint8_t* mem, uint32_t addr, float value) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    splatmod_write32(mem, addr, bits);
}

#ifdef __cplusplus
} // extern "C"

// exLaunch-style trampoline hook declaration helpers for C++20 mods.
//
// Example (guest function hook):
//   SPLAT_DEFINE_GUEST_HOOK(PlayerSetupHook)
//   void PlayerSetupHook::Callback(CPUState* cpu, uint32_t addr) {
//       Orig(cpu);
//       // modify cpu->r[3], etc.
//   }
//   // In splatmod_init: PlayerSetupHook::InstallAt(host, 0x023FED60u);
//
// Example (native C++ function hook):
//   SPLAT_DEFINE_NATIVE_HOOK(MyNativeHook, int, (int x, int y))
//   int MyNativeHook::Callback(int x, int y) {
//       return Orig(x + 1, y);
//   }
//   // In splatmod_init: MyNativeHook::InstallAtFn(host, &target_fn);

#define SPLAT_DEFINE_GUEST_HOOK(Name)                                                                                  \
    struct Name {                                                                                                      \
        static inline SplatModGuestOrig s_orig = nullptr;                                                              \
        static inline SplatModHookHandle s_handle = 0;                                                                 \
        static inline void Orig(CPUState* cpu) {                                                                       \
            if (s_orig) s_orig(cpu);                                                                                   \
        }                                                                                                              \
        static inline bool InstallAt(const SplatModHost* host, uint32_t guest_addr) {                                  \
            if (!host || !host->hook_guest || s_handle) return false;                                                  \
            s_handle = host->hook_guest(guest_addr, &Name::Callback, &Name::s_orig);                                   \
            return s_handle != 0;                                                                                      \
        }                                                                                                              \
        static inline void Uninstall(const SplatModHost* host) {                                                       \
            if (host && host->unhook_guest && s_handle) {                                                              \
                host->unhook_guest(s_handle);                                                                          \
                s_handle = 0;                                                                                          \
                s_orig = nullptr;                                                                                      \
            }                                                                                                          \
        }                                                                                                              \
        static void Callback(CPUState* cpu, uint32_t guest_addr);                                                      \
    };

#define SPLAT_DEFINE_NATIVE_HOOK(Name, Ret, Args)                                                                      \
    struct Name {                                                                                                      \
        using FnType = Ret(*) Args;                                                                                    \
        static inline FnType s_orig = nullptr;                                                                         \
        static inline SplatModHookHandle s_handle = 0;                                                                 \
        template <typename... CallArgs> static inline Ret Orig(CallArgs&&... call_args) {                              \
            return s_orig(static_cast<CallArgs&&>(call_args)...);                                                      \
        }                                                                                                              \
        static inline bool InstallAt(const SplatModHost* host, void* target) {                                         \
            if (!host || !host->hook_native || s_handle) return false;                                                 \
            void* orig_ptr = nullptr;                                                                                  \
            s_handle = host->hook_native(target, reinterpret_cast<void*>(&Name::Callback), &orig_ptr);                 \
            s_orig = reinterpret_cast<FnType>(orig_ptr);                                                               \
            return s_handle != 0;                                                                                      \
        }                                                                                                              \
        static inline bool InstallAtFn(const SplatModHost* host, FnType target) {                                      \
            return InstallAt(host, reinterpret_cast<void*>(target));                                                   \
        }                                                                                                              \
        static inline void Uninstall(const SplatModHost* host) {                                                       \
            if (host && host->unhook_native && s_handle) {                                                             \
                host->unhook_native(s_handle);                                                                         \
                s_handle = 0;                                                                                          \
                s_orig = nullptr;                                                                                      \
            }                                                                                                          \
        }                                                                                                              \
        static Ret Callback Args;                                                                                      \
    };
#endif

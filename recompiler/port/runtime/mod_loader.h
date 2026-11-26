#pragma once

// Runtime mod plugin discovery, loader, C++ hook dispatcher, and host extension hooks.

#include "../include/splat_mod_api.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace rbrew_mod {

struct ModSummary {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    bool initialized = false;
};

struct ModMenuPageView {
    uint64_t handle = 0;
    std::string title;
    SplatModMenuPageCallback draw = nullptr;
    SplatModMenuStatusCallback status = nullptr;
    void* user = nullptr;
};

namespace detail {
extern std::atomic<bool> has_vpad_hooks;
extern std::atomic<bool> has_kpad_providers;
extern std::atomic<bool> has_audio_taps;
extern std::atomic<bool> has_frame_capture;
extern std::atomic<bool> dual_view;
extern std::atomic<bool> suppress_cinema_hud;
extern std::atomic<bool> relative_mouse_gamepad;
extern std::atomic<bool> has_window_title_override;

void dispatch_vpad_hooks(uint8_t* mem);
bool dispatch_kpad_provider(uint32_t chan, SplatModProPad* out_pad);
void dispatch_audio_taps(const float* stereo_frames, uint32_t frame_count);
bool copy_window_title_override_locked(std::string& out_title);
} // namespace detail

// Scans $RBREW_MODS_DIR (or <exe_dir>/mods) unless RBREW_NO_MODS=1, validates
// splatmod_info, and runs optional splatmod_early_init.
// Returns >= 0 when a mod requests early process exit with that code, or -1 to continue.
int load_and_early_init(int argc, char** argv, uint32_t game_revision);

// Initializes all loaded mods once the guest CPU, arena, dispatch table, and
// OS hooks are registered.
void init_mods(CPUState* cpu);

// Calls splatmod_shutdown in reverse load order, removes all active mod hooks
// and extensions, and unloads mod shared libraries. Safe to call repeatedly.
void shutdown_mods();

// Test helpers for headless unit tests.
bool load_mod_from_path(const char* path, uint32_t game_revision);
void set_dispatch_cpu_for_test(CPUState* cpu);
void unload_mods_for_test();
const SplatModHost* host_api();

// Introspection for the ImGui ESC menu.
bool has_mods();
std::vector<ModSummary> list_mods();
std::vector<ModMenuPageView> list_menu_pages();

// Zero-overhead host extension fast-paths.
inline bool has_vpad_hooks() {
    return detail::has_vpad_hooks.load(std::memory_order_relaxed);
}

inline void on_vpad_read(uint8_t* mem) {
    if (detail::has_vpad_hooks.load(std::memory_order_relaxed)) detail::dispatch_vpad_hooks(mem);
}

inline bool has_kpad_providers() {
    return detail::has_kpad_providers.load(std::memory_order_relaxed);
}

inline bool poll_kpad_provider(uint32_t chan, SplatModProPad* out_pad) {
    return detail::has_kpad_providers.load(std::memory_order_relaxed) && detail::dispatch_kpad_provider(chan, out_pad);
}

inline void on_audio_frames(const float* stereo_frames, uint32_t frame_count) {
    if (detail::has_audio_taps.load(std::memory_order_relaxed)) detail::dispatch_audio_taps(stereo_frames, frame_count);
}

inline bool has_frame_capture() {
    return detail::has_frame_capture.load(std::memory_order_relaxed);
}

bool query_frame_capture(uint32_t* out_view, uint32_t* out_width, uint32_t* out_height);
void submit_captured_frame(const uint8_t* rgba, uint32_t width, uint32_t height);

inline bool dual_view_active() {
    return detail::dual_view.load(std::memory_order_relaxed);
}

inline bool suppress_cinema_hud() {
    return detail::suppress_cinema_hud.load(std::memory_order_relaxed);
}

inline bool relative_mouse_in_gamepad() {
    return detail::relative_mouse_gamepad.load(std::memory_order_relaxed);
}

bool consume_relative_mouse_changed();

inline bool copy_window_title_override(std::string& out_title) {
    return detail::has_window_title_override.load(std::memory_order_relaxed) &&
           detail::copy_window_title_override_locked(out_title);
}

bool consume_window_title_changed();
bool consume_screen_mode_request(int* out_mode);
void note_screen_mode(int mode);
int current_screen_mode();
bool consume_quit_request();

} // namespace rbrew_mod

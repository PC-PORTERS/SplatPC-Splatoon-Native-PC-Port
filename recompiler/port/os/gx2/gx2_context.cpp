#include "../../runtime/rbrew_diagnostics.h"
#include "../../runtime/texture_diagnostics.h"
// GX2 context and initialization
// GX2Init, GX2Shutdown, GX2SetContextState, GX2DrawDone, swap chain
//
// Creates the selected backend's SDL2 window. Vulkan uses SDL_WINDOW_VULKAN
// and a native Vulkan device/swapchain; TV/GamePad toggle retains touch input.

#include "../os_common.h"
#include "../../runtime/screen_mode.h"
#include "gx2_render.h"
#include "gx2_pm4_state.h"
#include "gx2_perf.h"
#include "gx2_vblank.h"
#include "../../runtime/host_sleep.h"
#include <cstring>
#include <mutex>
#include <utility>
#include "gx2_native_context.h"
#include "gx2_surface.h"
#include "gx2_display_list.h"
#include "gx2_frame_interp.h"
#include "gx2_env_light.h"
#include "vk/gx2_vk_capture.h"
#include "../../runtime/port_settings.h"
#include "gx2_render_vk_debug.h"
#include "../vpad/game_controllers.h"
#include "../../runtime/splatoon_diagnostics.h"
#include "../../runtime/benchmark_input.h"
#if RBREW_GAME_REVISION == 288
#include "../../hooks/plaza_frame_rate.h"
#endif
#include "../../runtime/native_control_defaults.h"
#include "../../runtime/discord_rpc.h"
#include <cstdlib>
#include <string>
#include <stdio.h>
#include <cstdio>
#include <atomic>
#include <thread>
#include <array>
#include <vector>

// Definitions for shared screen-mode state (declared in screen_mode.h)
ScreenMode g_screen_mode = ScreenMode::TV;
TouchState g_touch_state = {false, 0, 0};

void gx2_draw_set_default_state(CPUState* cpu);

// SDL windowing is optional for headless tools and tests.
#if defined(HAVE_SDL2)
#include <SDL2/SDL.h>
#if defined(_WIN32)
#include <SDL2/SDL_syswm.h>
#endif
#include "vk/gx2_vk_shader.h"
#include "vk/gx2_imgui_overlay.h"
// gx2_render_vk.cpp owns the Vulkan device; this branch needs no GPU types.

static SDL_Window* s_window = nullptr;
static bool s_tv_enabled = true;

// GamePad screen resolution (hardware fixed)
static constexpr int DRC_W = 854;
static constexpr int DRC_H = 480;

// Frame statistics live in the ImGui performance overlay (F3); the title
// only shows the view mode.
static void update_window_title() {
    if (!s_window) return;
    char title_buf[128];
    std::snprintf(title_buf, sizeof(title_buf), "Splatoon | %s | Vulkan [F3=overlay | Tab=view | F11=window]",
                  g_screen_mode == ScreenMode::TV ? "TV" : "Gamepad");
    SDL_SetWindowTitle(s_window, title_buf);
}

// The settings menu needs a free cursor; the game captures it in TV mode.
// The F3 overlay only shows numbers: the mouse keeps turning the camera
// (RBREW_OVERLAY_CURSOR=1: free it for the overlay too, to click its widgets).
static bool imgui_wants_cursor() {
    static const bool overlay_cursor = [] {
        const char* v = std::getenv("RBREW_OVERLAY_CURSOR");
        return v && v[0] == '1';
    }();
    return gx2_imgui::menu_open() || (overlay_cursor && gx2_imgui::visible());
}
static void sync_overlay_mouse_mode() {
    if (g_screen_mode == ScreenMode::TV) SDL_SetRelativeMouseMode(imgui_wants_cursor() ? SDL_FALSE : SDL_TRUE);
}

// Switch between TV and Gamepad view, adjusting mouse capture accordingly
static void toggle_screen_mode() {
    if (g_screen_mode == ScreenMode::TV) {
        g_screen_mode = ScreenMode::Gamepad;
        SDL_SetRelativeMouseMode(SDL_FALSE); // free the cursor for touch sim
        g_touch_state = {false, 0, 0};
    } else {
        g_screen_mode = ScreenMode::TV;
        sync_overlay_mouse_mode(); // capture cursor for camera, unless the overlay is open
        g_touch_state = {false, 0, 0};
    }
    update_window_title();
}

static bool window_fullscreen() {
    return s_window && (SDL_GetWindowFlags(s_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
}
static void set_window_fullscreen(bool full) {
    if (!s_window || window_fullscreen() == full) return;
    if (!full) {
        SDL_SetWindowFullscreen(s_window, 0);
        SDL_SetWindowSize(s_window, 1280, 720);
        SDL_SetWindowPosition(s_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    } else {
        SDL_SetWindowFullscreen(s_window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
    update_window_title();
}
// Single switch for the performance overlay (F3 and the ESC menu row).
static void set_overlay_display(bool show) {
    gx2_imgui::set_visible(show);
    sync_overlay_mouse_mode();
}

// Map a window-space mouse position to GamePad touch coordinates [0,853] x [0,479]
static void map_mouse_to_touch(int mx, int my) {
    int ww, wh;
    SDL_GetWindowSize(s_window, &ww, &wh);
    if (ww <= 0 || wh <= 0) return;

    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx >= ww) mx = ww - 1;
    if (my >= wh) my = wh - 1;

    g_touch_state.x = (uint16_t)((mx * (DRC_W - 1)) / (ww - 1));
    g_touch_state.y = (uint16_t)((my * (DRC_H - 1)) / (wh - 1));
}

#if defined(_WIN32)
extern "C" __declspec(dllimport) void __stdcall DisableProcessWindowsGhosting(void);
extern "C" __declspec(dllimport) void* __stdcall GetCurrentProcess(void);
extern "C" __declspec(dllimport) int __stdcall TerminateProcess(void* process, unsigned int code);
#endif
static SDL_threadID s_window_thread = 0;

void gx2_render_wait_cache_writes(uint32_t timeout_ms);
// Window close. exit() ran static destructors while the render, recording and
// compiler threads were still running; a joinable std::thread among them
// aborted every close with a crash report (owner WER dumps 2026-10-17).
// Guest file writes use C streams: flush them, let queued shader-cache files
// finish (bounded), then end the process without destructors.
[[noreturn]] static void gx2_close_requested() {
    RBREW_LOG_INFO("gx2", "Window close requested\n");
    gx2_render_wait_cache_writes(2000);
    std::fflush(nullptr);
    TXDIAG(txdiag::finish());
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), 0);
#endif
    std::_Exit(0);
}

// Keep the window responsive before the game presents its first frame:
// process OS messages on the owning thread (at most every 50 ms). SDL events
// stay queued for GX2SwapScanBuffers; only a close request is acted on here.
static void gx2_pump_window_messages() {
    if (!s_window || SDL_ThreadID() != s_window_thread) return;
    static Uint32 s_last = 0;
    const Uint32 now = SDL_GetTicks();
    if (now - s_last < 50) return;
    s_last = now;
    SDL_PumpEvents();
    if (SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_QUIT, SDL_QUIT) > 0) gx2_close_requested();
}

static bool gx2_platform_open_window() {
#if defined(RBREW_DISABLE_LOGGING)
    // SDL's internal diagnostics use their own callback, outside RBREW_LOGF.
    SDL_LogSetOutputFunction([](void*, int, SDL_LogPriority, const char*) {}, nullptr);
#endif
#if defined(_WIN32)
    // A long load before the first presented frame must not turn the window
    // into Windows' "(Not Responding)" ghost with a close prompt.
    DisableProcessWindowsGhosting();
#endif
    // VPADRead maps controller buttons by position. SDL otherwise labels
    // Nintendo-layout pads (Switch Pro, PowerA Spectra and other HIDAPI
    // Switch controllers) by their printed letters, swapping A/B and X/Y.
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    // Keep reading the controller while another window has focus.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SplatoonDiagnostics::Init();
    init_game_controllers();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC | SDL_INIT_SENSOR) !=
        0) {
        controller_log("SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    // Vulkan presentation: no GPU attributes or context. The device and
    // swapchain are created by gx2_render_init for this window; the present
    // mode avoids a second refresh wait (see gx2_vk.cpp create_swapchain).
    SDL_DisplayMode desktop{};
    if (SDL_GetDesktopDisplayMode(0, &desktop) != 0 || desktop.w <= 0 || desktop.h <= 0) return false;
    s_window = SDL_CreateWindow(
        "Splatoon Native PC Port", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, desktop.w, desktop.h,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!s_window) {
        RBREW_LOG_ERROR("gx2", "SDL_CreateWindow(SDL_WINDOW_VULKAN) failed: %s\n", SDL_GetError());
        return false;
    }
#if defined(_WIN32)
    SDL_SysWMinfo window_info{};
    SDL_VERSION(&window_info.version);
    if (SDL_GetWindowWMInfo(s_window, &window_info)) {
        const HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
        if (icon) {
            SendMessageW(window_info.info.win.window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
            SendMessageW(window_info.info.win.window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        }
    }
#endif
    gx2_native_set_context(s_window, nullptr);
    s_window_thread = SDL_ThreadID();
    if (!std::getenv("RBREW_VK_CACHE_DIR")) {
        // Stable SPIR-V / pipeline cache next to the executable.
        if (char* base = SDL_GetBasePath()) {
            gx2vk::shader_set_cache_dir(std::string(base) + "vulkan_cache");
            SDL_free(base);
        }
    }

    // Start in TV view with mouse captured for camera control.
    // Raise the window first so it has focus, then flush any spurious events
    // (e.g. SDL_QUIT) that SDL may have queued during window creation or
    // SDL_SetRelativeMouseMode before the window was visible.
    SDL_RaiseWindow(s_window);
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    SDL_SetRelativeMouseMode(SDL_TRUE);
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
    g_screen_mode = ScreenMode::TV;
    update_window_title();
    {
        // Saved window and overlay choices (the ImGui interface reads and
        // writes rbrew_settings directly).
        const auto settings = rbrew_settings::get();
        if (!settings.fullscreen) set_window_fullscreen(false);
        set_overlay_display(settings.fps_overlay);
        gx2_imgui::set_simple_overlay(settings.simple_overlay);
        gx2_imgui::note_fullscreen(window_fullscreen());
    }

    // Initialize the selected native renderer for the window.
    gx2_render_init();
    gx2_native_start_render_thread();
    gx2_render_startup(); // splash + shader warm-up ahead of the first frame
    return true;
}
#endif // HAVE_SDL2

// ---------------------------------------------------------------------------
// GX2Init(attribs) → TRUE
// ---------------------------------------------------------------------------
static void GX2Init(CPUState* cpu) {
#if defined(HAVE_SDL2)
    if (!s_window) {
        gx2_platform_open_window();
    }
#else
    RBREW_LOG_ERROR("gx2", "GX2Init called (SDL2 not available, stub)\n");
#endif
    RET = 1;
}

// GX2Shutdown()
static void GX2Shutdown(CPUState* cpu) {
#if defined(HAVE_SDL2)
    SplatoonDiagnostics::Shutdown();
    gx2_native_stop_render_thread();
    gx2_render_shutdown();
    if (s_window) {
        SDL_DestroyWindow(s_window);
        s_window = nullptr;
    }
    SDL_Quit();
#endif
}

// GX2SetContextState(state*)
namespace {
struct ContextRange {
    uint32_t memory_offset;
    uint32_t register_base;
    uint32_t first;
    uint32_t count;
};

constexpr ContextRange kContextRanges[] = {
    {0x0000, 0x2000, 0x300, 6},    {0x0000, 0x2000, 0x900, 0x48}, {0x0000, 0x2000, 0x980, 0x48},
    {0x0000, 0x2000, 0xA00, 0x48}, {0x0000, 0x2000, 0x310, 0xC},  {0x0000, 0x2000, 0x542, 1},
    {0x0000, 0x2000, 0x235, 1},    {0x0000, 0x2000, 0x232, 2},    {0x0000, 0x2000, 0x23A, 1},
    {0x0000, 0x2000, 0x256, 1},    {0x0000, 0x2000, 0x60C, 1},    {0x0000, 0x2000, 0x5C5, 1},
    {0x0000, 0x2000, 0x2C8, 1},    {0x0000, 0x2000, 0x363, 1},    {0x0000, 0x2000, 0x404, 2},
    {0x2C00, 0xA000, 0x000, 2},    {0x2C00, 0xA000, 0x003, 3},    {0x2C00, 0xA000, 0x00A, 4},
    {0x2C00, 0xA000, 0x010, 0x38}, {0x2C00, 0xA000, 0x050, 0x34}, {0x2C00, 0xA000, 0x08E, 4},
    {0x2C00, 0xA000, 0x094, 0x40}, {0x2C00, 0xA000, 0x100, 9},    {0x2C00, 0xA000, 0x10C, 3},
    {0x2C00, 0xA000, 0x10F, 0x60}, {0x2C00, 0xA000, 0x185, 0xA},  {0x2C00, 0xA000, 0x191, 0x27},
    {0x2C00, 0xA000, 0x1E0, 9},    {0x2C00, 0xA000, 0x200, 1},    {0x2C00, 0xA000, 0x202, 7},
    {0x2C00, 0xA000, 0x0E0, 0x20}, {0x2C00, 0xA000, 0x210, 0x29}, {0x2C00, 0xA000, 0x250, 0x34},
    {0x2C00, 0xA000, 0x290, 1},    {0x2C00, 0xA000, 0x292, 2},    {0x2C00, 0xA000, 0x2A1, 1},
    {0x2C00, 0xA000, 0x2A5, 1},    {0x2C00, 0xA000, 0x2A8, 2},    {0x2C00, 0xA000, 0x2AC, 3},
    {0x2C00, 0xA000, 0x2CA, 1},    {0x2C00, 0xA000, 0x2CC, 1},    {0x2C00, 0xA000, 0x2CE, 1},
    {0x2C00, 0xA000, 0x300, 9},    {0x2C00, 0xA000, 0x30C, 1},    {0x2C00, 0xA000, 0x312, 1},
    {0x2C00, 0xA000, 0x316, 2},    {0x2C00, 0xA000, 0x343, 2},    {0x2C00, 0xA000, 0x349, 3},
    {0x2C00, 0xA000, 0x34C, 2},    {0x2C00, 0xA000, 0x351, 1},    {0x2C00, 0xA000, 0x37E, 6},
    {0x2C00, 0xA000, 0x2B4, 3},    {0x2C00, 0xA000, 0x2B8, 3},    {0x2C00, 0xA000, 0x2BC, 3},
    {0x2C00, 0xA000, 0x2C0, 3},    {0x2C00, 0xA000, 0x2C8, 1},    {0x2C00, 0xA000, 0x29B, 1},
    {0x2C00, 0xA000, 0x08C, 1},    {0x2C00, 0xA000, 0x0D5, 1},    {0x2C00, 0xA000, 0x284, 0xC},
    {0x3C00, 0xC000, 0, 0x800},    {0x5C00, 0xF880, 0, 0x60},     {0x5E00, 0xE000, 0, 0x70},
    {0x5E00, 0xE000, 0x380, 0x70}, {0x5E00, 0xE000, 0x460, 0x70}, {0x5E00, 0xE000, 0x7E0, 0x70},
    {0x5E00, 0xE000, 0x8B9, 7},    {0x5E00, 0xE000, 0x8C0, 0x70}, {0x5E00, 0xE000, 0x930, 0x70},
    {0x5E00, 0xE000, 0xCB0, 0x70}, {0x5E00, 0xE000, 0xD89, 7},    {0x9500, 0xF000, 0, 0x36},
    {0x9500, 0xF000, 0x36, 0x36},  {0x9500, 0xF000, 0x6C, 0x36},
};

std::array<uint32_t, 3> s_active_context_state{};

template <class Visitor> void visit_context_words(uint32_t state, Visitor&& visitor) {
    for (const auto& range : kContextRanges) {
        for (uint32_t i = 0; i < range.count; ++i) {
            const uint32_t memory_index = range.first + i;
            visitor(range.register_base + memory_index, state + range.memory_offset + memory_index * 4);
        }
    }
}

constexpr size_t context_register_words() {
    size_t words = 0;
    for (const auto& range : kContextRanges) words += range.count;
    return words;
}

template <class ReadWord> void restore_context_words(ReadWord&& read_word) {
    std::vector<Gx2RegisterWrite> writes;
    writes.reserve(context_register_words());
    for (const auto& range : kContextRanges) {
        for (uint32_t i = 0; i < range.count; ++i) {
            const uint32_t memory_index = range.first + i;
            writes.push_back({range.register_base + memory_index, read_word(range.memory_offset + memory_index * 4)});
        }
    }
    gx2_render_apply_registers(writes.data(), writes.size());

    std::array<uint32_t, 0x400> uniforms{};
    for (uint32_t i = 0; i < uniforms.size(); ++i) uniforms[i] = read_word(0x3C00 + i * 4);
    gx2_render_set_uniform_words(true, 0, uniforms.data(), uint32_t(uniforms.size()));
    for (uint32_t i = 0; i < uniforms.size(); ++i) uniforms[i] = read_word(0x3C00 + (0x400 + i) * 4);
    gx2_render_set_uniform_words(false, 0, uniforms.data(), uint32_t(uniforms.size()));
}

void restore_context_state(CPUState* cpu, uint32_t state) {
    restore_context_words([cpu, state](uint32_t offset) { return rbrew_read32(MEM, state + offset); });
}

// Each range (and each uniform bank) is one contiguous guest run: store it with
// one write instead of one per word. The same bytes land at the same
// addresses in the same range order; a run that is not entirely accessible
// falls back to per-word stores, which skip the inaccessible words as before.
void write_context_run(uint8_t* mem, uint32_t address, const uint32_t* words, uint32_t count) {
    if (rbrew_memory_accessible(mem, address, count * 4)) {
        std::array<uint32_t, 0x800> be;
        for (uint32_t i = 0; i < count; ++i) be[i] = rbrew_bswap32(words[i]);
        rbrew_write_memory(mem, address, be.data(), count * 4);
        return;
    }
    for (uint32_t i = 0; i < count; ++i) rbrew_write32(mem, address + i * 4, words[i]);
}

void write_context_state(uint8_t* mem, uint32_t state) {
    std::array<uint32_t, 0x800> words;
    for (const auto& range : kContextRanges) {
        for (uint32_t i = 0; i < range.count; ++i)
            words[i] = gx2_render_register(range.register_base + range.first + i);
        write_context_run(mem, state + range.memory_offset + range.first * 4, words.data(), range.count);
    }
    for (const bool pixel : {true, false}) {
        for (uint32_t i = 0; i < 0x400; ++i) words[i] = gx2_render_uniform_word(pixel, i);
        write_context_run(mem, state + 0x3C00 + (pixel ? 0 : 0x400) * 4, words.data(), 0x400);
    }
}

uint32_t read_context_snapshot_word(const uint8_t* snapshot, uint32_t offset) {
    return (uint32_t(snapshot[offset]) << 24) | (uint32_t(snapshot[offset + 1]) << 16) |
           (uint32_t(snapshot[offset + 2]) << 8) | uint32_t(snapshot[offset + 3]);
}

void restore_context_snapshot(CPUState*, const uint8_t* snapshot) {
    restore_context_words([snapshot](uint32_t offset) { return read_context_snapshot_word(snapshot, offset); });
}

void snapshot_context_state(CPUState* cpu, uint32_t state) {
    std::memset(MEM + state, 0, 0xA100);
    rbrew_write32(MEM, state + 0x9800, ARG1 & 1);
    rbrew_write32(MEM, state + 0x9808, 0);
    write_context_state(MEM, state);
    (void)cpu;
}

void save_active_context(CPUState* cpu, uint32_t state) {
    write_context_state(MEM, state);
    gx2_render_save_context_state(state);

    const uint32_t list_size = rbrew_read32(MEM, state + 0x9804);
    if (!list_size) return;

    const uint32_t updated_size = gx2_dl_create_snapshot(cpu, state + 0x9E00, 0x300, restore_context_snapshot,
                                                         std::span<const uint8_t>(MEM + state, 0x9800));
    if (updated_size)
        rbrew_write32(MEM, state + 0x9804, updated_size);
    else
        RBREW_LOG_ERROR("gx2", "Failed to refresh active context shadow list at %08X\n", state);
}
} // namespace

static void GX2SetContextState(CPUState* cpu) {
    if (!ARG0) return; // Native register writes are always shadowed in renderer-owned state.
    const uint32_t core = rbrew_current_core();
    if (core >= s_active_context_state.size()) {
        RBREW_LOG_ERROR("gx2", "GX2SetContextState received an invalid core id %u\n", core);
        return;
    }
    auto& active_context_state = s_active_context_state[core];
    const uint32_t state = ARG0;
    if (!rbrew_memory_accessible(MEM, state, 0xA100)) {
        RBREW_LOG_ERROR("gx2", "GX2SetContextState received an invalid state address %08X\n", state);
        return;
    }
    // The GPU shadows register writes into the active context's memory, so
    // reselecting the active context reloads its current state, not the
    // values captured when it was last switched away from.
    if (active_context_state && rbrew_memory_accessible(MEM, active_context_state, 0xA100))
        save_active_context(cpu, active_context_state);
    const uint32_t list_size = rbrew_read32(MEM, state + 0x9804);
    extern bool g_gx2_frame_trace_active;
    if (g_gx2_frame_trace_active)
        RBREW_LOG_DEBUG("ftrace", "SetContextState core=%u active=%08X state=%08X list=%u\n", core,
                        active_context_state, state, list_size);
    if (list_size)
        gx2_dl_call(cpu, state + 0x9E00, list_size);
    else
        restore_context_state(cpu, state);
    gx2_render_restore_context_state(state, MEM);
    active_context_state = state;
}

// GX2GetContextStateDisplayList(state, outPtr, outSize)
static void GX2GetContextStateDisplayList(CPUState* cpu) {
    if (!ARG0 || !rbrew_memory_accessible(MEM, ARG0, 0xA100)) return;
    if (ARG1) rbrew_write32(MEM, ARG1, ARG0 + 0x9E00);
    if (ARG2) rbrew_write32(MEM, ARG2, rbrew_read32(MEM, ARG0 + 0x9804));
}

// GX2DrawDone()
static void GX2DrawDone(CPUState* cpu) {
#if defined(HAVE_SDL2)
    gx2_pump_window_messages();
    RET = gx2_native_gpu_finish() ? 1 : 0; // original PPC returns GX2WaitTimeStamp's BOOL in r3
#else
    static std::atomic_flag missing_gpu_reported = ATOMIC_FLAG_INIT;
    if (!missing_gpu_reported.test_and_set()) RBREW_LOG_ERROR("gx2", "GX2DrawDone cannot wait without native GL\n");
    RET = 0;
#endif
}

// GX2Flush()
static void GX2Flush(CPUState* cpu) {
    (void)cpu;
#if defined(HAVE_SDL2)
    gx2_pump_window_messages();
    gx2_native_gpu_flush();
#endif
}

// Swap bookkeeping (gx2.rpl: swap counter DAT_10015620, flip counter
// DAT_10016140, last flip / last vsync OSTime pairs). On the console the
// GPU bumps the flip counter when a queued frame reaches the scan-out. The
// host retires a guest frame after the render thread has consumed its data
// and queued presentation; Vulkan scan-out itself remains asynchronous.
static std::atomic<uint32_t> s_swap_count{0};
static std::atomic<uint32_t> s_flip_count{0};
static std::atomic<uint64_t> s_last_flip{0};
static std::atomic<uint32_t> s_pacer_flag_address{0}; // Gambit pacer object + 0x74 (bit 0: 30 FPS mode)
static std::atomic<uint64_t> s_last_vsync{0};
static std::atomic<float> s_last_vsync_wait_ms{0.0f};
static std::atomic<float> s_last_present_wait_ms{0.0f};
static std::atomic<uint32_t> s_swap_interval{1};

// GX2SetSwapInterval(interval): vblanks between flips (0 = flip at once).
static void GX2SetSwapInterval(CPUState* cpu) {
    s_swap_interval.store(ARG0, std::memory_order_relaxed);
}

// The console flips a swapped frame at the first vblank after the frame is
// done that is also at least `interval` vblanks after the previous flip, and
// the game paces itself on the flip counter. Flipping as soon as the frame was
// presented let it run at 90-100 FPS with game logic and camera sped up.
// Default "auto": a single late frame flips at once (no drop to 30 FPS in the
// 60 FPS tutorial), but a scene whose frames take longer than a vblank most of
// the time (the plaza, ~25 ms) is held at a steady two vblanks per frame, as
// the console shows it, instead of alternating 26/33 ms. Hysteresis keeps it
// from toggling. RBREW_FRAME_PACING=vsync waits for the next vblank on every
// late frame (console-strict: drops to 30 on any slow frame), =adaptive never
// holds 30, =0 restores the immediate flip.
static void pace_flip() {
    // Default: adaptive (60 FPS cap, a late frame flips at once, never held at
    // 30). The user wants a steady 60 everywhere; "auto" (hold 30 in heavy
    // scenes) and "vsync" (console-strict) stay available for testing.
    static const int configured_mode = [] { // 0 off, 1 vblank-quantized, 2 adaptive, 3 auto
        const char* v = std::getenv("RBREW_FRAME_PACING");
        if (v && v[0] == '0') return 0;
        if (v && !std::strcmp(v, "auto")) return 3;
        if (v && !std::strcmp(v, "vsync")) return 1;
        return 2;
    }();
    const bool shaders = rbrew_settings::g_shader_quality.load(std::memory_order_relaxed) != 0;
    const int mode = shaders ? 2 : configured_mode;
    const uint32_t interval = shaders ? 1u : s_swap_interval.load(std::memory_order_relaxed);
    if (!mode || !interval) return;
    const uint64_t cycle = uint64_t(RBREW_TIMER_CLOCK) * 1001; // 60000 vblanks per cycle
    const auto vblank_index = [cycle](uint64_t ticks) {
        return (ticks / cycle) * 60000 + (ticks % cycle) * 60000 / cycle;
    };
    static uint64_t s_last_flip_index = 0, s_last_flip_ticks = 0; // swap thread only
    static uint8_t s_slow[30]{};
    static unsigned s_slow_head = 0, s_slow_count = 0;
    static bool s_half_rate = false;
    const uint64_t now_ticks = rbrew_system_ticks();
    const uint64_t now_index = vblank_index(now_ticks);
    uint32_t effective = interval;
    // Frame rate 30 (low-end PCs): two vblanks per frame, as the console paces
    // the game's own 30 FPS mode; each frame runs two logic steps.
    if (rbrew_settings::g_fps30.load(std::memory_order_relaxed)) effective = std::max(effective, 2u);
    if (mode == 3 && s_last_flip_ticks) {
        // Frame work: last flip to this swap, minus the game's own vsync waits
        // and the present wait (at 30 FPS the game waits longer itself; counting
        // that kept the lock on after a loading screen). Slow = missed a vblank.
        const double work_ms = double(now_ticks - s_last_flip_ticks) * 1000.0 / RBREW_TIMER_CLOCK -
                               s_last_vsync_wait_ms.load(std::memory_order_relaxed) -
                               s_last_present_wait_ms.load(std::memory_order_relaxed);
        const uint8_t slow = work_ms > 17.0 ? 1 : 0, fast = work_ms < 15.0 ? 1 : 0;
        s_slow_count += slow - s_slow[s_slow_head]; // counts slow frames in the window
        s_slow[s_slow_head] = slow;
        s_slow_head = (s_slow_head + 1) % 30;
        static uint8_t s_fast[30]{};
        static unsigned s_fast_head = 0, s_fast_count = 0;
        s_fast_count += fast - s_fast[s_fast_head];
        s_fast[s_fast_head] = fast;
        s_fast_head = (s_fast_head + 1) % 30;
        if (!s_half_rate && s_slow_count >= 20)
            s_half_rate = true;
        else if (s_half_rate && s_fast_count >= 24)
            s_half_rate = false;
        if (s_half_rate) effective = std::max(effective, 2u);
    }
    const uint64_t target_index =
        mode == 1 ? std::max(s_last_flip_index + effective, now_index + 1) : s_last_flip_index + effective;
    if (s_last_flip_index && target_index > now_index) {
        Gx2PerfScope perf_scope(kPerfVsync);
        const auto wait_start = std::chrono::steady_clock::now();
        const uint64_t target = gx2_vblank_tick(target_index, RBREW_TIMER_CLOCK);
        rbrew_host_sleep_until(rbrew_boot_time +
                               std::chrono::nanoseconds(target / RBREW_TIMER_CLOCK * 1000000000ull +
                                                        target % RBREW_TIMER_CLOCK * 1000000000ull / RBREW_TIMER_CLOCK +
                                                        1u));
        s_last_vsync_wait_ms.fetch_add(
            std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - wait_start).count(),
            std::memory_order_relaxed);
        s_last_flip_index = target_index;
    } else {
        s_last_flip_index = now_index;
    }
    s_last_flip_ticks = rbrew_system_ticks();
}

// GX2WaitForVsync() — blocks until the next vblank (gx2.rpl waits on the
// vsync event; GX2WaitForFlip/GX2SetSwapInterval poll in loops around it).
static void GX2WaitForVsync(CPUState* cpu) {
    const auto vsync_start = std::chrono::steady_clock::now();
    Gx2PerfScope perf_scope(kPerfVsync);
    (void)cpu;
#if defined(HAVE_SDL2)
    gx2_pump_window_messages();
#endif
    const uint64_t now = rbrew_system_ticks();
    thread_local Gx2VblankWaiter waiter;
    // (A "skip if present waited > 4 ms" check was removed: the present wait is
    // reset at the end of every swap, so it never applied.)
    const uint64_t target = waiter.target(now, RBREW_TIMER_CLOCK);
    // Gambit's frame pacer (02796C4C) waits for the next vblank only after a
    // frame took longer than one vblank: a 20 ms plaza frame then became a
    // 33 ms one (30 FPS). When the last flip is already a vblank or more ago
    // and every swap has flipped, the frame is late anyway: return at once,
    // as adaptive vsync does (the flip itself stays paced by pace_flip).
    // The plaza steps its game logic in fixed 2-vblank steps (its pacer's 30
    // FPS mode), so skipping this wait runs it faster than the console. Off by
    // default; RBREW_VSYNC_WAIT_LATE=0 skips it (testing).
    static const bool wait_late = [] {
        const char* v = std::getenv("RBREW_VSYNC_WAIT_LATE");
        return !(v && v[0] == '0');
    }();
    const uint64_t vblank_ticks = uint64_t(RBREW_TIMER_CLOCK) * 1001 / 60000;
    const uint64_t last_flip = s_last_flip.load(std::memory_order_relaxed);
    const bool late = !wait_late && last_flip && now > last_flip && now - last_flip >= vblank_ticks &&
                      s_flip_count.load() == s_swap_count.load();
    // Gambit's pacer in its 30 FPS mode (the plaza; flag bit 0 at +0x74 of
    // its frame object) also waits here until two vblanks have passed.
    // RBREW_GAME_30FPS=0 skips that wait (pace_flip still caps flips at 60).
    // Kept by default: skipping it runs a 30 FPS-mode scene's fixed-step logic
    // too fast (the plaza now runs in the 60 FPS mode, see RBREW_PLAZA_60FPS).
    // RBREW_GAME_30FPS=0 skips the wait (testing).
    static const bool keep_30 = [] {
        const char* v = std::getenv("RBREW_GAME_30FPS");
        return !(v && v[0] == '0');
    }();
    const bool pacer = cpu && cpu->lr == 0x02796D7Cu;
    // The pacer's object is in r31 (callee-saved) here: remember where its
    // 30 FPS-mode flag lives so GX2SwapScanBuffers can switch the game to its
    // 60 FPS mode (RBREW_GAME_60FPS_MODE).
    if (pacer && cpu->r[31] && rbrew_memory_accessible(MEM, cpu->r[31] + 0x74, 4))
        s_pacer_flag_address.store(cpu->r[31] + 0x74, std::memory_order_relaxed);
    // In the plaza the 60 FPS fix (RBREW_PLAZA_60FPS) inserts 30 FPS-mode
    // frames only to catch up with real time; they must not wait for two
    // vblanks, so the pacer's wait is skipped there.
    const bool plaza_catch_up = pacer && rbrew_control_defaults::active_scene_type(MEM) == 2 &&
                                rbrew_settings::g_plaza_60fps.load(std::memory_order_relaxed) && [] {
                                    const char* v = std::getenv("RBREW_PLAZA_60FPS");
                                    return !(v && (v[0] == '0' || v[0] == '1'));
                                }();
    if ((!keep_30 || plaza_catch_up) && pacer && s_flip_count.load() == s_swap_count.load()) {
        s_last_vsync_wait_ms.fetch_add(0.0f, std::memory_order_relaxed);
        return;
    }
    if (target > now && !late) {
        rbrew_host_sleep_until(rbrew_boot_time +
                               std::chrono::nanoseconds(target / RBREW_TIMER_CLOCK * 1000000000ull +
                                                        target % RBREW_TIMER_CLOCK * 1000000000ull / RBREW_TIMER_CLOCK +
                                                        1u));
    }
    waiter.complete(rbrew_system_ticks(), RBREW_TIMER_CLOCK);
    uint64_t previous = s_last_vsync.load(std::memory_order_relaxed);
    while (previous < waiter.observed &&
           !s_last_vsync.compare_exchange_weak(previous, waiter.observed, std::memory_order_relaxed)) {}
    const float vsync_dur =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - vsync_start).count();
    s_last_vsync_wait_ms.fetch_add(vsync_dur, std::memory_order_relaxed);
}

// GX2GetSwapStatus(u32* swapCount, u32* flipCount, OSTime* lastFlip,
//                  OSTime* lastVsync) — gx2.rpl 020032d0.
static void GX2GetSwapStatus(CPUState* cpu) {
    const uint32_t swaps = s_swap_count.load(), flips = s_flip_count.load();
    const uint64_t flip_time = s_last_flip.load(), vsync_time = s_last_vsync.load();
    if (ARG0) rbrew_write32(MEM, ARG0, swaps);
    if (ARG1) rbrew_write32(MEM, ARG1, flips);
    if (ARG2) {
        rbrew_write32(MEM, ARG2, uint32_t(flip_time >> 32));
        rbrew_write32(MEM, ARG2 + 4, uint32_t(flip_time));
    }
    if (ARG3) {
        rbrew_write32(MEM, ARG3, uint32_t(vsync_time >> 32));
        rbrew_write32(MEM, ARG3 + 4, uint32_t(vsync_time));
    }
}

// GX2CopyColorBufferToScanBuffer(colorBuffer*, scanTarget)
// scanTarget: 1 = GX2_SCAN_TARGET_TV, 4 = GX2_SCAN_TARGET_DRC
static void GX2CopyColorBufferToScanBuffer(CPUState* cpu) {
#if defined(HAVE_SDL2)
    // gx2.rpl encodes the color buffer into the command stream at call time:
    // snapshot it and post the copy behind the frame's draws.
    if (ARG0 && rbrew_memory_accessible(MEM, ARG0, GX2CB_SIZEOF) && gx2_native_async_active()) {
        struct ScanCopy final : Gx2AsyncWork {
            uint32_t address, target;
            uint8_t* mem;
            std::array<uint8_t, GX2CB_SIZEOF> descriptor;
            void run() override { gx2_render_copy_to_scan(address, target, mem, descriptor.data()); }
        };
        auto* copy = new ScanCopy;
        copy->address = ARG0;
        copy->target = ARG1;
        copy->mem = MEM;
        std::copy_n(MEM + ARG0, GX2CB_SIZEOF, copy->descriptor.data());
        gx2_native_post(copy);
        return;
    }
    gx2_native_run(true, [&] { gx2_render_copy_to_scan(ARG0, ARG1, MEM); });
#else
    gx2_render_copy_to_scan(ARG0, ARG1, MEM);
#endif
}

#if defined(HAVE_SDL2)
// Presents posted to the render thread and completed by it. The guest may
// record one frame while the previous one is still being rendered, as with
// the console's queued flips; it never runs further ahead than that.
static std::atomic<uint64_t> s_presents_posted{0}, s_presents_done{0};
#endif

namespace {
// Plaza at 60 FPS (RBREW_PLAZA_60FPS), the same idea as the Cemu
// "60FPS In Plaza" graphic pack, in own code for this build: Gambit_fn_02A092A8
// (called through a vtable every frame) copies an object's 30 FPS request
// (bit 0 of +0x304) into the framework's 30 FPS mode bit. Clearing the request
// before it runs keeps the game in its 60 FPS mode throughout; the plaza then
// steps its logic once per frame like the rest of the game.
//
// Default "adaptive" (RBREW_PLAZA_60FPS unset or 2): the logic steps once per
// iteration, so a frame slower than 1/60 s would slow the game down. A time
// account (real time minus 1/60 s per iteration) lets the plaza's own 30 FPS
// request through for one frame (two iterations, two steps, its undrawn
// iteration included) whenever the logic is half a step or more behind, so
// the speed stays right on average. RBREW_PLAZA_60FPS=1: always 60 FPS mode;
// =0: the game's own 30 FPS plaza.
HostFunc s_plaza_sync_original = nullptr;
void plaza_mode_sync(CPUState* cpu) {
    static const bool adaptive = [] {
        const char* v = std::getenv("RBREW_PLAZA_60FPS");
        return !(v && v[0] == '1');
    }();
    using clock = std::chrono::steady_clock;
    constexpr double kStepMs = 1000.0 * 1001.0 / 60000.0;
    static clock::time_point last{};
    static double behind_ms = 0; // real time not yet covered by logic steps
    const auto now = clock::now();
    if (last.time_since_epoch().count())
        behind_ms = std::clamp(behind_ms + std::chrono::duration<double, std::milli>(now - last).count() - kStepMs,
                               -kStepMs, 2 * kStepMs);
    last = now;
    const uint32_t object = cpu->r[3];
    if (object && rbrew_memory_accessible(cpu->mem, object + 0x304, 4)) {
        const uint32_t request = rbrew_read32(cpu->mem, object + 0x304);
        // ESC menu "Frame rate: 30" (low-end PCs): the game's own 30 FPS mode
        // in every scene, as the console's plaza: two logic steps per drawn
        // frame, so the game speed stays normal. The bit set here is cleared
        // again when the mode is left.
        static std::vector<uint32_t> s_forced_30;
        if (rbrew_settings::g_fps30.load(std::memory_order_relaxed)) {
            static FILE* stats30 = [] {
                const char* p = std::getenv("RBREW_PLAZA_STATS");
                return p && *p ? std::fopen(p, "w") : nullptr;
            }();
            if (stats30) {
                static clock::time_point second = now;
                static unsigned calls = 0, objects = 0;
                static uint32_t last_object = 0;
                ++calls;
                objects += object != last_object;
                last_object = object;
                if (now - second >= std::chrono::seconds(1)) {
                    std::fprintf(stats30, "30 FPS mode: calls %u object changes %u flags %08X\n", calls, objects,
                                 s_pacer_flag_address.load() ? rbrew_read32(cpu->mem, s_pacer_flag_address.load())
                                                             : 0u);
                    std::fflush(stats30);
                    calls = objects = 0;
                    second = now;
                }
            }
            if (!(request & 1u)) {
                rbrew_write32(cpu->mem, object + 0x304, request | 1u);
                if (std::find(s_forced_30.begin(), s_forced_30.end(), object) == s_forced_30.end())
                    s_forced_30.push_back(object);
            }
            s_plaza_sync_original(cpu);
            return;
        }
        if (const auto forced = std::find(s_forced_30.begin(), s_forced_30.end(), object);
            forced != s_forced_30.end()) {
            s_forced_30.erase(forced);
            rbrew_write32(cpu->mem, object + 0x304, request & ~1u);
            s_plaza_sync_original(cpu);
            return;
        }
        // The plaza sets its request once on entry; after it was cleared the
        // gate owns the bit for that object while the plaza (scene kind 2) runs.
        static uint32_t s_requester = 0;
        if (request & 1u) s_requester = object;
        const bool owned =
            adaptive && object == s_requester && rbrew_control_defaults::active_scene_type(cpu->mem) == 2;
        // ESC menu "Plaza 60 FPS" off: the plaza's own 30 FPS request again.
        const bool menu_on = rbrew_settings::g_plaza_60fps.load(std::memory_order_relaxed);
        bool want_30 = owned && !menu_on;
        // 120 FPS mode: no 30 FPS frames. After the plaza's loading hitch they
        // stuck at ~5900 draws per frame (twice the 60 FPS frame) and 46 FPS;
        // the 120 FPS controller keeps the 60 steps by dropping in-between
        // frames instead. RBREW_PLAZA_ADAPTIVE_120=1: the time account here too.
        static const bool adaptive_120 = [] {
            const char* v = std::getenv("RBREW_PLAZA_ADAPTIVE_120");
            return v && v[0] == '1';
        }();
        if (owned && menu_on && gx2interp::mode_enabled() && !adaptive_120) {
            behind_ms = 0;
        } else if (owned && menu_on) {
            // Finish a 30 FPS frame once begun: its second (undrawn) iteration
            // runs with bit 1 of the framework flags set.
            const uint32_t flags_address = s_pacer_flag_address.load(std::memory_order_relaxed);
            const uint32_t flags = flags_address && rbrew_memory_accessible(cpu->mem, flags_address, 4)
                                       ? rbrew_read32(cpu->mem, flags_address)
                                       : 0;
            want_30 = ((flags & 3u) == 3u) || behind_ms >= kStepMs / 2;
        }
        // Only the plaza is changed; other scenes keep their own request.
        const bool plaza = rbrew_control_defaults::active_scene_type(cpu->mem) == 2;
        const uint32_t wanted = want_30 ? (request | 1u) : (request & ~1u);
        if (plaza && wanted != request) rbrew_write32(cpu->mem, object + 0x304, wanted);
        // RBREW_PLAZA_STATS=<file> (diagnostics): calls, requests and 30 FPS frames per second.
        static FILE* stats = [] {
            const char* p = std::getenv("RBREW_PLAZA_STATS");
            return p && *p ? std::fopen(p, "w") : nullptr;
        }();
        if (stats) {
            static clock::time_point second = now;
            static unsigned calls = 0, requests = 0, thirty = 0;
            ++calls;
            requests += request & 1u;
            thirty += want_30;
            if (now - second >= std::chrono::seconds(1)) {
                std::fprintf(stats, "calls %u requests %u thirty %u behind %.1f ms\n", calls, requests, thirty,
                             behind_ms);
                std::fflush(stats);
                calls = requests = thirty = 0;
                second = now;
            }
        }
    }
    s_plaza_sync_original(cpu);
}
void install_plaza_60fps(CPUState* cpu) {
#if RBREW_GAME_REVISION == 288
    (void)cpu; // revision 288 registers its own callsite (plaza_frame_rate_register)
    return;
#else
    static const bool enabled = [] {
        const char* v = std::getenv("RBREW_PLAZA_60FPS");
        return !(v && v[0] == '0');
    }();
    static bool installed = false;
    constexpr uint32_t kSync = 0x02A092A8u;
    if (!enabled || installed || !cpu || (kSync >> 2) >= cpu->dispatch_size || !cpu->dispatch_table[kSync >> 2]) return;
    installed = true;
    s_plaza_sync_original = cpu->dispatch_table[kSync >> 2];
    cpu->dispatch_table[kSync >> 2] = plaza_mode_sync;
    RBREW_LOG_INFO("gx2", "plaza 60 FPS mode sync installed\n");
#endif
}
} // namespace

// GX2SwapScanBuffers()
// Called every frame — pump SDL events, blit game FBO to window, swap buffers.
static void GX2SwapScanBuffers(CPUState* cpu) {
#if RBREW_GAME_REVISION == 288
    static const bool regulator_ready = [] {
        plaza_frame_rate_register();
        return true;
    }();
    (void)regulator_ready;
    static float s_prev_guest_work_ms = 0.0f;
    uint32_t timing_flags = 0;
    plaza_frame_rate_update(MEM, s_prev_guest_work_ms);
#endif
    install_plaza_60fps(cpu);
    // 120 FPS mode (gx2_frame_interp.h): in-between frames, the plaza
    // included since 2026-10-27. RBREW_INTERP_PLAZA=0: not in the plaza.
    static const bool interp_plaza = [] {
        const char* v = std::getenv("RBREW_INTERP_PLAZA");
        return !(v && v[0] == '0');
    }();
    if (cpu) {
        const bool plaza = rbrew_control_defaults::active_scene_type(cpu->mem) == 2;
        gx2interp::set_wanted(interp_plaza || !plaza);
        gx2vk::capture::set_plaza(plaza);
        gx2env::frame(cpu->mem, s_swap_count.load());
    }
    gx2_perf_frame();
    using frame_clock = std::chrono::steady_clock;
    static frame_clock::time_point s_prev_swap_tp{};
    const frame_clock::time_point swap_start_tp = frame_clock::now();
    check_controller_hotplug_periodic();
    if (s_swap_count.fetch_add(1) == 0) RBREW_LOG_INFO("gx2", "GX2SwapScanBuffers called for the first time\n");
    rbrew_benchmark::frame.store(s_swap_count.load(), std::memory_order_relaxed);
    rbrew_benchmark::frame_draws.store(rbrew_benchmark::guest_draws.exchange(0, std::memory_order_relaxed),
                                       std::memory_order_relaxed);
    rbrew::discord_rpc_poll_game_state(cpu ? cpu->mem : nullptr, s_swap_count.load());
#if defined(HAVE_SDL2)
    if (s_window) {
        // Pump SDL events first (the ImGui menu frees the cursor itself:
        // sync_overlay_mouse_mode).
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            gx2_imgui::process_event(ev);            // queued for the render thread
            if (gx2_imgui::menu_event(ev)) continue; // ESC menu (vk/gx2_imgui_overlay.h)
            if (ev.type == SDL_MOUSEWHEEL && gx2env::fly_camera()) {
                gx2env::fly_zoom(float(ev.wheel.y));
                continue;
            }
            switch (ev.type) {
            case SDL_QUIT: gx2_close_requested(); break;

            case SDL_KEYDOWN:
                if (ev.key.repeat) break;
                if (ev.key.keysym.scancode == SDL_SCANCODE_TAB) {
                    toggle_screen_mode();
                } else if (ev.key.keysym.scancode == SDL_SCANCODE_F11) {
                    auto settings = rbrew_settings::get();
                    settings.fullscreen = !window_fullscreen();
                    set_window_fullscreen(settings.fullscreen);
                    rbrew_settings::set(settings);
                } else if (ev.key.keysym.scancode == SDL_SCANCODE_F3) {
                    auto settings = rbrew_settings::get();
                    settings.fps_overlay = !gx2_imgui::visible();
                    set_overlay_display(settings.fps_overlay);
                    rbrew_settings::set(settings);
                    controller_log("F3 pressed: overlay visible=%d\n", gx2_imgui::visible());
                } else if (ev.key.keysym.scancode == SDL_SCANCODE_F8) {
                    SplatoonDiagnostics::ToggleManualRecord();
                    controller_log("F8 pressed: Manual recording toggled\n");
                } else if (ev.key.keysym.scancode == SDL_SCANCODE_F9) {
                    SplatoonDiagnostics::ManualCapture();
                    controller_log("F9 pressed: Diagnostic snapshot captured\n");
                } else if (ev.key.keysym.scancode == SDL_SCANCODE_F12) {
                    gx2_render_request_photo(); // photo mode: Screenshots\*.png
                } else if (ev.key.keysym.scancode == SDL_SCANCODE_F7) {
                    // Recording sessions (RBREW_PROFILE_TRIGGER): start the CPU
                    // profile now, wherever the player is.
                    if (const char* trigger = std::getenv("RBREW_PROFILE_TRIGGER"); trigger && *trigger)
                        if (FILE* f = std::fopen(trigger, "ab")) std::fclose(f);
                    controller_log("F7 pressed: CPU profile requested\n");
                }
                break;

            case SDL_CONTROLLERDEVICEADDED: on_controller_device_added(ev.cdevice.which); break;

            case SDL_CONTROLLERDEVICEREMOVED: on_controller_device_removed(ev.cdevice.which); break;

            case SDL_JOYDEVICEADDED: on_controller_device_added(ev.jdevice.which); break;

            case SDL_JOYDEVICEREMOVED: on_controller_device_removed(ev.jdevice.which); break;

            case SDL_MOUSEBUTTONDOWN:
                if (g_screen_mode == ScreenMode::Gamepad && ev.button.button == SDL_BUTTON_LEFT) {
                    map_mouse_to_touch(ev.button.x, ev.button.y);
                    g_touch_state.touched = true;
                }
                break;

            case SDL_MOUSEBUTTONUP:
                if (g_screen_mode == ScreenMode::Gamepad && ev.button.button == SDL_BUTTON_LEFT) {
                    g_touch_state.touched = false;
                }
                break;

            case SDL_MOUSEMOTION:
                if (g_screen_mode == ScreenMode::Gamepad && g_touch_state.touched) {
                    map_mouse_to_touch(ev.motion.x, ev.motion.y);
                }
                break;

            default: break;
            }
        }

        // Blit game FBO to window and swap (events stay on this thread)
        bool gamepad = (g_screen_mode == ScreenMode::Gamepad);
        if (gx2_native_async_active()) {
            struct Present final : Gx2AsyncWork {
                SDL_Window* window;
                bool gamepad;
                Present(SDL_Window* w, bool g) : window(w), gamepad(g) {}
                bool replayable() const override { return false; } // the 120 FPS replay presents itself
                void run() override {
                    struct Done {
                        ~Done() {
                            s_presents_done.fetch_add(1, std::memory_order_release);
                            s_presents_done.notify_all();
                        }
                    } done;
                    gx2_render_present(window, gamepad);
                }
            };
            // The frame must be complete before the swap returns: the game
            // reuses its per-frame buffers (display lists, vertex and uniform
            // data) once it has swapped, and its GPU timestamps cannot see
            // work still queued here. Commands within the frame stay async.
            const auto pres_start = std::chrono::steady_clock::now();
            const uint64_t posted = s_presents_posted.fetch_add(1) + 1;
            gx2_native_post(new Present(s_window, gamepad));
            Gx2PerfScope wait_scope(kPerfFrameWait);
            for (uint64_t done = s_presents_done.load(std::memory_order_acquire); done < posted;
                 done = s_presents_done.load(std::memory_order_acquire))
                s_presents_done.wait(done, std::memory_order_acquire);
            const float pres_dur =
                std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - pres_start).count();
            s_last_present_wait_ms.store(pres_dur, std::memory_order_relaxed);
        } else {
            const auto pres_start = std::chrono::steady_clock::now();
            gx2_native_run(true, [&] { gx2_render_present(s_window, gamepad); });
            const float pres_dur =
                std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - pres_start).count();
            s_last_present_wait_ms.store(pres_dur, std::memory_order_relaxed);
        }
    }
#endif
    (void)cpu;
    // RBREW_GAME_60FPS_MODE=1 (experimental): Gambit runs the plaza in a 30
    // FPS mode (bit 0 of its pacer object's +0x74) that steps the game twice
    // per frame and holds two vblanks per frame. Clearing the bit each swap
    // keeps the game in its 60 FPS mode: one step per frame, paced at 60.
    // The plaza then shows no NPCs (2026-10-25: none after 80 s): in its 30
    // FPS mode Gambit's loop (02818E7C) draws every other iteration and the
    // undrawn ones do the remaining work; drawing every iteration instead
    // left the plaza black.
    static const bool force_60 = [] {
        const char* v = std::getenv("RBREW_GAME_60FPS_MODE");
        return v && v[0] == '1';
    }();
    if (force_60 && cpu) {
        const uint32_t flag = s_pacer_flag_address.load(std::memory_order_relaxed);
        if (flag && rbrew_memory_accessible(MEM, flag, 4)) {
            const uint32_t value = rbrew_read32(MEM, flag);
            if (value & 1u) {
                static unsigned logged = 0;
                if (logged++ < 4)
                    RBREW_LOG_INFO("gx2", "game 30 FPS mode flag %08X = %08X cleared (frame %u)\n", flag, value,
                                   s_swap_count.load());
                rbrew_write32(MEM, flag, value & ~1u);
            }
        }
    }
#if RBREW_GAME_REVISION == 288
    const uint32_t timing_owner = rbrew_read32(MEM, 0x101E7BDCu);
    if (timing_owner && rbrew_memory_accessible(MEM, timing_owner + 0x304, 4))
        timing_flags = rbrew_read32(MEM, timing_owner + 0x304);
#endif
    pace_flip();
    s_flip_count.store(s_swap_count.load());
    const uint64_t flip_tick = rbrew_system_ticks();
    s_last_flip.store(flip_tick);
    const auto swap_end_tp = frame_clock::now();
    // Completion-to-completion: the current present and preceding guest/vsync
    // work belong to the same interval. The first sample has no predecessor.
    const float frame_delta_ms =
        s_prev_swap_tp == frame_clock::time_point{}
            ? 0.0f
            : float(std::chrono::duration<double, std::milli>(swap_end_tp - s_prev_swap_tp).count());
    s_prev_swap_tp = swap_end_tp;
    const float cpu_duration_ms =
        float(std::chrono::duration<double, std::milli>(frame_clock::now() - swap_start_tp).count());
    const float vsync_wait_ms = s_last_vsync_wait_ms.exchange(0.0f, std::memory_order_relaxed);
    const float present_wait_ms = s_last_present_wait_ms.exchange(0.0f, std::memory_order_relaxed);
    const float guest_work_ms = std::max(0.0f, frame_delta_ms - (vsync_wait_ms + present_wait_ms));
#if RBREW_GAME_REVISION == 288
    speed_keeper_update(MEM, frame_delta_ms, timing_flags, s_swap_interval.load(std::memory_order_relaxed));
    s_prev_guest_work_ms = guest_work_ms;
#endif

    SplatoonDiagnostics::RecordFrame(uint32_t(s_swap_count.load()), frame_delta_ms, guest_work_ms, present_wait_ms,
                                     vsync_wait_ms, cpu_duration_ms, 0, 0, 0, uint8_t(g_screen_mode));

    static FILE* benchmark_csv = [] {
        const char* path = std::getenv("RBREW_BENCHMARK_CSV");
        FILE* file = path && *path ? std::fopen(path, "w") : nullptr;
        if (file) {
            std::setvbuf(file, nullptr, _IOFBF, 65536);
            std::fprintf(
                file,
                "Frame,Scene,FrameTime_ms,GuestWork_ms,VSyncWait_ms,PresentWait_ms,Capture,Profile,Draws,Submits\n");
        }
        return file;
    }();
    const uint32_t benchmark_frame = s_swap_count.load();
    if (benchmark_csv) {
        // Vulkan queue submissions since the previous frame (read racily: statistics).
        static uint64_t s_last_submissions = 0;
        const uint64_t submissions = gx2_render_vk_submissions();
        const uint64_t frame_submissions = submissions - std::exchange(s_last_submissions, submissions);
        std::fprintf(
            benchmark_csv, "%u,%d,%.6f,%.6f,%.6f,%.6f,%u,%u,%u,%llu\n", benchmark_frame,
            rbrew_control_defaults::active_scene_type(MEM), frame_delta_ms, guest_work_ms, vsync_wait_ms,
            present_wait_ms, rbrew_benchmark::captured_frame.load(std::memory_order_relaxed) == benchmark_frame,
            unsigned(rbrew_benchmark::profiling.load(std::memory_order_relaxed)),
            rbrew_benchmark::frame_draws.load(std::memory_order_relaxed), (unsigned long long)frame_submissions);
        if (benchmark_frame % 60 == 0) std::fflush(benchmark_csv);
    }
    static const bool frame_profile = std::getenv("RBREW_FRAME_PROFILE") != nullptr;
    static uint32_t s_frame_profile_counter = 0;
    if (++s_frame_profile_counter % 60 == 0 && frame_profile) {
        RBREW_LOG_INFO_BEGIN("FRAME_PROFILE",
                             "Frame %5u: Delta=%.2fms (%.1f FPS) | GuestWork=%.2fms | VSyncWait=%.2fms | "
                             "PresentWait=%.2fms | SwapProc=%.2fms\n",
                             s_frame_profile_counter, frame_delta_ms,
                             (frame_delta_ms > 0.01f ? 1000.0f / frame_delta_ms : 0.0f), guest_work_ms, vsync_wait_ms,
                             present_wait_ms, cpu_duration_ms);
        std::fflush(stderr);
    }
#if defined(HAVE_SDL2)
    // Requests made by ImGui widgets on the render thread are applied here,
    // on the thread that owns the SDL window.
    if (s_window) {
        bool fullscreen = false;
        if (gx2_imgui::consume_fullscreen_request(fullscreen)) set_window_fullscreen(fullscreen);
        if (gx2_imgui::consume_quit_request()) gx2_close_requested();
        if (gx2_imgui::consume_menu_changed()) sync_overlay_mouse_mode();
        gx2_imgui::note_fullscreen(window_fullscreen());
        // The overlay window's close button flips visibility on the render
        // thread: sync mouse capture and persist the choice here.
        if (gx2_imgui::consume_visibility_changed()) {
            sync_overlay_mouse_mode();
            auto settings = rbrew_settings::get();
            settings.fps_overlay = gx2_imgui::visible();
            rbrew_settings::set(settings);
        }
        // The simple overlay is visual only: no cursor or input change.
        if (gx2_imgui::consume_simple_overlay_changed()) {
            auto settings = rbrew_settings::get();
            settings.simple_overlay = gx2_imgui::simple_overlay();
            rbrew_settings::set(settings);
        }
    }
#endif
}

// GX2SetTVEnable(enable)
static void GX2SetTVEnable(CPUState* cpu) {
#if defined(HAVE_SDL2)
    s_tv_enabled = (ARG0 != 0);
#else
    (void)cpu;
#endif
}

// GX2SetDRCEnable(enable)
static void GX2SetDRCEnable(CPUState* cpu) {
    (void)cpu;
}

// GX2GetSystemTVScanMode() → GX2_TV_SCAN_MODE_1080P (4)
static void GX2GetSystemTVScanMode(CPUState* cpu) {
    RET = 4;
}

// GX2GetSystemDRCScanMode() → GX2_DRC_SCAN_MODE_DISABLED (0)
static void GX2GetSystemDRCScanMode(CPUState* cpu) {
    RET = 0;
}

static void GX2SetTVGamma(CPUState* cpu) {
    (void)cpu;
}
static void GX2SetDRCScale(CPUState* cpu) {
    (void)cpu;
}
static void GX2GetLastFrame(CPUState* cpu) {
    (void)cpu;
}

static void GX2GetLastFrameGamma(CPUState* cpu) {
    cpu->f[1] = 2.2;
}

static void GX2SetDefaultState(CPUState* cpu) {
    gx2_draw_set_default_state(cpu);
}

static void GX2CalcContextStateSize(CPUState* cpu) {
    if (ARG0) rbrew_write32(MEM, ARG0, 0x1000u);
}

static void GX2SetupContextStateEx(CPUState* cpu) {
    if (!ARG0 || !rbrew_memory_accessible(MEM, ARG0, 0xA100)) {
        RBREW_LOG_ERROR("gx2", "GX2SetupContextStateEx received an invalid state address %08X\n", ARG0);
        return;
    }
    const uint32_t state = ARG0;
    const uint32_t flags = ARG1;
    GX2SetDefaultState(cpu);
    snapshot_context_state(cpu, state);
    gx2_render_save_context_state(state);
    if ((flags & 2) == 0) {
        const uint32_t size = gx2_dl_create_snapshot(cpu, state + 0x9E00, 0x300, restore_context_snapshot,
                                                     std::span<const uint8_t>(MEM + state, 0x9800));
        rbrew_write32(MEM, state + 0x9804, size);
    }
}

// Cafe's Latte/GPU7 compatibility revision, independent of the host GPU.
// GFD readers use this no-argument query to validate shader file headers.
static void GX2TempGetGPUVersion(CPUState* cpu) {
    RET = 2;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
#include "gx2_addrs.h"

#if defined(HAVE_SDL2)
// GX2SetContextState runs on the render thread (it saves the renderer's
// registers into the outgoing context and replays the incoming one). The game
// thread used to wait for the whole render queue at every switch (31% of the
// plaza's game thread once the occlusion waits were gone). It is now posted
// in order with the other commands. Its shadow snapshot list is built in the
// render thread's private builder slot (gx2_display_list.cpp): built in the
// game core's slot it captured the game's concurrently posted commands and
// hung the game. The only CPU reader of the shadow it writes,
// GX2GetContextStateDisplayList, waits for the queue first.
// RBREW_CONTEXT_STATE_SYNC=1 restores the synchronous switch.
static bool context_state_sync() {
    static const bool sync = [] {
        const char* v = std::getenv("RBREW_CONTEXT_STATE_SYNC");
        return v && v[0] == '1';
    }();
    return sync;
}
static void GX2SetContextStateCall(CPUState* cpu) {
    // Synchronous until the game has presented 120 frames: posted switches
    // during boot hung the game before its first frame in rare runs.
    if (!context_state_sync() && s_swap_count.load(std::memory_order_relaxed) >= 120 && !gx2_dl_list_open() &&
        gx2_native_async_active()) {
        gx2_dl_record(GX2SetContextState, cpu);
        return;
    }
    GX2_NATIVE_CALL(GX2SetContextState)(cpu);
}
static void GX2GetContextStateDisplayListCall(CPUState* cpu) {
    if (!context_state_sync() && gx2_native_async_active()) gx2_native_run(true, [] {});
    GX2_NATIVE_CALL(GX2GetContextStateDisplayList)(cpu);
}
#endif

void gx2_context_register(CPUState* cpu) {
#if defined(HAVE_SDL2)
#define REG(name) rbrew_register_func(cpu, ADDR_##name, GX2_NATIVE_CALL(name))
#else
#define REG(name) rbrew_register_func(cpu, ADDR_##name, gx2_command_call<name, gx2_is_command(#name)>)
#endif
    REG(GX2Init);
#if defined(HAVE_SDL2)
    rbrew_register_func(cpu, ADDR_GX2SetContextState, GX2SetContextStateCall);
    rbrew_register_func(cpu, ADDR_GX2GetContextStateDisplayList, GX2GetContextStateDisplayListCall);
#else
    REG(GX2SetContextState);
    REG(GX2GetContextStateDisplayList);
#endif
    REG(GX2DrawDone);
    REG(GX2Flush);
    REG(GX2WaitForVsync);
    REG(GX2GetSwapStatus);
    REG(GX2SetSwapInterval);
    REG(GX2CopyColorBufferToScanBuffer);
    REG(GX2SwapScanBuffers);
    REG(GX2SetTVEnable);
    REG(GX2SetDRCEnable);
    REG(GX2SetTVGamma);
    REG(GX2SetDRCScale);
    REG(GX2GetLastFrame);
    REG(GX2SetDefaultState);
    REG(GX2SetupContextStateEx);
    REG(GX2TempGetGPUVersion);
#undef REG
}

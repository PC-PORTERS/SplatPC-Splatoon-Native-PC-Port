#pragma once
// Camera trace (F10): one CSV line per presented frame while it runs, to measure how smooth fast camera turns
// really are. Off until F10; F10 again (or 60 s) stops it. Written next to the game in
// Diagnostics\camtrace-<date>.csv. Per frame:
//   timing   frame time, guest work, vsync and present waits, draws;
//   input    VPADRead calls this frame, raw mouse counts, right stick;
//   motion   what the virtual GamePad told the game: mouse degrees consumed, time steps, reported yaw rate;
//   camera   the game's main camera yaw/pitch after this frame (look-at camera hook) and its change.
// Under RenderDoc (renderdoccmd capture ...), a frame slower than RBREW_CAMTRACE_RDOC_MS (default 25 ms) during
// a trace triggers a capture of the next frame (at most 3 per trace, 1 s apart).
// Release builds compile logging out: this writes its file directly. Costs nothing while off (one atomic load).
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace rbrew_camtrace {

struct State {
    std::mutex lock;
    std::atomic<bool> on{false};
    std::atomic<int> changed{0}; // +1 started, -1 stopped (for the window title)
    FILE* file = nullptr;
    std::string path;
    uint32_t frames = 0, first_frame = 0;
    std::chrono::steady_clock::time_point start;
    // accumulated since the last frame line
    unsigned reads = 0, motions = 0;
    long mouse_x = 0, mouse_y = 0;
    float stick_rx = 0, stick_ry = 0;
    double mouse_yaw_deg = 0, mouse_pitch_deg = 0, dt_sum = 0, dt_min = 1e9, dt_max = 0, rate_max = 0;
    // camera
    bool camera = false;
    float cam_yaw = 0, cam_pitch = 0, last_yaw = 0, last_pitch = 0;
    bool have_last = false;
    int search = -1; // main camera search: 0 not a camera scene, 1 idle, 2 searching, 3 found
    // RenderDoc
    void (*trigger_capture)() = nullptr;
    unsigned captures = 0;
    uint32_t last_capture_frame = 0;
    bool capture_next_line = false;
};

inline State& state() {
    static State s;
    return s;
}

inline bool active() {
    return state().on.load(std::memory_order_relaxed);
}

inline void find_renderdoc(State& s) {
#if defined(_WIN32)
    s.trigger_capture = nullptr;
    HMODULE module = GetModuleHandleA("renderdoc.dll");
    if (!module) return;
    using GetApi = int(__cdecl*)(int, void**);
    auto get_api = reinterpret_cast<GetApi>(GetProcAddress(module, "RENDERDOC_GetAPI"));
    void** table = nullptr;
    // eRENDERDOC_API_Version_1_0_0: TriggerCapture is the 16th entry of the function table (index 15).
    if (get_api && get_api(10000, reinterpret_cast<void**>(&table)) == 1 && table)
        s.trigger_capture = reinterpret_cast<void (*)()>(table[15]);
#else
    (void)s;
#endif
}

inline void stop_locked(State& s) {
    if (s.file) {
        std::fflush(s.file);
        std::fclose(s.file);
        s.file = nullptr;
    }
    s.on.store(false, std::memory_order_relaxed);
    s.changed.store(-1, std::memory_order_relaxed);
}

// F10 (window thread): start or stop.
inline void toggle() {
    State& s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    if (s.on.load()) {
        stop_locked(s);
        return;
    }
    std::error_code error;
    std::filesystem::create_directories("Diagnostics", error);
    char name[64];
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::strftime(name, sizeof name, "Diagnostics/camtrace-%Y%m%d-%H%M%S.csv", &local);
    s.path = name;
    s.file = std::fopen(name, "w");
    if (!s.file) return;
    std::setvbuf(s.file, nullptr, _IOFBF, 1 << 16);
    std::fprintf(s.file, "frame,t_ms,scene,frame_ms,guest_ms,vsync_ms,present_ms,draws,reads,motions,mouse_dx,mouse_dy,"
                         "stick_rx,stick_ry,mouse_yaw_deg,mouse_pitch_deg,dt_sum_ms,dt_min_ms,dt_max_ms,max_rate_dps,"
                         "camera,cam_yaw_deg,cam_pitch_deg,cam_dyaw_deg,cam_dpitch_deg,cam_search,rdoc\n");
    s.frames = 0;
    s.first_frame = 0;
    s.start = std::chrono::steady_clock::now();
    s.reads = s.motions = 0;
    s.mouse_x = s.mouse_y = 0;
    s.mouse_yaw_deg = s.mouse_pitch_deg = s.dt_sum = s.dt_max = s.rate_max = 0;
    s.dt_min = 1e9;
    s.camera = s.have_last = s.capture_next_line = false;
    s.captures = 0;
    s.last_capture_frame = 0;
    find_renderdoc(s);
    s.on.store(true, std::memory_order_relaxed);
    s.changed.store(1, std::memory_order_relaxed);
}

// Game thread, each VPADRead: raw mouse counts.
inline void add_input(int mouse_dx, int mouse_dy) {
    if (!active()) return;
    State& s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    ++s.reads;
    s.mouse_x += mouse_dx;
    s.mouse_y += mouse_dy;
}

// Each virtual-GamePad step: mouse degrees consumed, time step (s), yaw rate reported to the game (deg/s),
// and the right stick the game gets (controller, keyboard or input script).
inline void add_motion(float dt, float mouse_yaw_deg, float mouse_pitch_deg, float yaw_rate_dps, float rx, float ry) {
    if (!active()) return;
    State& s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    ++s.motions;
    s.mouse_yaw_deg += mouse_yaw_deg;
    s.mouse_pitch_deg += mouse_pitch_deg;
    s.dt_sum += dt;
    if (dt < s.dt_min) s.dt_min = dt;
    if (dt > s.dt_max) s.dt_max = dt;
    if (std::fabs(yaw_rate_dps) > s.rate_max) s.rate_max = std::fabs(yaw_rate_dps);
    s.stick_rx = rx;
    s.stick_ry = ry;
}

// Camera hooks, each frame: state of the main camera search.
inline void set_search(int value) {
    if (active()) state().search = value;
}

// Look-at camera hook: the main camera's direction (radians) as the game built it.
inline void set_camera(float yaw_rad, float pitch_rad) {
    if (!active()) return;
    State& s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    s.camera = true;
    s.cam_yaw = yaw_rad * 57.29578f;
    s.cam_pitch = pitch_rad * 57.29578f;
}

// End of each presented frame (swap): writes the line and resets the per-frame sums.
inline void end_frame(uint32_t frame, int scene, float frame_ms, float guest_ms, float vsync_ms, float present_ms,
                      unsigned draws) {
    if (!active()) return;
    State& s = state();
    std::lock_guard<std::mutex> guard(s.lock);
    if (!s.file) return;
    if (!s.frames) s.first_frame = frame;
    const double t = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s.start).count();
    float dyaw = 0, dpitch = 0;
    if (s.camera && s.have_last) {
        dyaw = std::remainder(s.cam_yaw - s.last_yaw, 360.0f);
        dpitch = s.cam_pitch - s.last_pitch;
    }
    if (s.camera) {
        s.last_yaw = s.cam_yaw;
        s.last_pitch = s.cam_pitch;
        s.have_last = true;
    }
    std::fprintf(s.file,
                 "%u,%.3f,%d,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%ld,%ld,%.3f,%.3f,%.4f,%.4f,%.3f,%.3f,%.3f,%.1f,%d,%.4f,%.4f,"
                 "%.4f,%.4f,%d,%d\n",
                 frame, t, scene, frame_ms, guest_ms, vsync_ms, present_ms, draws, s.reads, s.motions, s.mouse_x,
                 s.mouse_y, s.stick_rx, s.stick_ry, s.mouse_yaw_deg, s.mouse_pitch_deg, s.dt_sum * 1000.0,
                 s.motions ? s.dt_min * 1000.0 : 0.0, s.dt_max * 1000.0, s.rate_max, int(s.camera), s.cam_yaw,
                 s.cam_pitch, dyaw, dpitch, s.search, int(s.capture_next_line));
    s.capture_next_line = false;
    s.reads = s.motions = 0;
    s.mouse_x = s.mouse_y = 0;
    s.mouse_yaw_deg = s.mouse_pitch_deg = s.dt_sum = s.dt_max = s.rate_max = 0;
    s.dt_min = 1e9;
    s.camera = false;
    // A slow frame under RenderDoc: capture the next one (what the GPU draws right after the hitch).
    static const float threshold = [] {
        const char* v = std::getenv("RBREW_CAMTRACE_RDOC_MS");
        const float f = v && *v ? float(std::atof(v)) : 25.0f;
        return f > 0 ? f : 25.0f;
    }();
    if (s.trigger_capture && frame_ms > threshold && s.captures < 3 &&
        (!s.captures || frame - s.last_capture_frame > 60)) {
        s.trigger_capture();
        ++s.captures;
        s.last_capture_frame = frame;
        s.capture_next_line = true;
    }
    if (++s.frames >= 3600)
        stop_locked(s); // 60 s at 60 FPS
    else if (s.frames % 60 == 0)
        std::fflush(s.file);
}

// Window thread: +1 just started, -1 just stopped, 0 nothing new (for the title bar).
inline int consume_change() {
    return state().changed.exchange(0, std::memory_order_relaxed);
}

} // namespace rbrew_camtrace

#include "../../runtime/rbrew_diagnostics.h"
// gx2_perf.h - opt-in frame timing. RBREW_PERF=<file> appends, every 120
// swaps, the frame rate and the wall time per frame spent in each renderer
// stage (scopes nest: "draw" includes shader/vertex/uniform/texture), byte
// counters, and the costliest native GX2 commands (address = RVA in the
// linker map; "dl" = replayed from a display list, "call" = the caller's
// whole native call including any wait for the render thread).
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

enum Gx2PerfSlot {
    kPerfDraw,
    kPerfShader,
    kPerfVertex,
    kPerfUniform,
    kPerfTexture,
    kPerfDisplayList,
    kPerfPresent,
    kPerfVsync,
    kPerfTexUpload,
    kPerfTexReadback,
    kPerfTexScan,
    kPerfTexNew,
    kPerfTexInvalidated,
    kPerfTexHash,
    kPerfShaderKey,
    kPerfIndex,
    kPerfInvalidate,
    kPerfSubmitWait,
    kPerfRenderBusy,
    kPerfFrameWait,
    kPerfGlQuery,
    kPerfAudioRender,
    kPerfAudioMix,
    kPerfAudioCallbacks,
    kPerfAudioVoiceWait,
    kPerfPipeline,
    kPerfStage,
    kPerfSlots
};
inline const char* const kGx2PerfNames[kPerfSlots] = {
    "draw",     "shader",       "vertex",          "uniform",         "texture",
    "dlist",    "present",      "vsync",           "tex_upload",      "tex_readback",
    "tex_scan", "tex_new",      "tex_invalidated", "tex_hash",        "shader_key",
    "index",    "invalidate",   "submit_wait",     "render_busy",     "frame_wait",
    "gl_query", "audio_render", "audio_mix",       "audio_callbacks", "audio_voice_wait",
    "pipeline", "stage"};
enum Gx2PerfBytes { kBytesTexHash, kBytesShaderKey, kBytesVertex, kBytesIndex, kBytesUniformBlock, kBytesSlots };
inline const char* const kGx2PerfByteNames[kBytesSlots] = {"tex_hash", "shader_key", "vertex", "index", "ubo"};
inline std::atomic<uint64_t> g_gx2_perf_ns[kPerfSlots];
inline std::atomic<uint64_t> g_gx2_perf_calls[kPerfSlots];
inline std::atomic<uint64_t> g_gx2_perf_bytes[kBytesSlots];
// Last presented frame's phase times (live overlay mode, exchanged per swap).
inline std::atomic<uint64_t> g_gx2_perf_last_ns[kPerfSlots];
// Frame epoch for cache validation: content hashes are checked once per
// presented frame (Cemu's "chronon" model) instead of once per draw. The
// game's own flush/invalidation events stay the correctness backstop.
inline std::atomic<uint64_t> g_gx2_frame_epoch{1};

// GPU draws since the last exchange: ungated (one relaxed add per draw) so
// the ImGui overlay can show the RenderDoc draw count without the profiler.
inline std::atomic<uint64_t> g_gx2_gpu_draws{0};
inline void gx2_count_gpu_draw() {
    g_gx2_gpu_draws.fetch_add(1, std::memory_order_relaxed);
}

#ifdef RBREW_DISABLE_LOGGING
// The shipped build keeps only the live overlay path: no RBREW_PERF file.
inline FILE* gx2_perf_file() {
    return nullptr;
}
#else
inline FILE* gx2_perf_file() {
    static FILE* file = [] {
        const char* path = rbrew_diagnostic_env("RBREW_PERF");
        return path && *path ? std::fopen(path, "a") : nullptr;
    }();
    return file;
}
#endif

// Live phase timings for the ImGui Performance window: the overlay turns this
// on while visible and every Gx2PerfScope then records (two clock reads);
// off, each scope pays one relaxed load and skips the clocks. An RBREW_PERF
// file keeps exclusive ownership of the counters so the two drains cannot
// fight.
inline std::atomic<bool> g_gx2_perf_live{false};
inline void gx2_set_perf_live(bool on) {
    if (on && gx2_perf_file()) on = false;
    g_gx2_perf_live.store(on, std::memory_order_relaxed);
}
inline bool gx2_perf_enabled() {
    static const bool has_file = gx2_perf_file() != nullptr;
    return has_file || g_gx2_perf_live.load(std::memory_order_relaxed);
}
inline void gx2_perf_bytes(Gx2PerfBytes slot, uint64_t bytes) {
    if (gx2_perf_enabled()) g_gx2_perf_bytes[slot].fetch_add(bytes, std::memory_order_relaxed);
}
inline void gx2_perf_count(Gx2PerfSlot slot) {
    if (gx2_perf_enabled()) g_gx2_perf_calls[slot].fetch_add(1, std::memory_order_relaxed);
}

// Per native function: a small lock-free table keyed by function address.
struct Gx2PerfFunction {
    std::atomic<uintptr_t> key{0};
    std::atomic<uint64_t> ns{0}, calls{0};
};
inline Gx2PerfFunction g_gx2_perf_functions[2][256]; // [0] replayed, [1] caller-side

struct Gx2PerfFunctionSnapshot {
    uintptr_t key = 0;
    uint64_t ns = 0;
    uint64_t calls = 0;
};
inline std::array<Gx2PerfFunctionSnapshot, 8> g_gx2_perf_last_top_dl{};

struct Gx2PerfNameEntry {
    uintptr_t fn = 0;
    const char* name = nullptr;
};
inline std::vector<Gx2PerfNameEntry>& gx2_perf_names_registry() {
    static std::vector<Gx2PerfNameEntry> names;
    return names;
}
inline void gx2_perf_register_name(const void* fn, const char* name) {
    if (!fn || !name) return;
    auto& reg = gx2_perf_names_registry();
    reg.push_back({reinterpret_cast<uintptr_t>(fn), name});
}
inline const char* gx2_perf_function_name(uintptr_t key) {
    if (key == 1) return "UniformWords";
    const auto& reg = gx2_perf_names_registry();
    for (const auto& entry : reg) {
        if (entry.fn == key) return entry.name;
    }
    return nullptr;
}
inline void gx2_perf_function(unsigned table, const void* fn, uint64_t ns) {
    const auto key = fn ? reinterpret_cast<uintptr_t>(fn) : uintptr_t(1); // 1: uniform snapshot
    for (unsigned probe = 0, i = unsigned(key >> 4) % 256; probe < 256; ++probe, i = (i + 1) % 256) {
        auto& e = g_gx2_perf_functions[table][i];
        uintptr_t seen = e.key.load(std::memory_order_acquire);
        if (!seen && e.key.compare_exchange_strong(seen, key)) seen = key;
        if (seen != key) continue;
        e.ns.fetch_add(ns, std::memory_order_relaxed);
        e.calls.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}
#if defined(_MSC_VER)
extern "C" char __ImageBase;
inline uintptr_t gx2_perf_module_base() {
    return reinterpret_cast<uintptr_t>(&__ImageBase);
}
#else
inline uintptr_t gx2_perf_module_base() {
    return 0;
}
#endif
// Optional extra report lines from the renderer (see gx2_render.cpp).
inline void (*g_gx2_perf_detail)(FILE* f, unsigned frames) = nullptr;

inline uint64_t gx2_perf_now() {
    return uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
// Time the calling thread spent waiting for the render thread.
inline thread_local uint64_t t_gx2_perf_wait_ns = 0;

struct Gx2PerfScope {
    Gx2PerfSlot slot;
    bool on;
    std::chrono::steady_clock::time_point start;
    explicit Gx2PerfScope(Gx2PerfSlot s) : slot(s), on(gx2_perf_enabled()) {
        if (on) start = std::chrono::steady_clock::now();
    }
    ~Gx2PerfScope() { stop(); }
    void stop() {
        if (!on) return;
        on = false;
        const auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        g_gx2_perf_ns[slot] += uint64_t(ns);
        ++g_gx2_perf_calls[slot];
    }
    Gx2PerfScope(const Gx2PerfScope&) = delete;
    Gx2PerfScope& operator=(const Gx2PerfScope&) = delete;
};

// Called once per GX2SwapScanBuffers, on the thread that presents.
inline void gx2_perf_frame() {
    g_gx2_frame_epoch.fetch_add(1, std::memory_order_relaxed); // cache revalidation epoch
    if (!gx2_perf_enabled()) return;
    if (!gx2_perf_file()) {
        // Live overlay mode: one exchange per presented frame so the window
        // shows per-frame phase times (an RBREW_PERF file drains instead).
        for (unsigned i = 0; i < kPerfSlots; ++i)
            g_gx2_perf_last_ns[i].store(g_gx2_perf_ns[i].exchange(0, std::memory_order_relaxed),
                                        std::memory_order_relaxed);
        std::vector<Gx2PerfFunctionSnapshot> rows;
        rows.reserve(32);
        for (auto& e : g_gx2_perf_functions[0]) {
            if (const uintptr_t key = e.key.load(std::memory_order_relaxed)) {
                const uint64_t ns = e.ns.exchange(0, std::memory_order_relaxed);
                const uint64_t calls = e.calls.exchange(0, std::memory_order_relaxed);
                if (ns > 0) rows.push_back({key, ns, calls});
            }
        }
        std::sort(rows.begin(), rows.end(),
                  [](const Gx2PerfFunctionSnapshot& a, const Gx2PerfFunctionSnapshot& b) { return a.ns > b.ns; });
        for (size_t i = 0; i < g_gx2_perf_last_top_dl.size(); ++i) {
            g_gx2_perf_last_top_dl[i] = (i < rows.size()) ? rows[i] : Gx2PerfFunctionSnapshot{};
        }
        return;
    }
#ifndef RBREW_DISABLE_LOGGING
    using clock = std::chrono::steady_clock;
    static const clock::time_point first = clock::now();
    static clock::time_point last = first;
    static unsigned frames = 0;
    static uint64_t frame_wait_start = t_gx2_perf_wait_ns;
    if (++frames < 120) return;
    const auto now = clock::now();
    const double wall_ms = std::chrono::duration<double, std::milli>(now - last).count();
    g_gx2_perf_ns[kPerfFrameWait] += t_gx2_perf_wait_ns - frame_wait_start;
    g_gx2_perf_calls[kPerfFrameWait] += frames;
    frame_wait_start = t_gx2_perf_wait_ns;
    FILE* f = gx2_perf_file();
    std::fprintf(f, "[perf] t=%.0fs %.1f fps, %.2f ms/frame |", std::chrono::duration<double>(now - first).count(),
                 frames * 1000.0 / wall_ms, wall_ms / frames);
    for (unsigned i = 0; i < kPerfSlots; ++i) {
        const uint64_t ns = g_gx2_perf_ns[i].exchange(0), calls = g_gx2_perf_calls[i].exchange(0);
        std::fprintf(f, " %s %.2fms/%llu", kGx2PerfNames[i], ns / 1e6 / frames,
                     static_cast<unsigned long long>(calls / frames));
    }
    std::fprintf(f, " | KiB/frame");
    for (unsigned i = 0; i < kBytesSlots; ++i)
        std::fprintf(f, " %s %.0f", kGx2PerfByteNames[i], g_gx2_perf_bytes[i].exchange(0) / 1024.0 / frames);
    std::fprintf(f, "\n");
    for (unsigned table = 0; table < 2; ++table) {
        struct Row {
            uintptr_t key;
            uint64_t ns, calls;
        };
        std::vector<Row> rows;
        for (auto& e : g_gx2_perf_functions[table])
            if (const uintptr_t key = e.key.load()) rows.push_back({key, e.ns.exchange(0), e.calls.exchange(0)});
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ns > b.ns; });
        std::fprintf(f, "[perf-%s]", table ? "call" : "dl");
        for (size_t i = 0; i < rows.size() && i < 14; ++i)
            std::fprintf(f, " %llX:%.2fms/%llu",
                         static_cast<unsigned long long>(rows[i].key == 1 ? 0 : rows[i].key - gx2_perf_module_base()),
                         rows[i].ns / 1e6 / frames, static_cast<unsigned long long>(rows[i].calls / frames));
        std::fprintf(f, "\n");
    }
    if (g_gx2_perf_detail) g_gx2_perf_detail(f, frames);
    std::fflush(f);
    frames = 0;
    last = now;
#endif
}

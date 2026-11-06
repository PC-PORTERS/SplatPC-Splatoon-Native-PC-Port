#include "splatoon_diagnostics.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <array>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#if !defined(_MSC_VER)
#include <strings.h> // strcasecmp
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#undef min
#undef max
#endif

namespace SplatoonDiagnostics {

// Ring buffer capacities (power of 2)
static constexpr size_t kFrameRingCapacity = 4096;
static constexpr size_t kEventRingCapacity = 2048;

// Static preallocated memory (ZERO dynamic allocation during gameplay)
static FrameTelemetry s_frame_ring[kFrameRingCapacity] = {};
static EngineEvent s_event_ring[kEventRingCapacity] = {};

static std::atomic<uint32_t> s_frame_write_index{0};
static std::atomic<uint32_t> s_event_write_index{0};

static std::atomic<bool> s_initialized{false};
static std::atomic<DiagnosticLevel> s_level{DiagnosticLevel::FlightRecorder};

// Watchdog state
static std::atomic<uint64_t> s_last_heartbeat_ns{0};
static std::atomic<bool> s_watchdog_running{false};
static std::thread s_watchdog_thread;

// Live metrics for the ImGui performance overlay (gx2_imgui_overlay.cpp).
static OverlayMetrics s_overlay_metrics{};
static std::mutex s_overlay_mutex;

// Anomaly detection state
static constexpr size_t kRollingWindow = 60;
static float s_rolling_frame_times[kRollingWindow] = {};
static size_t s_rolling_idx = 0;
static size_t s_rolling_count = 0;
static float s_rolling_avg_ms = 16.67f;
static float s_rolling_median_ms = 16.67f;

static uint32_t s_low_fps_frame_counter = 0;
static std::chrono::steady_clock::time_point s_last_stutter_capture{};
static std::chrono::steady_clock::time_point s_last_low_fps_capture{};
static std::chrono::steady_clock::time_point s_last_cluster_capture{};

// Asynchronous background report worker
static std::mutex s_capture_mutex;
static std::thread s_worker_thread;
static std::atomic<bool> s_worker_active{false};

static uint64_t now_ns() {
    return uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

static std::string get_diagnostics_base_dir() {
    // 1. Try Game/Diagnostics
    std::string path = "Diagnostics";
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (!ec) return path;

#if defined(_WIN32)
    // 2. Fallback to %LOCALAPPDATA%/Splatoon/Diagnostics
    char appdata[MAX_PATH];
    if (GetEnvironmentVariableA("LOCALAPPDATA", appdata, sizeof(appdata)) > 0) {
        std::string fallback = std::string(appdata) + "\\Splatoon\\Diagnostics";
        std::filesystem::create_directories(fallback, ec);
        if (!ec) return fallback;
    }
#endif
    return ".";
}

// ---------------------------------------------------------------------------
// Windows crash report (unhandled exceptions only)
// ---------------------------------------------------------------------------
#if defined(_WIN32)
static LPTOP_LEVEL_EXCEPTION_FILTER s_previous_filter = nullptr;

// "module+offset" per frame, unwound from the faulting context (offsets into
// Splatoon.exe resolve with Splatoon.map). Allocation free.
static int format_backtrace(CONTEXT context, char* out, size_t size) {
    int len = snprintf(out, size, "\nBacktrace:\n");
    for (int frame = 0; frame < 24 && context.Rip && len < int(size) - 160; ++frame) {
        HMODULE module = nullptr;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(context.Rip), &module))
            GetModuleFileNameA(module, path, sizeof(path));
        const char* base = strrchr(path, '\\');
        len += snprintf(out + len, size - len, "  #%02d %s+0x%llX\n", frame, base ? base + 1 : path,
                        (unsigned long long)(context.Rip - reinterpret_cast<DWORD64>(module)));
        DWORD64 image = 0;
        PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
        if (!entry) { // leaf function: the return address is on top of the stack
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 frame_pointer = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, entry, &context, &handler_data, &frame_pointer,
                         nullptr);
    }
    return len;
}

// Unhandled-exception filter, not a vectored handler: the runtime handles
// some access violations itself (__try in gx2_display_list.cpp, the hang
// sampler), and a first-chance vectored handler wrote a "crash" report for
// each of them. Only exceptions nobody handled reach this filter.
static LONG WINAPI CrashUnhandledFilter(PEXCEPTION_POINTERS ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    {
        // The process is already failing: keep this path short.
        char time_str[64];
        SYSTEMTIME st;
        GetLocalTime(&st);
        snprintf(time_str, sizeof(time_str), "%04d-%02d-%02d_%02d-%02d-%02d_crash", st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond);

        std::string base = get_diagnostics_base_dir();
        std::string crash_dir = base + "\\" + time_str;
        CreateDirectoryA(base.c_str(), nullptr);
        CreateDirectoryA(crash_dir.c_str(), nullptr);

        std::string summary_path = crash_dir + "\\summary.txt";
        HANDLE hFile =
            CreateFileA(summary_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            char buf[2048];
            int len =
                snprintf(buf, sizeof(buf),
                         "============================================================\n"
                         "SPLATOON 1 PC PORT — CRASH REPORT (AUTOMATIC CAPTURE)\n"
                         "============================================================\n"
                         "Exception Code:    0x%08X\n"
                         "Fault Address:     0x%p\n"
                         "Thread ID:         %u\n"
                         "Frame Number:      %u\n"
                         "Timestamp:         %s\n\n"
                         "Registers:\n"
                         "  RIP/EIP: 0x%p\n"
                         "  RSP/ESP: 0x%p\n"
                         "  RAX/EAX: 0x%p\n"
                         "  RCX/ECX: 0x%p\n"
                         "  RDX/EDX: 0x%p\n"
                         "  RBX/EBX: 0x%p\n\n"
                         "See performance.csv and events.jsonl for pre-crash telemetry.\n",
                         (unsigned int)code, ep->ExceptionRecord->ExceptionAddress, (unsigned int)GetCurrentThreadId(),
                         s_frame_write_index.load(), time_str, (void*)ep->ContextRecord->Rip,
                         (void*)ep->ContextRecord->Rsp, (void*)ep->ContextRecord->Rax, (void*)ep->ContextRecord->Rcx,
                         (void*)ep->ContextRecord->Rdx, (void*)ep->ContextRecord->Rbx);
            DWORD written = 0;
            WriteFile(hFile, buf, (DWORD)len, &written, nullptr);
            len = format_backtrace(*ep->ContextRecord, buf, sizeof(buf));
            WriteFile(hFile, buf, (DWORD)len, &written, nullptr);
            CloseHandle(hFile);
        }
    }
    return s_previous_filter ? s_previous_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}
#endif

// ---------------------------------------------------------------------------
// Freeze Watchdog Thread
// ---------------------------------------------------------------------------
static void WatchdogThreadFunc() {
    while (s_watchdog_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        if (!s_watchdog_running.load(std::memory_order_relaxed)) break;

        uint64_t last = s_last_heartbeat_ns.load(std::memory_order_relaxed);
        if (last == 0) continue;

        uint64_t now = now_ns();
        // If main thread has not refreshed heartbeat in > 8.0 seconds:
        if (now > last && (now - last > 8000000000ULL)) {
            static std::atomic<bool> s_freeze_captured{false};
            if (!s_freeze_captured.exchange(true)) {
                TriggerCapture("freeze_hang", "Main game loop unresponsive for > 8 seconds");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void Init() {
    if (s_initialized.exchange(true)) return;

    // Check environment override
    const char* env_diag = std::getenv("SPLATOON_DIAGNOSTICS");
    if (env_diag && (strcmp(env_diag, "0") == 0 ||
#if defined(_MSC_VER)
                     _stricmp(env_diag, "off") == 0
#else
                     strcasecmp(env_diag, "off") == 0
#endif
                     )) {
        s_level.store(DiagnosticLevel::Disabled);
        return;
    }

    s_last_heartbeat_ns.store(now_ns());

#if defined(_WIN32)
    s_previous_filter = SetUnhandledExceptionFilter(CrashUnhandledFilter);
#endif

    // Start watchdog (daemon thread)
    s_watchdog_running.store(true);
    s_watchdog_thread = std::thread(WatchdogThreadFunc);
    s_watchdog_thread.detach();

    RecordEvent(EventCategory::Engine, EventSeverity::Info, 0, "Splatoon Auto Diagnostics initialized");
}

void Shutdown() {
    if (!s_initialized.exchange(false)) return;

    s_watchdog_running.store(false);

#if defined(_WIN32)
    SetUnhandledExceptionFilter(s_previous_filter);
    s_previous_filter = nullptr;
#endif
}

bool IsEnabled() {
    return s_level.load(std::memory_order_relaxed) != DiagnosticLevel::Disabled;
}

void SetLevel(DiagnosticLevel level) {
    s_level.store(level, std::memory_order_relaxed);
}

DiagnosticLevel GetLevel() {
    return s_level.load(std::memory_order_relaxed);
}

void Heartbeat() {
    s_last_heartbeat_ns.store(now_ns(), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Fast Path Recording
// ---------------------------------------------------------------------------
void RecordEvent(EventCategory category, EventSeverity severity, int16_t code, const char* message) {
    if (!IsEnabled()) return;

    uint32_t idx = s_event_write_index.fetch_add(1, std::memory_order_relaxed) % kEventRingCapacity;
    EngineEvent& ev = s_event_ring[idx];
    ev.timestamp_ns = now_ns();
    ev.frame_index = s_frame_write_index.load(std::memory_order_relaxed);
    ev.category = category;
    ev.severity = severity;
    ev.code = code;
    std::snprintf(ev.message, sizeof(ev.message), "%s", message ? message : "");
}

namespace {
// Resident host memory of this process (0 when the platform cannot report it).
// Windows: the process working set, as before. Linux: VmRSS from
// /proc/self/status, the same resident-page figure. Only called every 30
// frames for the overlay readout, never on the per-frame hot path.
uint64_t process_resident_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                sizeof(counters)))
        return counters.WorkingSetSize;
#elif defined(__linux__)
    std::ifstream status("/proc/self/status");
    for (std::string line; status && std::getline(status, line);) {
        if (line.compare(0, 6, "VmRSS:") != 0) continue;
        const char* value = line.c_str() + 6;
        while (*value == ' ' || *value == '\t') ++value;
        char* end = nullptr;
        const unsigned long long kb = std::strtoull(value, &end, 10);
        return end != value ? uint64_t(kb) * 1024 : 0;
    }
#endif
    return 0;
}
} // namespace

void RecordFrame(uint32_t frame_index, float frame_time_ms, float cpu_time_ms, float present_wait_ms,
                 float vsync_wait_ms, float render_wait_ms, uint16_t draw_calls, uint16_t shader_compiles,
                 uint16_t texture_uploads, uint8_t screen_mode) {
    if (!IsEnabled()) return;

    Heartbeat();

    uint64_t ts = now_ns();
    uint8_t flags = 0;

    // Update rolling window
    s_rolling_frame_times[s_rolling_idx] = frame_time_ms;
    s_rolling_idx = (s_rolling_idx + 1) % kRollingWindow;
    if (s_rolling_count < kRollingWindow) s_rolling_count++;

    // Calculate rolling average
    float sum = 0.0f;
    float sorted[kRollingWindow];
    for (size_t i = 0; i < s_rolling_count; ++i) {
        sum += s_rolling_frame_times[i];
        sorted[i] = s_rolling_frame_times[i];
    }
    s_rolling_avg_ms = sum / float(s_rolling_count);
    std::sort(sorted, sorted + s_rolling_count);
    s_rolling_median_ms = sorted[s_rolling_count / 2];

    auto now_clock = std::chrono::steady_clock::now();

    // 1. Short Stutter Detection:
    // Frame time > 45ms and > 2.5x rolling median (and not a cold startup frame)
    bool is_stutter = (frame_time_ms > 45.0f) && (frame_time_ms > 2.5f * s_rolling_median_ms) && (frame_index > 120);
    if (is_stutter) {
        flags |= 1;
        if (now_clock - s_last_stutter_capture > std::chrono::seconds(30)) {
            s_last_stutter_capture = now_clock;
            TriggerCapture("stutter", "Short frame-time spike detected");
        }
    }

    // 2. Stutter Cluster Detection:
    // Count frames > 33.3ms in the rolling window
    size_t slow_frame_count = 0;
    for (size_t i = 0; i < s_rolling_count; ++i) {
        if (s_rolling_frame_times[i] > 33.33f) slow_frame_count++;
    }
    if (slow_frame_count >= 5 && (frame_index > 180)) {
        if (now_clock - s_last_cluster_capture > std::chrono::seconds(45)) {
            s_last_cluster_capture = now_clock;
            TriggerCapture("stutter_cluster", "Cluster of slow frames in rolling window");
        }
    }

    // 3. Sustained Low FPS Detection:
    // Target threshold: if rolling median is > 25ms, likely a 30 FPS scene (Plaza). Target = 22 FPS.
    // If rolling median is < 25ms, likely a 60 FPS scene. Target = 45 FPS.
    float target_threshold_fps = (s_rolling_median_ms > 25.0f) ? 22.0f : 45.0f;
    float current_fps = frame_time_ms > 0.001f ? (1000.0f / frame_time_ms) : 60.0f;
    float rolling_fps = s_rolling_avg_ms > 0.001f ? (1000.0f / s_rolling_avg_ms) : 60.0f;

    if (rolling_fps < target_threshold_fps && frame_index > 180) {
        s_low_fps_frame_counter++;
        if (s_low_fps_frame_counter >= 180) { // ~3 seconds sustained
            flags |= 2;
            if (now_clock - s_last_low_fps_capture > std::chrono::seconds(45)) {
                s_last_low_fps_capture = now_clock;
                TriggerCapture("sustained_low_fps", "Sustained performance degradation detected");
            }
        }
    } else {
        s_low_fps_frame_counter = 0;
    }

    // Write to bounded ring buffer
    uint32_t ring_idx = s_frame_write_index.fetch_add(1, std::memory_order_relaxed) % kFrameRingCapacity;
    FrameTelemetry& rec = s_frame_ring[ring_idx];
    rec.frame_index = frame_index;
    rec.timestamp_ns = ts;
    rec.frame_time_ms = frame_time_ms;
    rec.cpu_time_ms = cpu_time_ms;
    rec.present_wait_ms = present_wait_ms;
    rec.vsync_wait_ms = vsync_wait_ms;
    rec.render_wait_ms = render_wait_ms;
    rec.draw_calls = draw_calls;
    rec.shader_compiles = shader_compiles;
    rec.texture_uploads = texture_uploads;
    rec.screen_mode = screen_mode;
    rec.flags = flags;

    // Update overlay metrics
    {
        std::lock_guard<std::mutex> lock(s_overlay_mutex);
        s_overlay_metrics.current_fps = current_fps;
        s_overlay_metrics.avg_fps = rolling_fps;
        s_overlay_metrics.frame_time_ms = frame_time_ms;
        s_overlay_metrics.cpu_time_ms = cpu_time_ms;
        s_overlay_metrics.present_wait_ms = present_wait_ms;
        s_overlay_metrics.vsync_wait_ms = vsync_wait_ms;
        s_overlay_metrics.render_wait_ms = render_wait_ms;
        s_overlay_metrics.draw_calls = draw_calls;
        s_overlay_metrics.shader_compiles = shader_compiles;
        s_overlay_metrics.texture_uploads = texture_uploads;
        s_overlay_metrics.is_stuttering = is_stutter;
        s_overlay_metrics.is_low_fps = (flags & 2) != 0;

        if (s_rolling_count > 0) {
            size_t p95_idx = size_t(float(s_rolling_count - 1) * 0.95f);
            size_t p99_idx = size_t(float(s_rolling_count - 1) * 0.99f);
            size_t low1_idx = size_t(float(s_rolling_count - 1) * 0.01f);
            s_overlay_metrics.p95_frame_time_ms = sorted[p95_idx];
            s_overlay_metrics.p99_frame_time_ms = sorted[p99_idx];
            s_overlay_metrics.low_1pct_fps = sorted[low1_idx] > 0.001f ? (1000.0f / sorted[p99_idx]) : 0.0f;
        }

        // A process query per frame is unnecessary for a title readout; keep
        // the last sample when a platform has no answer.
        static uint32_t s_ram_sample = 0;
        if (s_ram_sample++ % 30 == 0)
            if (const uint64_t resident = process_resident_bytes()) s_overlay_metrics.ram_usage_bytes = resident;
    }
}

// ---------------------------------------------------------------------------
// Background Asynchronous Report Generation
// ---------------------------------------------------------------------------
struct ReportSnapshot {
    std::string reason_tag;
    std::string detail_description;
    uint32_t trigger_frame;
    uint64_t trigger_timestamp_ns;
    std::vector<FrameTelemetry> frames;
    std::vector<EngineEvent> events;
};

static void WriteReportWorker(ReportSnapshot snap) {
    // Generate folder name: YYYY-MM-DD_HH-MM-SS_<reason_tag>
    auto now = std::chrono::system_clock::now();
    auto timer = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
#if defined(_WIN32)
    localtime_s(&tm_buf, &timer);
#else
    localtime_r(&timer, &tm_buf);
#endif

    char dir_name[128];
    snprintf(dir_name, sizeof(dir_name), "%04d-%02d-%02d_%02d-%02d-%02d_%s", tm_buf.tm_year + 1900, tm_buf.tm_mon + 1,
             tm_buf.tm_mday, tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, snap.reason_tag.c_str());

    std::string base_dir = get_diagnostics_base_dir();
    std::string out_dir = base_dir + "/" + dir_name;

    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) return;

    // 1. performance.csv
    {
        std::ofstream csv(out_dir + "/performance.csv");
        csv << "Timestamp_ns,Frame,FrameTime_ms,CPU_ms,PresentWait_ms,VSyncWait_ms,RenderWait_ms,DrawCalls,"
               "ShaderCompiles,TextureUploads,ScreenMode,Flags\n";
        for (const auto& f : snap.frames) {
            csv << f.timestamp_ns << "," << f.frame_index << "," << f.frame_time_ms << "," << f.cpu_time_ms << ","
                << f.present_wait_ms << "," << f.vsync_wait_ms << "," << f.render_wait_ms << "," << f.draw_calls << ","
                << f.shader_compiles << "," << f.texture_uploads << "," << int(f.screen_mode) << "," << int(f.flags)
                << "\n";
        }
    }

    // 2. events.jsonl
    {
        std::ofstream jsl(out_dir + "/events.jsonl");
        for (const auto& e : snap.events) {
            jsl << "{\"ts\":" << e.timestamp_ns << ",\"frame\":" << e.frame_index << ",\"cat\":" << int(e.category)
                << ",\"sev\":" << int(e.severity) << ",\"code\":" << e.code << ",\"msg\":\"" << e.message << "\"}\n";
        }
    }

    // 3. summary.txt & statistics
    float max_ft = 0.0f, sum_ft = 0.0f, sum_cpu = 0.0f, sum_present = 0.0f, sum_render = 0.0f;
    std::vector<float> ft_sorted;
    for (const auto& f : snap.frames) {
        if (f.frame_time_ms > max_ft) max_ft = f.frame_time_ms;
        sum_ft += f.frame_time_ms;
        sum_cpu += f.cpu_time_ms;
        sum_present += f.present_wait_ms;
        sum_render += f.render_wait_ms;
        ft_sorted.push_back(f.frame_time_ms);
    }
    float avg_ft = snap.frames.empty() ? 0.0f : (sum_ft / float(snap.frames.size()));
    float avg_fps = avg_ft > 0.001f ? (1000.0f / avg_ft) : 0.0f;
    std::sort(ft_sorted.begin(), ft_sorted.end());
    float p50 = ft_sorted.empty() ? 0.0f : ft_sorted[ft_sorted.size() / 2];
    float p95 = ft_sorted.empty() ? 0.0f : ft_sorted[size_t(float(ft_sorted.size() - 1) * 0.95f)];
    float p99 = ft_sorted.empty() ? 0.0f : ft_sorted[size_t(float(ft_sorted.size() - 1) * 0.99f)];

    std::string dominant_bottleneck = "CPU Execution";
    if (sum_render > sum_cpu && sum_render > sum_present)
        dominant_bottleneck = "Render-Thread Wait / Vulkan Command Submission";
    else if (sum_present > sum_cpu && sum_present > sum_render)
        dominant_bottleneck = "Present Wait / GPU Synchronization";

    {
        std::ofstream sum(out_dir + "/summary.txt");
        sum << "============================================================\n"
            << "SPLATOON 1 PC PORT — AUTOMATIC DIAGNOSTIC FLIGHT RECORDER\n"
            << "============================================================\n"
            << "Anomaly Detected:      " << snap.reason_tag << "\n"
            << "Description:           " << snap.detail_description << "\n"
            << "Trigger Frame:         " << snap.trigger_frame << "\n"
            << "Frames Recorded:       " << snap.frames.size() << " (~" << (snap.frames.size() / 60) << " seconds)\n"
            << "Average FPS:           " << avg_fps << " FPS (Average frame time: " << avg_ft << " ms)\n"
            << "Frame Time P50:        " << p50 << " ms\n"
            << "Frame Time P95:        " << p95 << " ms\n"
            << "Frame Time P99:        " << p99 << " ms\n"
            << "Frame Time Max:        " << max_ft << " ms\n"
            << "Dominant Bottleneck:   " << dominant_bottleneck << "\n"
            << "============================================================\n";
    }

    // 4. timeline.json (Chrome tracing / Perfetto format)
    {
        std::ofstream tl(out_dir + "/timeline.json");
        tl << "{\"traceEvents\":[\n";
        bool first = true;
        for (const auto& f : snap.frames) {
            if (!first) tl << ",\n";
            first = false;
            uint64_t ts_us = f.timestamp_ns / 1000;
            uint64_t dur_us = uint64_t(f.frame_time_ms * 1000.0f);
            tl << "{\"name\":\"Frame\",\"cat\":\"frame\",\"ph\":\"X\",\"ts\":" << ts_us << ",\"dur\":" << dur_us
               << ",\"pid\":1,\"tid\":1,\"args\":{\"frame\":" << f.frame_index << ",\"cpu_ms\":" << f.cpu_time_ms
               << ",\"present_ms\":" << f.present_wait_ms << ",\"render_ms\":" << f.render_wait_ms << "}}";
        }
        tl << "\n]}\n";
    }

    // 5. system.json
    {
        std::ofstream sys(out_dir + "/system.json");
        sys << "{\n"
            << "  \"os\": \"Windows x64\",\n"
            << "  \"renderer\": \"Vulkan\",\n"
            << "  \"toolchain\": \"MSVC 2022 (v143)\",\n"
            << "  \"frames_analyzed\": " << snap.frames.size() << "\n"
            << "}\n";
    }

    // 6. build.json
    {
        std::ofstream bld(out_dir + "/build.json");
        bld << "{\n"
            << "  \"target\": \"Splatoon.exe\",\n"
            << "  \"configuration\": \"Release\",\n"
            << "  \"diagnostics_version\": \"2.0.0-auto\"\n"
            << "}\n";
    }

    // 7. errors.txt
    {
        std::ofstream err(out_dir + "/errors.txt");
        for (const auto& e : snap.events) {
            if (e.severity >= EventSeverity::Warning) {
                err << "[" << e.timestamp_ns << "] [frame " << e.frame_index << "] "
                    << (e.severity == EventSeverity::Critical
                            ? "CRITICAL: "
                            : (e.severity == EventSeverity::Error ? "ERROR: " : "WARN: "))
                    << e.message << " (code " << e.code << ")\n";
            }
        }
    }

    // 8. report.md
    {
        std::ofstream gem(out_dir + "/report.md");
        gem << "# Splatoon 1 PC Port — Automated Diagnostic Report\n\n"
            << "## 1. Problem Classification\n"
            << "* **Anomaly Signature:** `" << snap.reason_tag << "`\n"
            << "* **Trigger Frame:** " << snap.trigger_frame << "\n"
            << "* **Detail:** " << snap.detail_description << "\n\n"
            << "## 2. Key Telemetry Measurements\n"
            << "| Metric | Measured Value | Target Baseline |\n"
            << "|---|---:|---:|\n"
            << "| Average FPS | " << avg_fps << " FPS | 60.0 FPS |\n"
            << "| Median Frame Time (P50) | " << p50 << " ms | 16.6 ms |\n"
            << "| P95 Frame Time | " << p95 << " ms | < 20.0 ms |\n"
            << "| P99 Frame Time | " << p99 << " ms | < 33.3 ms |\n"
            << "| Peak Frame Spike | " << max_ft << " ms | < 33.3 ms |\n\n"
            << "## 3. Bottleneck Attribution\n"
            << "* **Primary Bottleneck:** " << dominant_bottleneck << "\n\n"
            << "## 4. Next Step\n"
            << "Frame times alone do not show where the time goes. Record a session with\n"
            << "`Game\\Play and Record.bat`, then run `scripts\\dev.bat analyze` for the\n"
            << "symbolized CPU profile (which functions the game and render threads spend time in).\n\n"
            << "## 5. Artifacts Attached in Directory\n"
            << "* `performance.csv`: Raw timeline telemetry.\n"
            << "* `timeline.json`: Open in `chrome://tracing` or `ui.perfetto.dev` to inspect thread execution.\n"
            << "* `events.jsonl`: Structured engine events leading up to anomaly.\n";
    }

    // Retention policy: Keep at most 10 recent report directories
    try {
        std::vector<std::filesystem::directory_entry> entries;
        for (const auto& entry : std::filesystem::directory_iterator(base_dir)) {
            if (entry.is_directory()) entries.push_back(entry);
        }
        if (entries.size() > 10) {
            std::sort(entries.begin(), entries.end(),
                      [](const auto& a, const auto& b) { return a.last_write_time() < b.last_write_time(); });
            for (size_t i = 0; i < entries.size() - 10; ++i) {
                std::filesystem::remove_all(entries[i].path());
            }
        }
    } catch (...) {}

    s_worker_active.store(false);
}

void TriggerCapture(const char* reason_tag, const char* detail_description) {
    if (!IsEnabled()) return;

    // Avoid concurrent writes
    bool expected = false;
    if (!s_worker_active.compare_exchange_strong(expected, true)) return;

    ReportSnapshot snap;
    snap.reason_tag = reason_tag ? reason_tag : "anomaly";
    snap.detail_description = detail_description ? detail_description : "Anomaly detected";
    snap.trigger_frame = s_frame_write_index.load(std::memory_order_relaxed);
    snap.trigger_timestamp_ns = now_ns();

    // Snapshot frame ring buffer (chronological order)
    uint32_t current_idx = s_frame_write_index.load(std::memory_order_relaxed);
    uint32_t count = std::min(current_idx, uint32_t(kFrameRingCapacity));
    uint32_t start = (current_idx >= kFrameRingCapacity) ? (current_idx % kFrameRingCapacity) : 0;

    snap.frames.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t idx = (start + i) % kFrameRingCapacity;
        snap.frames.push_back(s_frame_ring[idx]);
    }

    // Snapshot event ring buffer
    uint32_t ev_curr = s_event_write_index.load(std::memory_order_relaxed);
    uint32_t ev_count = std::min(ev_curr, uint32_t(kEventRingCapacity));
    uint32_t ev_start = (ev_curr >= kEventRingCapacity) ? (ev_curr % kEventRingCapacity) : 0;

    snap.events.reserve(ev_count);
    for (uint32_t i = 0; i < ev_count; ++i) {
        uint32_t idx = (ev_start + i) % kEventRingCapacity;
        snap.events.push_back(s_event_ring[idx]);
    }

    std::thread(WriteReportWorker, std::move(snap)).detach();
}

// ---------------------------------------------------------------------------
// Overlay metrics (read by the ImGui performance overlay)
// ---------------------------------------------------------------------------
OverlayMetrics GetOverlayMetrics() {
    std::lock_guard<std::mutex> lock(s_overlay_mutex);
    return s_overlay_metrics;
}

namespace {
std::atomic<void (*)(const std::string&)> s_manual_profiler{nullptr};
} // namespace

void SetManualProfiler(void (*profiler)(const std::string& path)) {
    s_manual_profiler = profiler;
}

void ManualCapture() {
    TriggerCapture("manual_snapshot", "Manual F9 snapshot triggered by user");
    if (auto profiler = s_manual_profiler.load()) {
        char name[64];
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::strftime(name, sizeof(name), "%Y-%m-%d_%H-%M-%S_threads.txt", std::localtime(&now));
        profiler(get_diagnostics_base_dir() + "/" + name);
    }
}

namespace {
std::atomic<bool> s_manual_recording{false};
} // namespace

void ToggleManualRecord() {
    const bool was_recording = s_manual_recording.load(std::memory_order_relaxed);
    s_manual_recording.store(!was_recording, std::memory_order_relaxed);
    if (!was_recording) {
        RecordEvent(EventCategory::Engine, EventSeverity::Info, 0, "Extended recording started (F8)");
    } else {
        RecordEvent(EventCategory::Engine, EventSeverity::Info, 0, "Extended recording finished (F8)");
        TriggerCapture("manual_extended_record", "Extended recording finished by user (F8)");
    }
}

bool IsManualRecording() {
    return s_manual_recording.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Local Offline Analyzer (No Internet / Paid API Needed)
// ---------------------------------------------------------------------------
bool AnalyzeReport(const std::string& report_dir, std::string& out_summary, std::string& out_report) {
    std::string csv_path = report_dir + "/performance.csv";
    std::ifstream csv(csv_path);
    if (!csv.is_open()) return false;

    std::string line;
    std::getline(csv, line); // header

    std::vector<float> frame_times;
    float sum_cpu = 0.0f, sum_present = 0.0f, sum_render = 0.0f;

    while (std::getline(csv, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string token;
        int col = 0;
        float ft = 0.0f, cpu = 0.0f, present = 0.0f, render = 0.0f;
        while (std::getline(ss, token, ',')) {
            if (col == 2)
                ft = std::strtof(token.c_str(), nullptr);
            else if (col == 3)
                cpu = std::strtof(token.c_str(), nullptr);
            else if (col == 4)
                present = std::strtof(token.c_str(), nullptr);
            else if (col == 6)
                render = std::strtof(token.c_str(), nullptr);
            col++;
        }
        if (ft > 0.0f) {
            frame_times.push_back(ft);
            sum_cpu += cpu;
            sum_present += present;
            sum_render += render;
        }
    }

    if (frame_times.empty()) return false;

    float sum_ft = 0.0f;
    for (float f : frame_times) sum_ft += f;
    float avg_ft = sum_ft / float(frame_times.size());
    float avg_fps = avg_ft > 0.001f ? (1000.0f / avg_ft) : 0.0f;

    std::sort(frame_times.begin(), frame_times.end());
    float p50 = frame_times[frame_times.size() / 2];
    float p95 = frame_times[size_t(float(frame_times.size() - 1) * 0.95f)];
    float p99 = frame_times[size_t(float(frame_times.size() - 1) * 0.99f)];

    std::string sig = "UNKNOWN";
    if (sum_render > sum_cpu * 1.5f)
        sig = "SIGNATURE_RENDER_THREAD_STARVATION";
    else if (sum_present > sum_cpu * 1.5f)
        sig = "SIGNATURE_PRESENT_VSYNC_STALL";
    else if (avg_fps < 45.0f && p95 < 35.0f)
        sig = "SIGNATURE_SUSTAINED_LOW_FPS";
    else if (p99 > p50 * 3.0f)
        sig = "SIGNATURE_FRAME_PACING_SPIKE";

    std::ostringstream ss;
    ss << "Offline Diagnostic Analysis Result:\n"
       << "  Detected Signature: " << sig << "\n"
       << "  Sampled Frames:     " << frame_times.size() << "\n"
       << "  Average FPS:        " << avg_fps << " FPS\n"
       << "  P50 / P95 / P99:    " << p50 << "ms / " << p95 << "ms / " << p99 << "ms\n";
    out_summary = ss.str();

    std::ostringstream gs;
    gs << "# Diagnostic Analysis\n\n"
       << "* **Signature:** `" << sig << "`\n"
       << "* **Average FPS:** " << avg_fps << "\n"
       << "* **P99 Frame Time:** " << p99 << " ms\n";
    out_report = gs.str();

    return true;
}

} // namespace SplatoonDiagnostics

#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace SplatoonDiagnostics {

// Configuration levels
enum class DiagnosticLevel : uint8_t {
    Disabled = 0,
    FlightRecorder = 1, // Level 0: Always-on minimal flight recorder (default)
    Trace = 2,          // Level 1: Anomaly-triggered rich tracing
    DeepDebug = 3       // Level 2: Opt-in full profiling / validation
};

enum class EventCategory : uint8_t {
    Engine = 0,
    Graphics = 1,
    Input = 2,
    Audio = 3,
    FileSystem = 4,
    Network = 5,
    Memory = 6,
    System = 7
};

enum class EventSeverity : uint8_t { Info = 0, Warning = 1, Error = 2, Critical = 3 };

// 40-byte compact telemetry record per presented frame (lock-free bounded ring buffer)
#pragma pack(push, 1)
struct FrameTelemetry {
    uint32_t frame_index;
    uint64_t timestamp_ns;
    float frame_time_ms;
    float cpu_time_ms;
    float present_wait_ms;
    float vsync_wait_ms;
    float render_wait_ms;
    uint16_t draw_calls;
    uint16_t shader_compiles;
    uint16_t texture_uploads;
    uint8_t screen_mode; // 0 = TV, 1 = GamePad
    uint8_t flags;       // 1 = Stutter, 2 = LowFPS, 4 = SceneLoad, 8 = Manual
};
#pragma pack(pop)

// 64-byte compact engine event record
#pragma pack(push, 1)
struct EngineEvent {
    uint64_t timestamp_ns;
    uint32_t frame_index;
    EventCategory category;
    EventSeverity severity;
    int16_t code;
    char message[50];
};
#pragma pack(pop)

// Live performance overlay data (read by HUD)
struct OverlayMetrics {
    float current_fps = 0.0f;
    float avg_fps = 0.0f;
    float low_1pct_fps = 0.0f;
    float frame_time_ms = 0.0f;
    float p95_frame_time_ms = 0.0f;
    float p99_frame_time_ms = 0.0f;
    float cpu_time_ms = 0.0f;
    float present_wait_ms = 0.0f;
    float vsync_wait_ms = 0.0f;
    float render_wait_ms = 0.0f;
    uint32_t draw_calls = 0;
    uint32_t shader_compiles = 0;
    uint32_t texture_uploads = 0;
    uint32_t active_controllers = 0;
    uint64_t ram_usage_bytes = 0;
    int display_width = 0;
    int display_height = 0;
    bool is_stuttering = false;
    bool is_low_fps = false;
};

// Lifecycle
void Init();
void Shutdown();

bool IsEnabled();
void SetLevel(DiagnosticLevel level);
DiagnosticLevel GetLevel();

// Ultra-fast path (< 0.05 ms) - called once per presented frame in GX2SwapScanBuffers
void RecordFrame(uint32_t frame_index, float frame_time_ms, float cpu_time_ms, float present_wait_ms,
                 float vsync_wait_ms, float render_wait_ms, uint16_t draw_calls, uint16_t shader_compiles,
                 uint16_t texture_uploads, uint8_t screen_mode);

// Fast path - thread-safe, non-blocking, zero allocation event recorder
void RecordEvent(EventCategory category, EventSeverity severity, int16_t code, const char* message);

// Triggers an asynchronous report capture (stutter, sustained low FPS, error, manual)
void TriggerCapture(const char* reason_tag, const char* detail_description);

// Heartbeat for watchdog freeze detection
void Heartbeat();

// Live metrics for the ImGui performance overlay (F3)
OverlayMetrics GetOverlayMetrics();
void ManualCapture(); // F9: Save snapshot now
// F9 also runs this (set by the executable's thread profiler) with a
// "<Diagnostics>/<time>_threads.txt" path.
void SetManualProfiler(void (*profiler)(const std::string& path));
void ToggleManualRecord(); // F8: Extended recording
bool IsManualRecording();

// Offline analyzer (local diagnosis without internet or paid APIs)
bool AnalyzeReport(const std::string& report_dir, std::string& out_summary, std::string& out_report);

} // namespace SplatoonDiagnostics

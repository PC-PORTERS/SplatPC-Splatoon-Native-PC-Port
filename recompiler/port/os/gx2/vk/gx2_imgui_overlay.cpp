// gx2_imgui_overlay.cpp - Dear ImGui interface, render thread owned.
//
// See gx2_imgui_overlay.h for the threading contract. All ImGui and backend
// calls below run on the render thread, from inside gx2vk::present; the GX2
// thread only touches the event queue, the menu/visibility flags and the
// action mailbox. Settings are read and written through rbrew_settings
// (mutex-protected), so either thread may persist them.
#include "gx2_imgui_overlay.h"
#include "gx2_graphics_menu.h"

#include "rebrewu/logger.hpp"
#include "gx2_vk.h"
#include "gx2_vk_shader.h"

#include "../../runtime/port_settings.h"
#include "../../runtime/splatoon_diagnostics.h"
#include "../../runtime/online_identity.h"
#include "../../nn_nfp/nn_nfp.h"
#include "../gx2_perf.h"
#include "../../runtime/online_identity.h"
#include <fstream>
#include <sstream>

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_vulkan.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace gx2_imgui {
namespace {
namespace rs = rbrew_settings;
namespace diag = SplatoonDiagnostics;

// ---- cross-thread state (GX2 <-> render) ----
// Both overlays start hidden; F3 (or the settings menu) brings the debug one
// up, and RBREW_FPS_OVERLAY=1 / RBREW_SIMPLE_OVERLAY=1 start them visible.
std::atomic<bool> s_visible{[] {
    const char* v = std::getenv("RBREW_FPS_OVERLAY");
    return v && v[0] != '0';
}()};
std::atomic<bool> s_visible_changed{false};
std::atomic<bool> s_simple_visible{false};
std::atomic<bool> s_simple_changed{false};
std::atomic<bool> s_menu_open{false};
std::atomic<bool> s_menu_changed{false};
std::atomic<bool> s_block_until_released{false};
std::atomic<bool> s_fullscreen_mirror{true};
std::atomic<bool> s_fullscreen_requested{false};
std::atomic<bool> s_fullscreen_value{false};
std::atomic<bool> s_borderless_mirror{false};
std::atomic<bool> s_borderless_requested{false};
std::atomic<bool> s_borderless_value{false};
std::atomic<bool> s_quit_requested{false};
std::atomic<bool> s_quit_confirm_requested{false};
std::atomic<bool> s_quit_confirm_open{false};
std::atomic<bool> s_quit_opened_from_gameplay{false};
std::atomic<bool> s_cancel_quit_confirm{false};
std::atomic<bool> s_capture_active{false};   // render thread waits for a rebinding input
std::atomic<bool> s_amiibo_requested{false}; // open the menu on the amiibo picker

std::mutex s_event_lock;
std::vector<SDL_Event> s_events;

// ---- backend state (render thread only) ----
bool s_ready = false;
VkFormat s_format = VK_FORMAT_UNDEFINED;

// Rolling frame-time history for the graph (~4 s at 60 FPS).
std::vector<float> s_history;
constexpr size_t kHistory = 240;

// Simple overlay: best FPS seen since the interface was created, and shader
// compiler activity. The totals only grow when a compile or a disk-cache hit
// actually happened, so a recent change means compilers are busy.
float s_max_fps = 0.0f;
uint64_t s_shader_compiles = 0, s_shader_hits = 0;
double s_shader_last_change = -1.0e9;
bool s_shader_compiling = false;

void update_shader_activity() {
    const uint64_t compiles = gx2vk::shader_compile_count();
    s_shader_hits = gx2vk::shader_disk_hits();
    if (compiles != s_shader_compiles) {
        s_shader_compiles = compiles;
        s_shader_last_change = ImGui::GetTime();
    }
    s_shader_compiling = ImGui::GetTime() - s_shader_last_change < 0.75;
}

enum class MenuPage { Main, Display, Keyboard, Controller, Friends, Amiibo };
// F4 (GX2 thread) asks for the Friends page: when the menu is closed it opens on that page; when it is open the
// render thread switches to it (or closes the menu when already there).
std::atomic<bool> s_friends_request{false};
std::atomic<int> s_friends_toggle{0};
MenuPage s_menu_page = MenuPage::Main;
gx2_graphics_menu::Page s_graphics_page = gx2_graphics_menu::Page::None; // graphics/weather/camera/cinema page
bool s_prev_menu_open = false;
bool s_capture = false; // waiting for the key/button of the selected action
MenuPage s_capture_page = MenuPage::Keyboard;
int s_capture_action = 0;
MenuPage s_focused_page = MenuPage::Keyboard;
int s_focused_action = -1; // binding row focused for keyboard/gamepad Clear

const char* const kResolutionLabels[rs::kResolutionCount] = {"720p (console)", "1080p", "1440p", "Display"};
// GamePad input, with what it does in Splatoon where it is fixed.
const char* const kActionLabels[rs::kActionCount] = {"A  (Confirm)",
                                                     "B ",
                                                     "X (Jump)",
                                                     "Y",
                                                     "L",
                                                     "R  (sub weapon)",
                                                     "ZL  (squid form)",
                                                     "ZR  (shoot)",
                                                     "+  (Plus)",
                                                     "-  (Minus)",
                                                     "HOME",
                                                     "D-pad up",
                                                     "D-pad down",
                                                     "D-pad left",
                                                     "D-pad right",
                                                     "Left stick click",
                                                     "Right stick click",
                                                     "Move forward",
                                                     "Move back",
                                                     "Move left",
                                                     "Move right"};

std::string key_name(int binding) {
    if (binding == rs::kUnbound) return "-";
    if (binding >= rs::kMouseBase) {
        switch (binding - rs::kMouseBase) {
        case SDL_BUTTON_LEFT: return "Mouse left";
        case SDL_BUTTON_MIDDLE: return "Mouse middle";
        case SDL_BUTTON_RIGHT: return "Mouse right";
        case SDL_BUTTON_X1: return "Mouse 4";
        default: return "Mouse 5";
        }
    }
    const char* name = SDL_GetScancodeName(SDL_Scancode(binding));
    return name && *name ? name : "Key " + std::to_string(binding);
}
std::string pad_name(int binding) {
    switch (binding) {
    case rs::kUnbound: return "-";
    case rs::kPadTriggerLeft: return "L2 (LT)";
    case rs::kPadTriggerRight: return "R2 (RT)";
    case SDL_CONTROLLER_BUTTON_A: return "Cross (A)";
    case SDL_CONTROLLER_BUTTON_B: return "Circle (B)";
    case SDL_CONTROLLER_BUTTON_X: return "Square (X)";
    case SDL_CONTROLLER_BUTTON_Y: return "Triangle (Y)";
    case SDL_CONTROLLER_BUTTON_BACK: return "Share (Back)";
    case SDL_CONTROLLER_BUTTON_GUIDE: return "PS (Guide)";
    case SDL_CONTROLLER_BUTTON_START: return "Options (Start)";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "L3";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "R3";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "L1 (LB)";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "R1 (RB)";
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-pad up";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-pad down";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-pad left";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-pad right";
    case SDL_CONTROLLER_BUTTON_MISC1: return "Mute / Share";
    default: return "Paddle " + std::to_string(binding - SDL_CONTROLLER_BUTTON_PADDLE1 + 1);
    }
}

void check_vk_result(VkResult result) {
    if (result != VK_SUCCESS) RBREW_LOG_ERROR("imgui", "Vulkan error %d\n", int(result));
}

bool backend_init(SDL_Window* window, VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  uint32_t queue_family, VkQueue queue, VkFormat format, uint32_t image_count) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Keyboard too: arrows/Tab move through the menu, Enter/Space change.
    io.ConfigFlags |=
        ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr; // no imgui.ini next to the game; layout lives for the session
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL2_InitForVulkan(window)) {
        RBREW_LOG_ERROR("imgui", "ImGui_ImplSDL2_InitForVulkan failed\n");
        ImGui::DestroyContext();
        return false;
    }

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = instance;
    info.PhysicalDevice = physical;
    info.Device = device;
    info.QueueFamily = queue_family;
    info.Queue = queue;
    info.DescriptorPoolSize = 64; // font atlas plus headroom; the backend creates the pool
    info.MinImageCount = 2;
    info.ImageCount = std::max<uint32_t>(image_count, 2);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &format;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.CheckVkResultFn = check_vk_result;
    if (!ImGui_ImplVulkan_Init(&info)) {
        RBREW_LOG_ERROR("imgui", "ImGui_ImplVulkan_Init failed\n");
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        return false;
    }
    s_format = format;
    s_ready = true;
    RBREW_LOG_INFO("imgui", "interface ready (docking branch)\n");
    return true;
}

void backend_shutdown() {
    if (!s_ready) return;
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    s_ready = false;
    s_format = VK_FORMAT_UNDEFINED;
    s_history.clear();
    s_max_fps = 0.0f;
    s_shader_compiles = gx2vk::shader_compile_count();
    s_shader_hits = gx2vk::shader_disk_hits();
    s_shader_last_change = -1.0e9;
    s_shader_compiling = false;
    s_menu_page = MenuPage::Main;
    s_graphics_page = gx2_graphics_menu::Page::None;
    s_capture = false;
    s_focused_action = -1;
    s_quit_confirm_requested.store(false);
    s_quit_confirm_open.store(false);
    s_quit_opened_from_gameplay.store(false);
    s_cancel_quit_confirm.store(false);
    s_capture_active.store(false);
}

// Binds the selected action. A key already used by another action moves
// over: that action takes this one's previous binding (a swap).
void bind_action(MenuPage page, int action, int binding) {
    auto v = rs::get();
    int* table = page == MenuPage::Keyboard ? v.keys : v.pad;
    const int count = page == MenuPage::Keyboard ? rs::kActionCount : rs::kFirstMoveAction;
    const int previous = table[action];
    for (int other = 0; other < count; ++other)
        if (other != action && binding != rs::kUnbound && table[other] == binding) table[other] = previous;
    table[action] = binding;
    rs::set(v);
}

// Rebinding capture: true when the event selected the new binding (or
// cancelled the capture with Esc/touchpad).
bool process_capture_event(const SDL_Event& e) {
    if ((e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) ||
        (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_TOUCHPAD)) {
        s_capture = false;
        return true;
    }
    if (s_capture_page == MenuPage::Keyboard) {
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            bind_action(s_capture_page, s_capture_action, int(e.key.keysym.scancode));
            s_capture = false;
            return true;
        }
        if (e.type == SDL_MOUSEBUTTONDOWN) {
            bind_action(s_capture_page, s_capture_action, rs::kMouseBase + e.button.button);
            s_capture = false;
            return true;
        }
    } else {
        if (e.type == SDL_CONTROLLERBUTTONDOWN) {
            bind_action(s_capture_page, s_capture_action, int(e.cbutton.button));
            s_capture = false;
            return true;
        }
        if (e.type == SDL_CONTROLLERAXISMOTION && e.caxis.value > 16000) {
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) {
                bind_action(s_capture_page, s_capture_action, rs::kPadTriggerLeft);
                s_capture = false;
                return true;
            }
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
                bind_action(s_capture_page, s_capture_action, rs::kPadTriggerRight);
                s_capture = false;
                return true;
            }
        }
    }
    return false;
}

// Menu shortcuts that ImGui's own navigation does not cover: Back goes up a
// page (or closes), Delete/Square clears the focused binding.
void process_nav_event(const SDL_Event& e) {
    const bool typing = ImGui::GetIO().WantTextInput; // a text box has the keyboard: Backspace edits it
    const bool back =
        (e.type == SDL_KEYDOWN && !e.key.repeat && !typing && e.key.keysym.scancode == SDL_SCANCODE_BACKSPACE) ||
        (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_B);
    if (back) {
        if (s_quit_confirm_open.load(std::memory_order_relaxed)) {
            s_cancel_quit_confirm.store(true, std::memory_order_relaxed);
            return;
        }
        if (s_menu_page == MenuPage::Amiibo)
            set_menu_open(false); // back to the game's own "touch amiibo" screen
        else if (s_graphics_page != gx2_graphics_menu::Page::None)
            s_graphics_page = gx2_graphics_menu::Page::None;
        else if (s_menu_page != MenuPage::Main)
            s_menu_page = MenuPage::Main;
        else
            set_menu_open(false);
        return;
    }
    const bool clear = (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.scancode == SDL_SCANCODE_DELETE) ||
                       (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_X);
    if (clear && s_focused_action >= 0) bind_action(s_focused_page, s_focused_action, rs::kUnbound);
}

ImVec4 fps_color(float fps) {
    if (fps >= 55.0f) return ImVec4(0.35f, 0.95f, 0.45f, 1.0f);
    if (fps >= 30.0f) return ImVec4(0.95f, 0.85f, 0.35f, 1.0f);
    return ImVec4(0.95f, 0.35f, 0.35f, 1.0f);
}

const char* present_mode_name(VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "Immediate (tearing)";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "Mailbox (low latency)";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO (vsync)";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO relaxed";
    default: return "unknown";
    }
}

void build_performance_window(const diag::OverlayMetrics& metrics, VkExtent2D extent) {
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin("Performance", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    const float fps = metrics.avg_fps > 0.0f ? metrics.avg_fps : metrics.current_fps;
    ImGui::TextColored(fps_color(fps), "%.0f FPS", fps > 0.0f ? fps : 0.0f);
    ImGui::SameLine();
    ImGui::TextDisabled("(F3 toggles)");
    ImGui::Separator();
    ImGui::Text("Frame  %.2f ms (avg)", fps > 0.0f ? 1000.0f / fps : 0.0f);
    ImGui::Text("1%% low %.0f FPS", metrics.low_1pct_fps);
    ImGui::Text("Worst   %.1f ms", metrics.p99_frame_time_ms);
    ImGui::Text("P95     %.1f ms", metrics.p95_frame_time_ms);
    if (metrics.is_stuttering) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.0f), "stutter");
    }
    if (metrics.is_low_fps) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "low fps");
    }
    if (!s_history.empty()) {
        float worst = 33.3f;
        for (float ms : s_history) worst = std::max(worst, ms);
        char legend[48];
        std::snprintf(legend, sizeof(legend), "max %.1f ms (16.7 ms target)", worst);
        ImGui::PlotLines("##frame_ms", s_history.data(), int(s_history.size()), 0, legend, 0.0f, worst,
                         ImVec2(280, 64));
    }
    ImGui::Separator();
    ImGui::Text("CPU     %.2f ms", metrics.cpu_time_ms);
    ImGui::Text("Present %.2f ms", metrics.present_wait_ms);
    ImGui::Text("VSync   %.2f ms", metrics.vsync_wait_ms);
    ImGui::Text("Render  %.2f ms", metrics.render_wait_ms);
    ImGui::Text("Draws   %u / frame", metrics.draw_calls);
    // Per-thread CPU: each thread samples its own CPU time at its frame
    // boundary; the delta between two readings gives the utilization and the
    // CPU milliseconds that thread spends per displayed frame.
    static diag::ThreadSensorReading s_prev[8];
    static size_t s_prev_count = 0;
    diag::ThreadSensorReading now[8];
    const size_t sensor_count = diag::GetThreadSensorReadings(now, 8);
    if (sensor_count) {
        ImGui::Separator();
        ImGui::TextUnformatted("Thread CPU");
        for (size_t i = 0; i < sensor_count; ++i) {
            double utilization = 0.0;
            for (size_t p = 0; p < s_prev_count; ++p) {
                if (s_prev[p].name != now[i].name || now[i].wall_ns <= s_prev[p].wall_ns) continue;
                utilization = double(now[i].cpu_ns - s_prev[p].cpu_ns) / double(now[i].wall_ns - s_prev[p].wall_ns);
                break;
            }
            const float ms_per_frame = metrics.frame_time_ms > 0.0f ? float(utilization) * metrics.frame_time_ms : 0.0f;
            ImGui::Text("  %-7s %5.1f%%  %6.1f ms/f", now[i].name, utilization * 100.0, ms_per_frame);
        }
        for (size_t i = 0; i < sensor_count; ++i) s_prev[i] = now[i];
        s_prev_count = sensor_count;
    }
    // Replay phases of the last presented frame (scopes turn on with this
    // window): where the render thread spends its time — preparation
    // (vertex/index/texture/shader), command emission (draw), GPU sync
    // (submit_wait) and present.
    {
        ImGui::Separator();
        ImGui::TextUnformatted("Render phases (last frame)");
        static const unsigned phase_slots[] = {kPerfRenderBusy, kPerfDraw,    kPerfVertex,    kPerfIndex,
                                               kPerfTexture,    kPerfTexHash, kPerfShaderKey, kPerfDisplayList,
                                               kPerfSubmitWait, kPerfPresent, kPerfPipeline,  kPerfStage};
        for (const unsigned slot : phase_slots) {
            const double ms = double(g_gx2_perf_last_ns[slot].load(std::memory_order_relaxed)) / 1e6;
            ImGui::Text("  %-12s %6.2f ms", kGx2PerfNames[slot], ms);
        }
        if (g_gx2_perf_last_top_dl[0].ns > 0) {
            ImGui::TextDisabled("  Top display list commands:");
            for (const auto& row : g_gx2_perf_last_top_dl) {
                if (!row.key || !row.ns) break;
                const double ms = double(row.ns) / 1e6;
                const char* name = gx2_perf_function_name(row.key);
                if (name) {
                    ImGui::Text("    %-20s %5.2f ms (%llu)", name, ms, (unsigned long long)row.calls);
                } else {
                    ImGui::Text("    0x%08llX           %5.2f ms (%llu)", (unsigned long long)row.key, ms,
                                (unsigned long long)row.calls);
                }
            }
        }
    }
    ImGui::Separator();
    if (metrics.ram_usage_bytes)
        ImGui::Text("RAM     %u MB", unsigned(metrics.ram_usage_bytes / (1024 * 1024)));
    else
        ImGui::TextDisabled("RAM     --"); // platform did not report it
    if (ImGui::CollapsingHeader("Backend")) {
        const auto& caps = gx2vk::caps();
        ImGui::Text("Device %s", caps.device_name.c_str());
        ImGui::Text("Vulkan %u.%u.%u, %s", VK_API_VERSION_MAJOR(caps.api_version),
                    VK_API_VERSION_MINOR(caps.api_version), VK_API_VERSION_PATCH(caps.api_version),
                    present_mode_name(gx2vk::present_mode()));
        ImGui::Text("Presents %llu, swapchain %ux%u", (unsigned long long)gx2vk::present_count(), extent.width,
                    extent.height);
        const auto mem = gx2vk::memory_usage();
        ImGui::Text("Memory device-local %.1f MB, host-only %.1f MB", mem.device_local / 1048576.0,
                    mem.host_only / 1048576.0);
        ImGui::Text("Allocated %.1f MB in %llu images", gx2vk::allocated_bytes() / 1048576.0,
                    (unsigned long long)gx2vk::image_count());
        const uint64_t errors = gx2vk::validation_error_count();
        if (errors == 0)
            ImGui::Text("Validation errors 0, warnings %llu", (unsigned long long)gx2vk::validation_warning_count());
        else {
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "Validation errors %llu, warnings %llu",
                               (unsigned long long)errors, (unsigned long long)gx2vk::validation_warning_count());
            ImGui::TextWrapped("%s", gx2vk::last_validation_error().c_str());
        }
    }
    ImGui::End();
    if (!open) set_visible(false);
}

// Purely visual readout: no title bar, no window chrome and no input, so the
// game keeps every event and the mouse stays captured.
void build_simple_overlay(const diag::OverlayMetrics& metrics) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 12.0f, viewport->WorkPos.y + 12.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    if (!ImGui::Begin("##simple_fps", nullptr, flags)) {
        ImGui::End();
        return;
    }
    const float fps = metrics.current_fps > 0.0f ? metrics.current_fps : metrics.avg_fps;
    s_max_fps = std::max(s_max_fps, fps);
    if (fps > 0.0f)
        ImGui::TextColored(fps_color(fps), "%.0f FPS", fps);
    else
        ImGui::TextDisabled("-- FPS");
    ImGui::Text("%.2f ms", metrics.frame_time_ms);
    if (s_max_fps > 0.0f)
        ImGui::Text("max %.0f FPS", s_max_fps);
    else
        ImGui::TextDisabled("max --");
    ImGui::End();
}

void build_debug_window() {
    ImGui::SetNextWindowPos(ImVec2(12, 320), ImGuiCond_FirstUseEver);
    ImGui::Begin("Debug");
    // Whether the runtime is compiling shaders right now, and how much work it
    // has done overall: this explains first-visit hitches and loading time.
    if (s_shader_compiling)
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.0f), "Shaders compiling");
    else
        ImGui::TextDisabled("Shaders idle");
    ImGui::SameLine();
    ImGui::TextDisabled("(%llu compiled, %llu cache hits)", (unsigned long long)s_shader_compiles,
                        (unsigned long long)s_shader_hits);
    ImGui::Text("Shader modules %llu", (unsigned long long)gx2vk::shader_module_count());
    bool simple = simple_overlay();
    if (ImGui::Checkbox("Simple FPS overlay", &simple)) {
        set_simple_overlay(simple);
        auto v = rs::get();
        v.simple_overlay = simple;
        rs::set(v);
    }
    ImGui::Separator();
    ImGui::TextDisabled("Diagnostics flight recorder");
    ImGui::Text("State %s", diag::IsEnabled() ? "enabled" : "disabled");
    const char* levels[] = {"Disabled", "Flight recorder", "Trace", "Deep debug"};
    int level = int(diag::GetLevel());
    if (ImGui::Combo("Level", &level, levels, 4)) diag::SetLevel(diag::DiagnosticLevel(level));
    if (ImGui::Button("Save snapshot now (F9)")) diag::ManualCapture();
    if (ImGui::Button(diag::IsManualRecording() ? "Stop extended recording" : "Start extended recording (F8)"))
        diag::ToggleManualRecord();
    ImGui::TextDisabled("Reports land in Diagnostics/.");
    ImGui::End();
}

void build_controls_page(MenuPage page) {
    const bool keyboard = page == MenuPage::Keyboard;
    ImGui::SeparatorText(keyboard ? "Mouse" : "Gyro");
    if (keyboard)
        gx2_graphics_menu::build_mouse_rows();
    else
        gx2_graphics_menu::build_gyro_rows();
    ImGui::SeparatorText(keyboard ? "Keys" : "Buttons");
    const auto v = rs::get();
    const int count = keyboard ? rs::kActionCount : rs::kFirstMoveAction;
    for (int action = 0; action < count; ++action) {
        ImGui::PushID(action);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(kActionLabels[action]);
        ImGui::SameLine(220);
        const int binding = keyboard ? v.keys[action] : v.pad[action];
        const bool capturing = s_capture && s_capture_page == page && s_capture_action == action;
        const std::string label = capturing ? "press ..." : keyboard ? key_name(binding) : pad_name(binding);
        if (ImGui::Button(label.c_str(), ImVec2(180, 0))) {
            s_capture = true;
            s_capture_page = page;
            s_capture_action = action;
        }
        if (ImGui::IsItemFocused()) {
            s_focused_page = page;
            s_focused_action = action;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) bind_action(page, action, rs::kUnbound);
        ImGui::PopID();
    }
    if (ImGui::Button("Reset to defaults")) {
        auto defaults = rs::get();
        rs::default_bindings(defaults.keys, defaults.pad);
        rs::set(defaults);
    }
    ImGui::SameLine();
    if (ImGui::Button("Back")) {
        s_menu_page = MenuPage::Main;
        s_capture = false;
    }
    ImGui::TextDisabled("%s", keyboard ? "Enter: change    Delete: clear    Esc: close"
                                       : "Cross: change    Square: clear    Circle: back");
}

// ---- Friends page (F4) ----
// The game's friend list is read from RBREW_FRIENDS_DIR by the fp stubs and filled by scripts/friends_relay.py from the
// server. This page only talks to that relay through small files in the same folder: add.cmd / remove.cmd (a pseudo),
// add.res / remove.res ("code|pseudo|text") and state.res ("friend|pid|pseudo|online" lines, "msg|text").
namespace friends_ui {
namespace fs = std::filesystem;
struct Row {
    std::string pid, name;
    bool online = false;
};
struct Request {
    std::string pid, name;
};
std::vector<Row> s_rows;
std::vector<Request> s_requests;          // friend requests received, from the "request|" lines of state.res
std::map<std::string, double> s_answered; // pid -> when it was answered here: hidden until the relay catches up
std::string s_message, s_result;
double s_read_at = -10.0;
char s_input[16] = "";
bool s_focus_input = true;

fs::path dir() {
    static const fs::path path = [] {
        const char* value = std::getenv("RBREW_FRIENDS_DIR");
        return value && *value ? fs::path(value) : fs::path();
    }();
    return path;
}

bool read_file(const fs::path& path, std::string& text) {
    std::ifstream in(path);
    if (!in) return false;
    std::ostringstream all;
    all << in.rdbuf();
    text = all.str();
    return true;
}

void refresh() {
    const double now = ImGui::GetTime();
    if (dir().empty() || now - s_read_at < 1.0) return;
    s_read_at = now;
    std::string text;
    if (read_file(dir() / "state.res", text)) {
        s_rows.clear();
        s_requests.clear();
        s_message.clear();
        std::istringstream lines(text);
        for (std::string line; std::getline(lines, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("msg|", 0) == 0) {
                s_message = line.substr(4);
                continue;
            }
            if (line.rfind("request|", 0) == 0) {
                const auto split = line.find('|', 8);
                if (split == std::string::npos) continue;
                Request request{line.substr(8, split - 8), line.substr(split + 1)};
                const auto answered = s_answered.find(request.pid);
                if (answered != s_answered.end() && now - answered->second < 15.0) continue;
                s_requests.push_back(std::move(request));
                continue;
            }
            if (line.rfind("friend|", 0) != 0) continue;
            const auto a = line.find('|', 7), b = a == std::string::npos ? a : line.find('|', a + 1);
            if (b == std::string::npos) continue;
            s_rows.push_back({line.substr(7, a - 7), line.substr(a + 1, b - a - 1), line.substr(b + 1) == "1"});
        }
        std::erase_if(s_answered, [&](const auto& entry) { return now - entry.second >= 15.0; });
    }
    for (const char* name : {"add.res", "remove.res", "accept.res", "decline.res"}) {
        if (!read_file(dir() / name, text)) continue;
        std::error_code ec;
        fs::remove(dir() / name, ec);
        const auto a = text.find('|'), b = a == std::string::npos ? a : text.find('|', a + 1);
        if (b == std::string::npos) continue;
        std::string detail = text.substr(b + 1);
        while (!detail.empty() && (detail.back() == '\n' || detail.back() == '\r')) detail.pop_back();
        s_result = text.substr(a + 1, b - a - 1) + ": " + detail;
    }
}

void send(const char* file, const std::string& pseudo) {
    if (dir().empty() || pseudo.empty()) return;
    const fs::path temporary = dir() / (std::string(file) + ".tmp");
    {
        std::ofstream out(temporary, std::ios::trunc);
        out << pseudo << "\n";
    }
    std::error_code ec;
    fs::rename(temporary, dir() / file, ec);
}

void build_page() {
    if (dir().empty()) {
        ImGui::TextWrapped("Friends need online play: start the game with PLAY (not 'Play offline').");
        if (ImGui::Button("Back")) s_menu_page = MenuPage::Main;
        return;
    }
    refresh();
    ImGui::TextDisabled("Your pseudo: %s", rbrew_online::pseudo().c_str());
    ImGui::TextWrapped(
        "Type a friend's pseudo and press Enter: they get a request and must accept it. You then see each other "
        "in the game's friend list and can join each other's rooms.");
    ImGui::Separator();
    if (s_focus_input) {
        ImGui::SetKeyboardFocusHere();
        s_focus_input = false;
    }
    const bool entered =
        ImGui::InputText("##friend_pseudo", s_input, sizeof s_input, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool clicked = ImGui::Button("Add");
    if ((entered || clicked) && s_input[0]) {
        send("add.cmd", s_input);
        s_result = "searching...";
        s_input[0] = 0;
        s_focus_input = true;
    }
    if (!s_result.empty()) ImGui::TextWrapped("%s", s_result.c_str());
    if (!s_message.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.35f, 1.0f));
        ImGui::TextWrapped("Server: %s", s_message.c_str());
        ImGui::PopStyleColor();
    }
    if (!s_requests.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Friend requests (%zu)", s_requests.size());
        for (size_t i = 0; i < s_requests.size(); ++i) {
            const Request request = s_requests[i];
            ImGui::PushID(("request_" + request.pid).c_str());
            ImGui::TextUnformatted(request.name.c_str());
            ImGui::SameLine(ImGui::GetWindowWidth() - 190.0f);
            const bool accepted = ImGui::SmallButton("Accept##request_accept");
            ImGui::SameLine();
            const bool declined = ImGui::SmallButton("Decline##request_decline");
            ImGui::PopID();
            if (accepted || declined) {
                send(accepted ? "accept.cmd" : "decline.cmd", request.pid);
                s_answered[request.pid] = ImGui::GetTime();
                s_result = request.name + (accepted ? ": accepting..." : ": declining...");
                s_requests.erase(s_requests.begin() + long(i));
                break;
            }
        }
    }
    ImGui::Separator();
    if (s_rows.empty()) ImGui::TextDisabled("No friends yet.");
    for (size_t i = 0; i < s_rows.size(); ++i) {
        ImGui::PushID(int(i));
        ImGui::TextColored(s_rows[i].online ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s %s",
                           s_rows[i].online ? "(online)" : "(offline or has not added you)", s_rows[i].name.c_str());
        ImGui::SameLine(ImGui::GetWindowWidth() - 90.0f);
        if (ImGui::SmallButton("Remove")) send("remove.cmd", s_rows[i].name);
        ImGui::PopID();
    }
    ImGui::Separator();
    if (ImGui::Button("Back")) s_menu_page = MenuPage::Main;
    ImGui::SameLine();
    ImGui::TextDisabled("F4 / Esc: close");
}
} // namespace friends_ui

// The emulated GamePad touchpoint: a file browser for amiibo dumps (.bin).
// The chosen dump is placed on the touchpoint and the game reads it through
// nn::nfp on its next poll. Starts in Game/amiibo, then where the last dump was.
struct AmiiboBrowser {
    std::filesystem::path dir = "amiibo";
    std::vector<std::filesystem::path> folders, dumps;
    bool listed = false;
    char path[1024] = "";
    std::string error;
};
AmiiboBrowser s_amiibo;

std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

void list_amiibo_dir() {
    namespace fs = std::filesystem;
    AmiiboBrowser& b = s_amiibo;
    std::error_code ec;
    fs::create_directories("amiibo", ec);
    b.dir = fs::absolute(b.dir, ec);
    b.folders.clear();
    b.dumps.clear();
    for (fs::directory_iterator it(b.dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->is_directory(ec))
            b.folders.push_back(it->path());
        else if (it->path().extension() == ".bin" || it->path().extension() == ".BIN")
            b.dumps.push_back(it->path());
    }
    std::sort(b.folders.begin(), b.folders.end());
    std::sort(b.dumps.begin(), b.dumps.end());
    b.listed = true;
}

void place_amiibo(const std::filesystem::path& file) {
    rbrew_nfp::Tag tag;
    if (!rbrew_nfp::load_dump(file, tag, s_amiibo.error)) return;
    s_amiibo.error.clear();
    s_amiibo.dir = file.parent_path();
    s_amiibo.listed = false;
    if (rbrew_nfp::touch(tag)) set_menu_open(false);
}

void build_amiibo_page() {
    if (!rbrew_nfp::searching()) { // the game stopped waiting (or already read an amiibo)
        set_menu_open(false);
        return;
    }
    AmiiboBrowser& b = s_amiibo;
    if (!b.listed || ImGui::IsWindowAppearing()) list_amiibo_dir();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::TextWrapped("Choose an amiibo dump (.bin) to place on the GamePad's NFC touchpoint.");
    ImGui::TextDisabled("%s", utf8(b.dir).c_str());
    ImGui::SetNextItemWidth(width - ImGui::CalcTextSize("Open").x - ImGui::GetStyle().FramePadding.x * 2 -
                            ImGui::GetStyle().ItemSpacing.x);
    const bool typed = ImGui::InputTextWithHint("##amiibo_path", "or paste a file path", b.path, sizeof(b.path),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Open") || typed) && b.path[0]) {
        // Quotes from Explorer's "Copy as path".
        std::string text = b.path;
        std::erase(text, '"');
        place_amiibo(std::filesystem::path(std::u8string(text.begin(), text.end())));
    }
    if (!b.error.empty()) ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1.0f), "%s", b.error.c_str());
    ImGui::Separator();
    if (ImGui::BeginChild("##amiibo_files", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 4))) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        std::filesystem::path next;
        if (b.dir.has_parent_path() && b.dir.parent_path() != b.dir && ImGui::Selectable("[..]"))
            next = b.dir.parent_path();
        for (const auto& folder : b.folders)
            if (ImGui::Selectable(("[" + utf8(folder.filename()) + "]").c_str())) next = folder;
        for (const auto& dump : b.dumps)
            if (ImGui::Selectable(utf8(dump.filename()).c_str())) place_amiibo(dump);
        if (b.folders.empty() && b.dumps.empty()) ImGui::TextDisabled("No .bin files in this folder.");
        if (!next.empty()) {
            b.dir = next;
            list_amiibo_dir();
        }
    }
    ImGui::EndChild();
    if (ImGui::Button("Back", ImVec2(width, 0))) set_menu_open(false);
}

// ---- Friends page (F4) ----
// The game's friend list is read from RBREW_FRIENDS_DIR by the fp stubs and filled by scripts/friends_relay.py from the
// server. This page only talks to that relay through small files in the same folder: add.cmd / remove.cmd (a pseudo),
// add.res / remove.res ("code|pseudo|text") and state.res ("friend|pid|pseudo|online" lines, "msg|text").

const char* menu_title(MenuPage page) {
    switch (page) {
    case MenuPage::Display: return "Display";
    case MenuPage::Keyboard: return "Mouse & keyboard";
    case MenuPage::Controller: return "Controller";
    case MenuPage::Friends: return "Friends";
    case MenuPage::Amiibo: return "amiibo";
    default: return "Settings";
    }
}

// Display page: the window, the frame rate and the FPS counters.
void build_display_page() {
    auto v = rs::get();
    bool dirty = false;
    ImGui::SeparatorText("Window");
    bool fullscreen = s_fullscreen_mirror.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Fullscreen", &fullscreen)) {
        s_fullscreen_value.store(fullscreen, std::memory_order_relaxed);
        s_fullscreen_requested.store(true, std::memory_order_relaxed);
        v.fullscreen = fullscreen;
        if (fullscreen && v.borderless) {
            v.borderless = false;
            s_borderless_mirror.store(false, std::memory_order_relaxed);
        }
        dirty = true;
    }
    bool borderless = s_borderless_mirror.load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Borderless window", &borderless)) {
        s_borderless_value.store(borderless, std::memory_order_relaxed);
        s_borderless_requested.store(true, std::memory_order_relaxed);
        s_borderless_mirror.store(borderless, std::memory_order_relaxed);
        v.borderless = borderless;
        if (borderless) {
            v.fullscreen = false;
            s_fullscreen_mirror.store(false, std::memory_order_relaxed);
        }
        dirty = true;
    }
    int resolution = v.resolution;
    if (ImGui::Combo("Render resolution", &resolution, kResolutionLabels, rs::kResolutionCount)) {
        v.resolution = resolution;
        dirty = true;
    }
    if (v.resolution != rs::active_resolution()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(after restart)");
    }
    ImGui::SeparatorText("Frame rate");
    dirty |= gx2_graphics_menu::build_frame_rows(v);
    dirty |= ImGui::Checkbox("Plaza 60 FPS", &v.plaza_60fps);
    ImGui::SeparatorText("FPS counter");
    bool overlay = visible();
    if (ImGui::Checkbox("Performance overlay (F3)", &overlay)) {
        set_visible(overlay);
        v.fps_overlay = overlay;
        dirty = true;
    }
    bool simple = simple_overlay();
    if (ImGui::Checkbox("Simple FPS overlay", &simple)) {
        set_simple_overlay(simple);
        v.simple_overlay = simple;
        dirty = true;
    }
    if (dirty) rs::set(v);
    ImGui::Spacing();
    if (ImGui::Button("Back")) s_menu_page = MenuPage::Main;
}

void build_settings_window() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 center(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                        viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 640), ImGuiCond_FirstUseEver);
    bool open = true;
    // One window ID for every page (the title shows the page).
    const char* title = s_graphics_page != gx2_graphics_menu::Page::None ? gx2_graphics_menu::title(s_graphics_page)
                                                                         : menu_title(s_menu_page);
    const std::string window = std::string(title) + "###settings";
    if (!ImGui::Begin(window.c_str(), &open)) {
        ImGui::End();
        return;
    }
    if (s_graphics_page != gx2_graphics_menu::Page::None) {
        if (gx2_graphics_menu::build_page(s_graphics_page)) s_graphics_page = gx2_graphics_menu::Page::None;
        ImGui::End();
        if (!open) set_menu_open(false);
        return;
    }
    if (s_menu_page == MenuPage::Friends) {
        friends_ui::build_page();
        ImGui::End();
        if (!open) set_menu_open(false);
        return;
    }
    if (s_menu_page == MenuPage::Amiibo) {
        build_amiibo_page();
        ImGui::End();
        if (!open) set_menu_open(false);
        return;
    }
    if (s_menu_page == MenuPage::Display) {
        build_display_page();
        ImGui::End();
        if (!open) set_menu_open(false);
        return;
    }
    if (s_menu_page != MenuPage::Main) {
        build_controls_page(s_menu_page);
        ImGui::End();
        if (!open) set_menu_open(false);
        return;
    }
    auto v = rs::get();
    bool dirty = false;
    if (ImGui::Button("Resume")) set_menu_open(false);
    if (rbrew_online::config().online) {
        ImGui::SameLine();
        if (ImGui::Button("Friends (F4)")) {
            s_menu_page = MenuPage::Friends;
            friends_ui::s_focus_input = true;
        }
    }
    if (rbrew_nfp::searching()) {
        ImGui::SameLine();
        if (ImGui::Button("Scan amiibo")) s_menu_page = MenuPage::Amiibo;
    }
    // One button per page, two per row.
    ImGui::SeparatorText("Settings");
    const float tile_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    int tiles = 0;
    const auto tile = [&](const char* label) {
        if (tiles++ % 2) ImGui::SameLine();
        return ImGui::Button(label, ImVec2(tile_width, 0));
    };
    const auto menu_tile = [&](MenuPage page) {
        if (tile(menu_title(page))) s_menu_page = page;
    };
    const auto graphics_tile = [&](gx2_graphics_menu::Page page) {
        if (gx2_graphics_menu::available(page) && tile(gx2_graphics_menu::title(page))) s_graphics_page = page;
    };
    menu_tile(MenuPage::Display);
    graphics_tile(gx2_graphics_menu::Page::Graphics);
    menu_tile(MenuPage::Keyboard);
    menu_tile(MenuPage::Controller);
    graphics_tile(gx2_graphics_menu::Page::GamePad);
    graphics_tile(gx2_graphics_menu::Page::Camera);
    graphics_tile(gx2_graphics_menu::Page::Weather);
    graphics_tile(gx2_graphics_menu::Page::Cinema);
    ImGui::SeparatorText("General");
    dirty |= ImGui::SliderInt("Volume", &v.volume, 0, 100, "%d %%");
    dirty |= ImGui::Checkbox("Confirm before quitting", &v.confirm_quit);
    ImGui::Spacing();
    if (ImGui::Button("Quit game")) {
        open_quit_confirmation();
    }
    if (dirty) rs::set(v);
    ImGui::End();
    if (!open) set_menu_open(false);
}

void build_quit_confirmation_popup() {
    static bool s_dont_ask_again = false;

    if (s_quit_confirm_requested.exchange(false, std::memory_order_relaxed)) {
        s_dont_ask_again = false;
        ImGui::OpenPopup("Quit Game?###quit_confirm");
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Quit Game?###quit_confirm", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        s_quit_confirm_open.store(true, std::memory_order_relaxed);

        ImGui::Text("Are you sure you want to quit?");
        ImGui::Spacing();
        ImGui::Checkbox("Don't ask again", &s_dont_ask_again);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float button_width = 120.0f;
        if (ImGui::Button("Quit", ImVec2(button_width, 0))) {
            if (s_dont_ask_again) {
                auto settings = rs::get();
                settings.confirm_quit = false;
                rs::set(settings);
            }
            s_quit_requested.store(true, std::memory_order_relaxed);
            s_quit_confirm_open.store(false, std::memory_order_relaxed);
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        const bool cancel_clicked = ImGui::Button("Cancel", ImVec2(button_width, 0));
        ImGui::SetItemDefaultFocus();

        const bool cancel_requested =
            cancel_clicked || s_cancel_quit_confirm.exchange(false, std::memory_order_relaxed) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight);

        if (cancel_requested) {
            s_quit_confirm_open.store(false, std::memory_order_relaxed);
            ImGui::CloseCurrentPopup();
            if (s_quit_opened_from_gameplay.exchange(false, std::memory_order_relaxed)) {
                set_menu_open(false);
            }
        }

        ImGui::EndPopup();
    } else {
        if (!s_quit_confirm_requested.load(std::memory_order_relaxed))
            s_quit_confirm_open.store(false, std::memory_order_relaxed);
    }
}

} // namespace

void process_event(const SDL_Event& event) {
    std::lock_guard lock(s_event_lock);
    if (s_events.size() < 256) s_events.push_back(event);
}

bool menu_event(const SDL_Event& event) {
    if (event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == SDL_CONTROLLER_BUTTON_TOUCHPAD) {
        if (s_quit_confirm_open.load(std::memory_order_relaxed)) {
            s_cancel_quit_confirm.store(true, std::memory_order_relaxed);
            return true;
        }
        if (!s_capture_active.load(std::memory_order_relaxed)) set_menu_open(!menu_open());
        return true; // in capture mode the render thread cancels it from the queued copy
    }
    if (event.type == SDL_KEYDOWN && !event.key.repeat && event.key.keysym.scancode == SDL_SCANCODE_F4) {
        if (!s_capture_active.load(std::memory_order_relaxed)) {
            if (menu_open())
                s_friends_toggle.fetch_add(1, std::memory_order_relaxed);
            else {
                s_friends_request.store(true, std::memory_order_relaxed);
                set_menu_open(true);
            }
        }
        return true;
    }
    if (event.type == SDL_KEYDOWN && !event.key.repeat && event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
        if (s_quit_confirm_open.load(std::memory_order_relaxed)) {
            s_cancel_quit_confirm.store(true, std::memory_order_relaxed);
            return true;
        }
        if (!s_capture_active.load(std::memory_order_relaxed)) set_menu_open(!menu_open());
        return true;
    }
    return menu_open() && (event.type == SDL_KEYDOWN || event.type == SDL_CONTROLLERBUTTONDOWN);
}

void set_menu_open(bool open) {
    if (s_menu_open.exchange(open, std::memory_order_relaxed) == open) return;
    s_menu_changed.store(true, std::memory_order_relaxed);
    if (!open) s_block_until_released.store(true, std::memory_order_relaxed);
}

bool menu_open() {
    return s_menu_open.load(std::memory_order_relaxed);
}

void open_amiibo_picker() {
    s_amiibo_requested.store(true, std::memory_order_relaxed);
    set_menu_open(true);
}

void open_quit_confirmation() {
    if (!rs::g_confirm_quit.load(std::memory_order_relaxed)) {
        s_quit_requested.store(true, std::memory_order_relaxed);
        return;
    }
    if (!menu_open()) s_quit_opened_from_gameplay.store(true, std::memory_order_relaxed);
    s_quit_confirm_requested.store(true, std::memory_order_relaxed);
    s_quit_confirm_open.store(true, std::memory_order_relaxed);
    set_menu_open(true);
}

bool is_quit_confirmation_open() {
    return s_quit_confirm_open.load(std::memory_order_relaxed);
}

bool consume_menu_changed() {
    return s_menu_changed.exchange(false, std::memory_order_relaxed);
}

bool menu_blocks_input(bool any_input_held) {
    // The press that closed the menu must not reach the game, but the block
    // never outlasts half a second: a stick resting off centre (drift) or a
    // key held on purpose kept it forever and the keyboard seemed dead.
    static std::chrono::steady_clock::time_point closed_at;
    static bool was_open = false;
    if (s_menu_open.load(std::memory_order_relaxed)) {
        was_open = true;
        return true;
    }
    if (was_open) {
        closed_at = std::chrono::steady_clock::now();
        was_open = false;
    }
    const bool blocked = s_block_until_released.load(std::memory_order_relaxed);
    if (blocked && (!any_input_held || std::chrono::steady_clock::now() - closed_at > std::chrono::milliseconds(500)))
        s_block_until_released.store(false, std::memory_order_relaxed);
    return blocked;
}

bool consume_fullscreen_request(bool& fullscreen) {
    if (!s_fullscreen_requested.exchange(false, std::memory_order_relaxed)) return false;
    fullscreen = s_fullscreen_value.load(std::memory_order_relaxed);
    return true;
}

bool consume_quit_request() {
    return s_quit_requested.exchange(false, std::memory_order_relaxed);
}

bool consume_borderless_request(bool& borderless) {
    if (!s_borderless_requested.exchange(false, std::memory_order_relaxed)) return false;
    borderless = s_borderless_value.load(std::memory_order_relaxed);
    return true;
}

void note_borderless(bool borderless) {
    s_borderless_mirror.store(borderless, std::memory_order_relaxed);
}

void note_fullscreen(bool fullscreen) {
    s_fullscreen_mirror.store(fullscreen, std::memory_order_relaxed);
}

void set_visible(bool visible) {
    if (s_visible.exchange(visible, std::memory_order_relaxed) != visible)
        s_visible_changed.store(true, std::memory_order_relaxed);
}

bool visible() {
    return s_visible.load(std::memory_order_relaxed);
}

bool consume_visibility_changed() {
    return s_visible_changed.exchange(false, std::memory_order_relaxed);
}

void set_simple_overlay(bool visible) {
    if (s_simple_visible.exchange(visible, std::memory_order_relaxed) != visible)
        s_simple_changed.store(true, std::memory_order_relaxed);
}

bool simple_overlay() {
    return s_simple_visible.load(std::memory_order_relaxed);
}

bool consume_simple_overlay_changed() {
    return s_simple_changed.exchange(false, std::memory_order_relaxed);
}

bool ensure_ready(SDL_Window* window, VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  uint32_t queue_family, VkQueue queue, VkFormat format, uint32_t image_count) {
    if (!window) return false;
    if (s_ready && s_format == format) return true;
    // A swapchain recreation with another format needs a new pipeline.
    backend_shutdown();
    return backend_init(window, instance, physical, device, queue_family, queue, format, image_count);
}

ImDrawData* begin_frame(VkExtent2D extent) {
    if (!s_ready) return nullptr;
    const bool show_menu = menu_open();
    const bool show_simple = simple_overlay();
    if (!visible() && !show_menu && !show_simple) {
        s_capture_active.store(false, std::memory_order_relaxed);
        return nullptr;
    }
    std::vector<SDL_Event> events;
    {
        std::lock_guard lock(s_event_lock);
        events.swap(s_events);
    }
    // Raw shortcuts first (previous frame's state): rebinding capture, then
    // the Back/Clear keys the gamepad/keyboard navigation does not cover.
    for (const auto& event : events) {
        if (!show_menu) break;
        if (s_capture) {
            if (process_capture_event(event)) continue;
        } else {
            process_nav_event(event);
        }
    }
    if (!show_menu) s_capture = false; // closed from the GX2 thread mid-capture
    for (const auto& event : events) ImGui_ImplSDL2_ProcessEvent(&event);

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // Keep the interface readable from 720p to 4K.
    ImGui::GetIO().FontGlobalScale = std::clamp(float(extent.height) / 1080.0f, 1.0f, 2.0f);

    const bool fresh_menu = show_menu && !s_prev_menu_open;
    if (show_menu != s_prev_menu_open)
        diag::RecordEvent(diag::EventCategory::Input, diag::EventSeverity::Info, 10,
                          show_menu ? "menu opened" : "menu closed");
    s_prev_menu_open = show_menu;
    if (fresh_menu) {
        const bool friends_page = s_friends_request.exchange(false, std::memory_order_relaxed);
        s_menu_page = friends_page ? MenuPage::Friends : MenuPage::Main;
        if (friends_page) friends_ui::s_focus_input = true;
        s_graphics_page = gx2_graphics_menu::Page::None;
        s_capture = false;
    }
    if (s_friends_toggle.exchange(0, std::memory_order_relaxed) > 0 && show_menu && !fresh_menu) {
        if (s_menu_page == MenuPage::Friends)
            set_menu_open(false);
        else {
            s_menu_page = MenuPage::Friends;
            s_graphics_page = gx2_graphics_menu::Page::None;
            s_capture = false;
            friends_ui::s_focus_input = true;
        }
    }
    if (show_menu && s_amiibo_requested.exchange(false, std::memory_order_relaxed)) {
        diag::RecordEvent(diag::EventCategory::Input, diag::EventSeverity::Info, 11, "amiibo picker shown");
        s_menu_page = MenuPage::Amiibo;
        s_graphics_page = gx2_graphics_menu::Page::None;
        s_capture = false;
    }
    update_shader_activity();
    s_focused_action = -1;
    if (show_menu && !s_quit_opened_from_gameplay.load(std::memory_order_relaxed)) build_settings_window();
    build_quit_confirmation_popup();
    // Phase scopes (gx2_perf.h) record while this window is visible; off they
    // cost one relaxed load per scope. An RBREW_PERF file keeps them anyway.
    gx2_set_perf_live(visible());
    if (visible() || show_simple) {
        const auto metrics = diag::GetOverlayMetrics();
        if (visible()) {
            if (metrics.frame_time_ms > 0.0f) {
                s_history.push_back(metrics.frame_time_ms);
                if (s_history.size() > kHistory) s_history.erase(s_history.begin());
            }
            build_performance_window(metrics, extent);
            build_debug_window();
        }
        if (show_simple) build_simple_overlay(metrics);
    }
    s_capture_active.store(s_capture && show_menu, std::memory_order_relaxed);
    ImGui::Render();
    return ImGui::GetDrawData();
}

void render_draw_data(ImDrawData* draw_data, VkCommandBuffer cb, VkImage target, VkImageView target_view,
                      VkExtent2D extent) {
    if (!draw_data || draw_data->CmdListsCount == 0) return;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = target;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cb, &rendering);
    ImGui_ImplVulkan_RenderDrawData(draw_data, cb);
    vkCmdEndRendering(cb);

    std::swap(barrier.oldLayout, barrier.newLayout);
    std::swap(barrier.srcAccessMask, barrier.dstAccessMask);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
}

void shutdown() {
    backend_shutdown();
}

} // namespace gx2_imgui

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
#include "../gx2_detile.h"

#include "../../runtime/port_settings.h"
#include "../../runtime/splatoon_diagnostics.h"
#include "../../runtime/mod_loader.h"
#include "../../runtime/online_identity.h"
#include "../../online/online_boot.h"
#include "../../nn_nfp/nn_nfp.h"
#include "../gx2_env_light.h"
#include "../gx2_perf.h"

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_vulkan.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
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
std::atomic<bool> s_capture_active{false};     // render thread waits for a rebinding input
std::atomic<bool> s_amiibo_requested{false};   // open the menu on the amiibo picker
std::atomic<uint64_t> s_mod_page_requested{0}; // open the menu directly on a mod page
std::atomic<bool> s_startup_login_open{false}; // startup ImGui Online Account Login modal

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

enum class MenuPage { Main, Display, Keyboard, Controller, Friends, OnlineLogin, Amiibo, Mods, ModPage };
// F4 (GX2 thread) asks for the Friends page: when the menu is closed it opens on that page; when it is open the
// render thread switches to it (or closes the menu when already there).
std::atomic<bool> s_friends_request{false};
std::atomic<int> s_friends_toggle{0};
MenuPage s_menu_page = MenuPage::Main;
uint64_t s_selected_mod_page = 0;
bool s_mod_page_direct = false;
bool s_mod_back_pressed = false;
bool s_focus_direct_canvas = false;
std::string s_mod_toggle_error;
std::string s_confirm_delete_mod_id;
uint64_t s_texture_epoch = 1;
ImFont* s_display_font = nullptr;
float s_menu_anim = 0.0f;
std::unordered_map<ImGuiID, float> s_nav_hover;

struct ModTextureEntry {
    gx2vk::Image image;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
};
std::unordered_map<uint64_t, ModTextureEntry> s_mod_textures;

void clear_mod_textures() {
    for (auto& [id, entry] : s_mod_textures) {
        if (s_ready && entry.descriptor) ImGui_ImplVulkan_RemoveTexture(entry.descriptor);
        if (entry.image) gx2vk::destroy_image(entry.image);
    }
    s_mod_textures.clear();
    ++s_texture_epoch;
}
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

static Uint32 s_sdl_window_id = 0;

void check_vk_result(VkResult result) {
    if (result != VK_SUCCESS) RBREW_LOG_ERROR("imgui", "Vulkan error %d\n", int(result));
}

bool backend_init(SDL_Window* window, VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  uint32_t queue_family, VkQueue queue, VkFormat format, uint32_t image_count) {
    s_sdl_window_id = window ? SDL_GetWindowID(window) : 0;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Keyboard too: arrows/Tab move through the menu, Enter/Space change.
    io.ConfigFlags |=
        ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_DockingEnable;
    io.ConfigNavCursorVisibleAlways = true;
    io.IniFilename = nullptr; // no imgui.ini next to the game; layout lives for the session
    ImGui::StyleColorsDark();
    io.Fonts->AddFontDefault();
    s_display_font = nullptr;

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
    info.DescriptorPoolSize = 128; // font atlas + mod textures + headroom
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
    clear_mod_textures();
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
    s_menu_anim = 0.0f;
    s_nav_hover.clear();
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
        else if (s_menu_page == MenuPage::ModPage && s_mod_page_direct)
            s_mod_back_pressed = true; // delegate screen-level back to direct custom mod UI
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
    (void)page;
    static int s_last_controls_frame = -10;
    const int frame = ImGui::GetFrameCount();
    const bool force_select = (frame - s_last_controls_frame) > 1;
    s_last_controls_frame = frame;

    if (ImGui::BeginTabBar("##controls_tabs")) {
        const ImGuiTabItemFlags kb_flags =
            (force_select && s_menu_page == MenuPage::Keyboard) ? ImGuiTabItemFlags_SetSelected : 0;
        const ImGuiTabItemFlags pad_flags =
            (force_select && s_menu_page == MenuPage::Controller) ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem("Mouse & keyboard", nullptr, kb_flags)) {
            if (!force_select && s_menu_page != MenuPage::Keyboard) {
                s_menu_page = MenuPage::Keyboard;
                s_capture = false;
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controller", nullptr, pad_flags)) {
            if (!force_select && s_menu_page != MenuPage::Controller) {
                s_menu_page = MenuPage::Controller;
                s_capture = false;
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Spacing();

    const bool keyboard = s_menu_page == MenuPage::Keyboard;
    ImGui::SeparatorText(keyboard ? "Mouse" : "Gyro");
    if (keyboard)
        gx2_graphics_menu::build_mouse_rows();
    else
        gx2_graphics_menu::build_gyro_rows();
    ImGui::Spacing();
    ImGui::SeparatorText(keyboard ? "Key bindings" : "Button bindings");
    const auto v = rs::get();
    const int count = keyboard ? rs::kActionCount : rs::kFirstMoveAction;
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float spacing_x = ImGui::GetStyle().ItemSpacing.x;
    const float clear_w = ImGui::CalcTextSize("Clear").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    float max_label_w = 0.0f;
    for (int action = 0; action < count; ++action) {
        max_label_w = std::max(max_label_w, ImGui::CalcTextSize(kActionLabels[action]).x);
    }
    const float btn_w =
        std::clamp(avail_w - max_label_w - clear_w - spacing_x * 3.0f, 110.0f, std::max(120.0f, avail_w * 0.36f));
    const float btn_col_x = std::max(max_label_w + spacing_x * 2.0f, avail_w - btn_w - clear_w - spacing_x);

    for (int action = 0; action < count; ++action) {
        ImGui::PushID(action);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(kActionLabels[action]);
        ImGui::SameLine(btn_col_x);
        const int binding = keyboard ? v.keys[action] : v.pad[action];
        const bool capturing = s_capture && s_capture_page == s_menu_page && s_capture_action == action;
        const std::string label = capturing ? "Press key..." : keyboard ? key_name(binding) : pad_name(binding);
        if (capturing) {
            const ImVec4 active_col = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
            const ImVec4 hover_col = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
            ImGui::PushStyleColor(ImGuiCol_Button, active_col);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover_col);
        }
        if (ImGui::Button(label.c_str(), ImVec2(btn_w, 0))) {
            s_capture = true;
            s_capture_page = s_menu_page;
            s_capture_action = action;
        }
        if (capturing) ImGui::PopStyleColor(2);
        if (ImGui::IsItemFocused()) {
            s_focused_page = s_menu_page;
            s_focused_action = action;
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear", ImVec2(clear_w, 0))) bind_action(s_menu_page, action, rs::kUnbound);
        ImGui::PopID();
    }
    ImGui::Spacing();
    if (ImGui::Button("Reset to defaults")) {
        auto defaults = rs::get();
        rs::default_bindings(defaults.keys, defaults.pad);
        rs::set(defaults);
    }
    ImGui::TextDisabled("%s", keyboard ? "Enter: change   Delete: clear   Esc: resume"
                                       : "Cross: change   Square: clear   Circle: back");
}

// ---- Friends page (F4) ----
// The game's friend list is read from RBREW_FRIENDS_DIR by the fp stubs and filled by online/friends_relay.cpp from
// the server. This page only talks to that relay through small files in the same folder: add.cmd / remove.cmd (a pseudo),
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
    return rbrew_online::friends_dir();
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
    const auto online_cfg = rbrew_online::config();
    if (!online_cfg.online || dir().empty()) {
        ImGui::TextWrapped("Online mode is off. Log in to an online account or enable Online in SplatPC Builder.");
        if (ImGui::Button("Online Login / Account")) s_menu_page = MenuPage::OnlineLogin;
        return;
    }
    refresh();
    ImGui::TextDisabled("Pseudo: %s   |   PID: %08x   |   Server: %s:%u", rbrew_online::pseudo().c_str(),
                        rbrew_online::identity().pid, online_cfg.host.c_str(), unsigned(online_cfg.port));
    ImGui::TextWrapped(
        "Type a friend's pseudo and press Enter: they get a request and must accept it. You then see each other "
        "in the game's friend list and can join each other's rooms.");
    ImGui::Separator();
    if (s_focus_input) {
        ImGui::SetKeyboardFocusHere();
        s_focus_input = false;
    }
    const float add_w = ImGui::CalcTextSize("Add").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(
        std::max(120.0f, ImGui::GetContentRegionAvail().x - add_w - ImGui::GetStyle().ItemSpacing.x));
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
        if (ImGui::BeginTable("##requests_tbl", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed);
            for (size_t i = 0; i < s_requests.size(); ++i) {
                const Request request = s_requests[i];
                ImGui::TableNextRow();
                ImGui::PushID(("request_" + request.pid).c_str());
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(request.name.c_str());
                ImGui::TableSetColumnIndex(1);
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
            ImGui::EndTable();
        }
    }
    ImGui::Separator();
    if (s_rows.empty()) {
        ImGui::TextDisabled("No friends yet.");
    } else if (ImGui::BeginTable("##friends_tbl", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("friend", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthFixed);
        for (size_t i = 0; i < s_rows.size(); ++i) {
            ImGui::TableNextRow();
            ImGui::PushID(int(i));
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(s_rows[i].online ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                               "%s %s", s_rows[i].online ? "(online)" : "(offline or has not added you)",
                               s_rows[i].name.c_str());
            ImGui::TableSetColumnIndex(1);
            if (ImGui::SmallButton("Remove")) send("remove.cmd", s_rows[i].name);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    if (ImGui::Button("Account / Tokens")) s_menu_page = MenuPage::OnlineLogin;
    ImGui::SameLine();
    ImGui::TextDisabled("F4 / Esc: close");
}
} // namespace friends_ui

namespace online_login_ui {
bool s_initialized = false;
bool s_login_submitted = false;
char s_pseudo[16] = "";
char s_server[64] = "";
char s_register_url[256] = "";
char s_nncs1[32] = "";
char s_nncs2[32] = "";

void copy_field(char* dst, size_t cap, const std::string& src) {
    if (!cap) return;
    const size_t n = std::min(src.size(), cap - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

void ensure_loaded() {
    if (s_initialized) return;
    const auto f = rbrew_native::current_login_form();
    copy_field(s_pseudo, sizeof s_pseudo, f.pseudo);
    copy_field(s_server, sizeof s_server, f.server);
    copy_field(s_register_url, sizeof s_register_url, f.register_url);
    copy_field(s_nncs1, sizeof s_nncs1, f.nncs1);
    copy_field(s_nncs2, sizeof s_nncs2, f.nncs2);
    s_initialized = true;
}

void draw_form_body(bool startup_mode) {
    ensure_loaded();
    const auto st = rbrew_native::login_status();

    ImGui::TextWrapped(
        "Sign in to the SplatPC community server. Your identity and session token are saved next to the game.");
    ImGui::Separator();

    ImGui::BeginDisabled(st.connecting);
    ImGui::InputText("Pseudo", s_pseudo, sizeof s_pseudo);
    ImGui::TextDisabled("2 to 10 characters: letters, digits, space, _ - .");
    ImGui::InputText("Server (IP:Port)", s_server, sizeof s_server);
    ImGui::InputText("Register URL", s_register_url, sizeof s_register_url);
    if (ImGui::CollapsingHeader("Advanced (NNCS1 / NNCS2)")) {
        ImGui::InputText("NNCS1 IP", s_nncs1, sizeof s_nncs1);
        ImGui::InputText("NNCS2 IP", s_nncs2, sizeof s_nncs2);
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    if (st.has_identity)
        ImGui::Text("Identity (online_identity.ini): PID %08x", st.pid);
    else
        ImGui::TextDisabled("Identity (online_identity.ini): auto-created on connect");
    ImGui::Text("Registration (online_registered.txt): %s", st.has_registered ? "Saved" : "Not registered yet");
    ImGui::Text("Session Token (online_session.ini): %s", st.has_saved_token ? "Saved" : "None");

    if (!st.message.empty()) {
        ImVec4 col = ImVec4(0.4f, 1.0f, 0.5f, 1.0f);
        if (st.connecting)
            col = ImVec4(1.0f, 0.85f, 0.35f, 1.0f);
        else if (st.has_error)
            col = ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
        ImGui::Separator();
        ImGui::TextColored(col, "%s", st.message.c_str());
        if (!st.detail.empty()) ImGui::TextWrapped("%s", st.detail.c_str());
    }

    ImGui::Separator();
    ImGui::BeginDisabled(st.connecting);
    if (ImGui::Button(st.connecting ? "Connecting..." : "Connect & Save Tokens")) {
        s_login_submitted = true;
        rbrew_native::start_interactive_login({s_pseudo, s_server, s_register_url, s_nncs1, s_nncs2});
    }
    if (startup_mode) {
        ImGui::SameLine();
        if (ImGui::Button("Play Offline")) {
            rbrew_online::set_session_offline(true);
            s_startup_login_open.store(false, std::memory_order_relaxed);
        }
    }
    if (st.has_saved_token || st.has_registered) {
        const float clear_w = ImGui::CalcTextSize("Clear Saved Tokens").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float right_edge = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        if ((ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + clear_w) <= right_edge) {
            ImGui::SameLine();
        }
        if (ImGui::Button("Clear Saved Tokens")) rbrew_native::clear_saved_tokens();
    }
    ImGui::EndDisabled();

    if (startup_mode && s_login_submitted && !st.connecting && st.logged_in && !st.has_error) {
        s_login_submitted = false;
        s_startup_login_open.store(false, std::memory_order_relaxed);
    }
}

void build_startup_window() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500.0f, 0.0f), ImGuiCond_Always);
    if (ImGui::Begin("Splatoon Online - Account Login", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
        draw_form_body(true);
    }
    ImGui::End();
}
} // namespace online_login_ui

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

namespace {

ImGuiCol map_mod_col(int col_id) {
    switch (col_id) {
    case SPLATMOD_COL_TEXT: return ImGuiCol_Text;
    case SPLATMOD_COL_TEXT_DISABLED: return ImGuiCol_TextDisabled;
    case SPLATMOD_COL_WINDOW_BG: return ImGuiCol_WindowBg;
    case SPLATMOD_COL_CHILD_BG: return ImGuiCol_ChildBg;
    case SPLATMOD_COL_BORDER: return ImGuiCol_Border;
    case SPLATMOD_COL_FRAME_BG: return ImGuiCol_FrameBg;
    case SPLATMOD_COL_BUTTON: return ImGuiCol_Button;
    case SPLATMOD_COL_BUTTON_HOVERED: return ImGuiCol_ButtonHovered;
    case SPLATMOD_COL_BUTTON_ACTIVE: return ImGuiCol_ButtonActive;
    case SPLATMOD_COL_SCROLLBAR_BG: return ImGuiCol_ScrollbarBg;
    case SPLATMOD_COL_SCROLLBAR_GRAB: return ImGuiCol_ScrollbarGrab;
    case SPLATMOD_COL_SCROLLBAR_GRAB_HOVERED: return ImGuiCol_ScrollbarGrabHovered;
    default: return ImGuiCol_Text;
    }
}

ImGuiStyleVar map_mod_var(int var_id) {
    switch (var_id) {
    case SPLATMOD_VAR_WINDOW_PADDING: return ImGuiStyleVar_WindowPadding;
    case SPLATMOD_VAR_WINDOW_ROUNDING: return ImGuiStyleVar_WindowRounding;
    case SPLATMOD_VAR_WINDOW_BORDER_SIZE: return ImGuiStyleVar_WindowBorderSize;
    case SPLATMOD_VAR_CHILD_ROUNDING: return ImGuiStyleVar_ChildRounding;
    case SPLATMOD_VAR_FRAME_PADDING: return ImGuiStyleVar_FramePadding;
    case SPLATMOD_VAR_FRAME_ROUNDING: return ImGuiStyleVar_FrameRounding;
    case SPLATMOD_VAR_FRAME_BORDER_SIZE: return ImGuiStyleVar_FrameBorderSize;
    case SPLATMOD_VAR_ITEM_SPACING: return ImGuiStyleVar_ItemSpacing;
    case SPLATMOD_VAR_SCROLLBAR_ROUNDING: return ImGuiStyleVar_ScrollbarRounding;
    default: return ImGuiStyleVar_FrameRounding;
    }
}

ImU32 rgba_u32(float r, float g, float b, float a) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a));
}

uint64_t mod_create_rgba_texture(void*, const uint8_t* rgba, uint32_t w, uint32_t h) {
    if (!s_ready || !rgba || w == 0 || h == 0 || !gx2vk::ready()) return 0;
    if (s_mod_textures.size() >= 64) return 0;
    const VkDeviceSize bytes = VkDeviceSize(w) * VkDeviceSize(h) * 4u;

    gx2vk::ImageDesc desc{};
    desc.type = VK_IMAGE_TYPE_2D;
    desc.format = VK_FORMAT_R8G8B8A8_UNORM;
    desc.width = w;
    desc.height = h;
    desc.depth = 1;
    desc.levels = 1;
    desc.layers = 1;
    desc.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    ModTextureEntry entry{};
    if (!gx2vk::create_image(entry.image, desc)) return 0;

    const gx2vk::Span span = gx2vk::upload(bytes, 16);
    if (!span.buffer || !span.data) {
        gx2vk::destroy_image(entry.image);
        return 0;
    }
    std::memcpy(span.data, rgba, size_t(bytes));

    VkCommandBuffer cb = gx2vk::cmd();
    VkBufferImageCopy region{};
    region.bufferOffset = span.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cb, span.buffer, entry.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    gx2vk::note_gpu_write(entry.image);
    gx2vk::barrier_if_dirty();

    gx2vk::ViewDesc vdesc{};
    vdesc.type = VK_IMAGE_VIEW_TYPE_2D;
    vdesc.format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageView view = gx2vk::image_view(entry.image, vdesc);
    if (!view) {
        gx2vk::destroy_image(entry.image);
        return 0;
    }

    VkSamplerCreateInfo sinfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sinfo.magFilter = VK_FILTER_LINEAR;
    sinfo.minFilter = VK_FILTER_LINEAR;
    sinfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sinfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sinfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sinfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sinfo.maxLod = 1.0f;
    (void)gx2vk::sampler(sinfo);

    entry.descriptor = ImGui_ImplVulkan_AddTexture(view, VK_IMAGE_LAYOUT_GENERAL);
    if (!entry.descriptor) {
        gx2vk::destroy_image(entry.image);
        return 0;
    }

    const uint64_t handle = reinterpret_cast<uint64_t>(entry.descriptor);
    s_mod_textures.emplace(handle, std::move(entry));
    return handle;
}

void mod_destroy_texture(void*, uint64_t tex_id) {
    if (!tex_id) return;
    auto it = s_mod_textures.find(tex_id);
    if (it == s_mod_textures.end()) return;
    VkDescriptorSet ds = it->second.descriptor;
    gx2vk::Image img = std::move(it->second.image);
    s_mod_textures.erase(it);
    gx2vk::defer([ds, img = std::move(img)]() mutable {
        if (s_ready && ds) ImGui_ImplVulkan_RemoveTexture(ds);
        if (img) gx2vk::destroy_image(img);
    });
}

bool mod_detile_gx2_mip0(void*, uint32_t w_elem, uint32_t h_elem, uint32_t pitch_elem, uint32_t bpe, uint32_t tile_mode,
                         uint32_t swizzle, const uint8_t* tiled, size_t tiled_size, uint8_t* out) {
    Gx2DetileDesc d{};
    d.width = w_elem;
    d.height = h_elem;
    d.pitch = pitch_elem;
    d.bits_per_element = bpe;
    d.tile_mode = tile_mode;
    d.swizzle = swizzle;
    return gx2_detile_mip0(d, tiled, tiled_size, out);
}

void mod_image_rounded(void*, uint64_t tex_id, float w, float h, float rounding, float border_r, float border_g,
                       float border_b, float border_a) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + h);
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (tex_id != 0) {
        dl->AddImageRounded(static_cast<ImTextureID>(tex_id), p0, p1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE,
                            rounding);
    } else {
        dl->AddRectFilled(p0, p1, IM_COL32(34, 38, 56, 255), rounding);
    }
    if (border_a > 0.01f) {
        dl->AddRect(p0, p1, rgba_u32(border_r, border_g, border_b, border_a), rounding, 0, 2.0f);
    }
}

bool mod_stage_card(void*, const char* id, uint64_t tex_id, const char* title, const char* badge, bool selected,
                    float w, float h, float accent_r, float accent_g, float accent_b) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + h);
    const bool clicked = ImGui::InvisibleButton(id ? id : "##card", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = 10.0f;

    // Background plate
    dl->AddRectFilled(p0, p1, IM_COL32(22, 26, 40, 255), rounding);

    // Stage preview art inside upper portion / full bleed with bottom caption scrim
    if (tex_id != 0) {
        dl->AddImageRounded(static_cast<ImTextureID>(tex_id), p0, p1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE,
                            rounding);
    } else {
        dl->AddRectFilledMultiColor(p0, p1, IM_COL32(36, 44, 68, 255), IM_COL32(48, 32, 68, 255),
                                    IM_COL32(24, 28, 44, 255), IM_COL32(20, 24, 38, 255));
    }

    // Bottom dark gradient scrim for crisp stage name readability
    const float scrim_top = p0.y + h * 0.56f;
    dl->AddRectFilledMultiColor(ImVec2(p0.x, scrim_top), p1, IM_COL32(10, 12, 20, 20), IM_COL32(10, 12, 20, 20),
                                IM_COL32(8, 10, 16, 238), IM_COL32(8, 10, 16, 238));
    // Re-mask bottom corners cleanly
    dl->AddRectFilled(ImVec2(p0.x, p1.y - 30.0f), p1, IM_COL32(10, 12, 18, 225), rounding,
                      ImDrawFlags_RoundCornersBottom);

    if (hovered || focused) {
        dl->AddRectFilled(p0, p1, rgba_u32(accent_r, accent_g, accent_b, 0.14f), rounding);
    }

    // Crisp stage title centered in bottom strip
    if (title && *title) {
        ImFont* tfont = s_display_font ? s_display_font : ImGui::GetFont();
        const float tsize = s_display_font ? 20.0f : ImGui::GetFontSize();
        const ImVec2 tsz = tfont->CalcTextSizeA(tsize, FLT_MAX, w - 14.0f, title);
        const float tx = p0.x + std::max(7.0f, (w - tsz.x) * 0.5f);
        const float ty = p1.y - std::max(26.0f, tsz.y + 6.0f);
        dl->AddText(tfont, tsize, ImVec2(tx + 1.5f, ty + 1.5f), IM_COL32(0, 0, 0, 230), title, nullptr, w - 14.0f);
        dl->AddText(tfont, tsize, ImVec2(tx, ty), IM_COL32(255, 255, 255, 255), title, nullptr, w - 14.0f);
    }

    // Badge pill (top-left) if provided
    if (badge && *badge) {
        const ImVec2 bsz = ImGui::CalcTextSize(badge);
        const ImVec2 bp0(p0.x + 8.0f, p0.y + 8.0f);
        const ImVec2 bp1(bp0.x + bsz.x + 14.0f, bp0.y + bsz.y + 6.0f);
        dl->AddRectFilled(bp0, bp1, rgba_u32(accent_r, accent_g, accent_b, 0.95f), 6.0f);
        dl->AddText(ImVec2(bp0.x + 7.0f, bp0.y + 3.0f), IM_COL32(12, 16, 24, 255), badge);
    }

    // Border highlight (selected accent + distinct crisp white focus/hover halo)
    if (selected) {
        dl->AddRect(p0, p1, rgba_u32(accent_r, accent_g, accent_b, 1.0f), rounding, 0, 3.5f);
    } else {
        dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 42), rounding, 0, 1.2f);
    }
    if (focused || hovered) {
        dl->AddRect(ImVec2(p0.x - 2.0f, p0.y - 2.0f), ImVec2(p1.x + 2.0f, p1.y + 2.0f), IM_COL32(255, 255, 255, 245),
                    rounding + 2.0f, 0, 2.8f);
    }
    return clicked;
}

bool mod_option_card(void*, const char* id, const char* title, const char* subtitle, const char* badge, bool selected,
                     float w, float h, float accent_r, float accent_g, float accent_b) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + h);
    const bool clicked = ImGui::InvisibleButton(id ? id : "##opt", ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = 11.0f;

    ImU32 bg = IM_COL32(28, 32, 48, 240);
    if (selected) {
        bg = IM_COL32(int(24 + accent_r * 34), int(28 + accent_g * 34), int(42 + accent_b * 34), 245);
    } else if (hovered || focused) {
        bg = IM_COL32(40, 46, 68, 250);
    }
    dl->AddRectFilled(p0, p1, bg, rounding);

    // Left ink accent stripe
    dl->AddRectFilled(p0, ImVec2(p0.x + 8.0f, p1.y), rgba_u32(accent_r, accent_g, accent_b, 0.95f), rounding,
                      ImDrawFlags_RoundCornersLeft);

    const float text_left = p0.x + 20.0f;
    float wrap_w = w - 34.0f;
    if (badge && *badge) {
        const ImVec2 bsz = ImGui::CalcTextSize(badge);
        const float bw = bsz.x + 18.0f;
        const ImVec2 bp1(p1.x - 14.0f, p0.y + 14.0f + bsz.y + 8.0f);
        const ImVec2 bp0(bp1.x - bw, p0.y + 14.0f);
        dl->AddRectFilled(bp0, bp1, rgba_u32(accent_r, accent_g, accent_b, 0.95f), 7.0f);
        dl->AddText(ImVec2(bp0.x + 9.0f, bp0.y + 4.0f), IM_COL32(12, 16, 24, 255), badge);
        wrap_w = std::max(120.0f, bp0.x - text_left - 10.0f);
    }

    if (title && *title) {
        ImFont* tfont = s_display_font ? s_display_font : ImGui::GetFont();
        const float tsize = s_display_font ? 23.0f : (ImGui::GetFontSize() * 1.12f);
        dl->AddText(tfont, tsize, ImVec2(text_left + 1.0f, p0.y + 11.0f), IM_COL32(0, 0, 0, 180), title);
        dl->AddText(tfont, tsize, ImVec2(text_left, p0.y + 10.0f), IM_COL32(255, 255, 255, 255), title);
    }
    if (subtitle && *subtitle) {
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.88f, ImVec2(text_left, p0.y + 38.0f),
                    IM_COL32(192, 204, 226, 245), subtitle, nullptr, wrap_w);
    }

    if (selected) {
        dl->AddRect(p0, p1, rgba_u32(accent_r, accent_g, accent_b, 1.0f), rounding, 0, 3.0f);
    } else {
        dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 45), rounding, 0, 1.2f);
    }
    if (focused || hovered) {
        dl->AddRect(ImVec2(p0.x - 2.0f, p0.y - 2.0f), ImVec2(p1.x + 2.0f, p1.y + 2.0f), IM_COL32(255, 255, 255, 245),
                    rounding + 2.0f, 0, 2.8f);
    }
    return clicked;
}

const SplatModUiEx kModUiEx = {
    sizeof(SplatModUiEx),
    [](void*) { return s_texture_epoch; },
    &mod_create_rgba_texture,
    &mod_destroy_texture,
    &mod_detile_gx2_mip0,
    [](void*, float* out_w, float* out_h) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        if (out_w) *out_w = vp ? vp->WorkSize.x : 1280.0f;
        if (out_h) *out_h = vp ? vp->WorkSize.y : 720.0f;
    },
    [](void*, float* out_w, float* out_h) {
        const ImVec2 av = ImGui::GetContentRegionAvail();
        if (out_w) *out_w = av.x;
        if (out_h) *out_h = av.y;
    },
    [](void*) { return ImGui::GetTime(); },
    [](void*) {
        const bool p = s_mod_back_pressed;
        s_mod_back_pressed = false;
        return p;
    },
    [](void*, float scale) { ImGui::SetWindowFontScale(scale); },
    [](void*, int col_id, float r, float g, float b, float a) {
        ImGui::PushStyleColor(map_mod_col(col_id), ImVec4(r, g, b, a));
    },
    [](void*, int count) { ImGui::PopStyleColor(count); },
    [](void*, int var_id, float val) { ImGui::PushStyleVar(map_mod_var(var_id), val); },
    [](void*, int var_id, float x, float y) { ImGui::PushStyleVar(map_mod_var(var_id), ImVec2(x, y)); },
    [](void*, int count) { ImGui::PopStyleVar(count); },
    [](void*, const char* id, float w, float h, bool border) {
        return ImGui::BeginChild(id ? id : "##child", ImVec2(w, h), border ? ImGuiChildFlags_Borders : 0);
    },
    [](void*) { ImGui::EndChild(); },
    [](void*, float offset_x, float spacing) { ImGui::SameLine(offset_x, spacing); },
    [](void*, float w, float h) { ImGui::Dummy(ImVec2(w, h)); },
    [](void*, float x) { ImGui::SetCursorPosX(x); },
    [](void*, float y) { ImGui::SetCursorPosY(y); },
    [](void*) { return ImGui::GetCursorPosX(); },
    [](void*) { return ImGui::GetCursorPosY(); },
    [](void*) {
        ImGui::SetWindowFocus();
        ImGui::SetKeyboardFocusHere();
    },
    [](void*, const char* label, float w, float h) { return ImGui::Button(label ? label : "", ImVec2(w, h)); },
    &mod_image_rounded,
    [](void*, float x, float y, float w, float h, float r, float g, float b, float a, float rounding) {
        const ImVec2 wp = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(wp.x + x, wp.y + y), ImVec2(wp.x + x + w, wp.y + y + h),
                                                  rgba_u32(r, g, b, a), rounding);
    },
    &mod_stage_card,
    &mod_option_card,
    [](void*, float width, float r, float g, float b, float a, const char* text) {
        if (!text) return;
        const ImVec2 tsz = ImGui::CalcTextSize(text);
        const float cx = ImGui::GetCursorPosX();
        if (width > tsz.x) ImGui::SetCursorPosX(cx + (width - tsz.x) * 0.5f);
        ImGui::TextColored(ImVec4(r, g, b, a), "%s", text);
    },
};

const SplatModUi kModUi = {
    nullptr,
    [](void*, const char* text) { ImGui::TextUnformatted(text ? text : ""); },
    [](void*, float r, float g, float b, float a, const char* text) {
        ImGui::TextColored(ImVec4(r, g, b, a), "%s", text ? text : "");
    },
    [](void*, const char* text) { ImGui::TextWrapped("%s", text ? text : ""); },
    [](void*) { ImGui::Separator(); },
    [](void*, const char* label) { ImGui::SeparatorText(label ? label : ""); },
    [](void*) { ImGui::Spacing(); },
    [](void*) { ImGui::SameLine(); },
    [](void*, const char* label, float width) { return ImGui::Button(label ? label : "", ImVec2(width, 0.0f)); },
    [](void*, const char* label, bool* value) { return value ? ImGui::Checkbox(label ? label : "", value) : false; },
    [](void*, const char* label, int* value, int min_val, int max_val, const char* format) {
        if (!value) return false;
        return ImGui::SliderInt(label ? label : "", value, min_val, max_val, (format && *format) ? format : "%d");
    },
    [](void*, const char* label, const char* hint, char* buf, size_t buf_size) {
        if (!buf || buf_size == 0) return false;
        return ImGui::InputTextWithHint(label ? label : "", hint ? hint : "", buf, buf_size);
    },
    [](void*, bool disabled) { ImGui::BeginDisabled(disabled); },
    [](void*) { ImGui::EndDisabled(); },
    [](void*, const char* text) { ImGui::SetClipboardText(text ? text : ""); },
    &kModUiEx,
};

} // namespace

void build_mods_page() {
    const auto mods = rbrew_mod::list_mods();
    size_t enabled_count = 0;
    for (const auto& m : mods) {
        if (m.enabled) ++enabled_count;
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Mods (%zu/%zu enabled)", enabled_count, mods.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Rescan")) {
        s_mod_toggle_error.clear();
        s_confirm_delete_mod_id.clear();
        rbrew_mod::rescan_mods();
    }
    ImGui::Separator();

    if (mods.empty()) {
        ImGui::TextDisabled("No mods installed.");
    } else if (ImGui::BeginTable("##mods_table", 2,
                                 ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("mod", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed);

        for (const auto& mod : mods) {
            ImGui::TableNextRow();
            ImGui::PushID(mod.id.c_str());

            ImGui::TableSetColumnIndex(0);
            bool enabled = mod.enabled;
            if (ImGui::Checkbox("##enabled", &enabled)) {
                s_mod_toggle_error.clear();
                s_confirm_delete_mod_id.clear();
                std::string err;
                if (!rbrew_mod::set_mod_enabled(mod.id, enabled, &err)) {
                    s_mod_toggle_error = err.empty() ? (mod.name + ": toggle failed") : (mod.name + ": " + err);
                } else if (!enabled && s_selected_mod_page != 0) {
                    bool still_present = false;
                    for (const auto& p : rbrew_mod::list_menu_pages()) {
                        if (p.handle == s_selected_mod_page) {
                            still_present = true;
                            break;
                        }
                    }
                    if (!still_present) {
                        s_selected_mod_page = 0;
                        s_mod_page_direct = false;
                    }
                }
            }
            ImGui::SameLine();
            ImGui::BeginGroup();
            if (mod.enabled) {
                ImGui::TextWrapped("%s %s", mod.name.c_str(), mod.version.c_str());
            } else {
                ImGui::TextDisabled("%s %s (Disabled)", mod.name.c_str(), mod.version.c_str());
            }
            if (!mod.author.empty()) {
                ImGui::TextDisabled("by %s", mod.author.c_str());
            }
            ImGui::EndGroup();

            ImGui::TableSetColumnIndex(1);
            if (s_confirm_delete_mod_id == mod.id) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.16f, 0.16f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.22f, 0.22f, 1.0f));
                if (ImGui::SmallButton("Confirm Delete")) {
                    s_mod_toggle_error.clear();
                    std::string err;
                    if (!rbrew_mod::delete_mod(mod.id, &err)) {
                        s_mod_toggle_error = err.empty() ? (mod.name + ": delete failed") : (mod.name + ": " + err);
                    } else if (s_selected_mod_page != 0) {
                        bool still_present = false;
                        for (const auto& p : rbrew_mod::list_menu_pages()) {
                            if (p.handle == s_selected_mod_page) {
                                still_present = true;
                                break;
                            }
                        }
                        if (!still_present) {
                            s_selected_mod_page = 0;
                            s_mod_page_direct = false;
                        }
                    }
                    s_confirm_delete_mod_id.clear();
                }
                ImGui::PopStyleColor(2);
                ImGui::SameLine();
                if (ImGui::SmallButton("Cancel")) {
                    s_confirm_delete_mod_id.clear();
                }
            } else if (ImGui::SmallButton("Delete")) {
                s_confirm_delete_mod_id = mod.id;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (!s_mod_toggle_error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", s_mod_toggle_error.c_str());
    }

    const auto pages = rbrew_mod::list_menu_pages();
    if (!pages.empty()) {
        ImGui::Spacing();
        ImGui::SeparatorText("Mod Menus");
        for (const auto& page : pages) {
            if (ImGui::Button(page.title.c_str())) {
                s_selected_mod_page = page.handle;
                s_mod_page_direct = false;
                s_menu_page = MenuPage::ModPage;
            }
            if (page.status) {
                if (const char* st = page.status(page.user); st && *st) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", st);
                }
            }
        }
    }
}

void build_mod_custom_page() {
    const auto pages = rbrew_mod::list_menu_pages();
    const rbrew_mod::ModMenuPageView* active = nullptr;
    for (const auto& page : pages) {
        if (page.handle == s_selected_mod_page) {
            active = &page;
            break;
        }
    }
    if (!active || !active->draw) {
        s_selected_mod_page = 0;
        s_mod_page_direct = false;
        s_menu_page = MenuPage::Mods;
        return;
    }

    const int result = active->draw(&kModUi, active->user);
    if (result == SPLATMOD_MENU_QUIT) {
        open_quit_confirmation();
    } else if (result == SPLATMOD_MENU_CLOSE) {
        s_selected_mod_page = 0;
        s_mod_page_direct = false;
        s_mod_back_pressed = false;
        set_menu_open(false);
        return;
    } else if (result == SPLATMOD_MENU_BACK) {
        s_selected_mod_page = 0;
        const bool direct = s_mod_page_direct;
        s_mod_page_direct = false;
        s_mod_back_pressed = false;
        if (direct) {
            set_menu_open(false);
        } else {
            s_menu_page = MenuPage::Mods;
        }
        return;
    }

    if (!s_mod_page_direct) {
        ImGui::Separator();
        if (ImGui::Button("Back", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            s_selected_mod_page = 0;
            s_mod_page_direct = false;
            s_menu_page = MenuPage::Mods;
        }
    }
}
const char* menu_title(MenuPage page) {
    switch (page) {
    case MenuPage::Display: return "Display";
    case MenuPage::Keyboard:
    case MenuPage::Controller: return "Controls";
    case MenuPage::Friends: return "Friends";
    case MenuPage::OnlineLogin: return "Account";
    case MenuPage::Amiibo: return "amiibo";
    case MenuPage::Mods: return "Mods";
    default: return "General";
    }
}

struct ScopedPauseStyle {
    ScopedPauseStyle() {
        const float s = std::max(1.0f, ImGui::GetIO().FontGlobalScale);
        // Keep the built-in StyleColorsDark() palette and apply cleaner spacing/rounding.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * s, 12.0f * s));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * s, 5.0f * s));
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f * s, 4.0f * s));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * s, 6.0f * s));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(6.0f * s, 4.0f * s));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 12.0f * s);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f * s);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f * s);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f * s);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 4.0f * s);
        ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 2.0f * s);
        ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 3.0f * s);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 12.0f * s);
    }
    ~ScopedPauseStyle() { ImGui::PopStyleVar(16); }
};

bool nav_item(const char* id, const char* label, bool active, float width) {
    const float h = ImGui::GetFrameHeight() + 4.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + width, p0.y + h);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(width, h), ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsItemFocused();

    const ImGuiID key = ImGui::GetID(id);
    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.001f, 0.05f);
    float& ht = s_nav_hover[key];
    const float goal = (active || hovered || focused) ? 1.0f : 0.0f;
    ht += (goal - ht) * std::min(1.0f, dt * 14.0f);

    const ImVec4 btn_col = ImGui::GetStyleColorVec4(ImGuiCol_Button);
    const ImVec4 accent_col = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
    const ImVec4 text_col = ImGui::GetStyleColorVec4(ImGuiCol_Text);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active || ht > 0.02f) {
        const float bg_alpha = active ? 0.55f : 0.35f * ht;
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImVec4(btn_col.x, btn_col.y, btn_col.z, bg_alpha)), 3.0f);
    }

    if (active || ht > 0.05f) {
        const float bar_h = h * (active ? 0.62f : 0.42f * ht);
        const float by0 = p0.y + (h - bar_h) * 0.5f;
        const float bar_alpha = active ? 1.0f : 0.78f * ht;
        dl->AddRectFilled(ImVec2(p0.x + 2.0f, by0), ImVec2(p0.x + 5.0f, by0 + bar_h),
                          ImGui::GetColorU32(ImVec4(accent_col.x, accent_col.y, accent_col.z, bar_alpha)), 1.5f);
    }
    if (focused) {
        dl->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(accent_col.x, accent_col.y, accent_col.z, 0.90f)), 3.0f, 0, 1.5f);
    }

    const ImVec2 tsz = ImGui::CalcTextSize(label);
    const float ty = p0.y + (h - tsz.y) * 0.5f;
    const float text_alpha = (active || hovered) ? 1.0f : 0.80f;
    dl->AddText(ImVec2(p0.x + 12.0f, ty), ImGui::GetColorU32(ImVec4(text_col.x, text_col.y, text_col.z, text_alpha)),
                label);
    return clicked;
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
        ImGui::TextDisabled("Render resolution applies after restart.");
    }
    ImGui::Spacing();
    ImGui::SeparatorText("Frame rate");
    dirty |= gx2_graphics_menu::build_frame_rows(v);
    dirty |= ImGui::Checkbox("Plaza 60 FPS", &v.plaza_60fps);
    ImGui::Spacing();
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
}

void build_general_page() {
    auto v = rs::get();
    bool dirty = false;
    ImGui::SeparatorText("Audio");
    dirty |= ImGui::SliderInt("Volume", &v.volume, 0, 100, "%d %%");
    ImGui::Spacing();
    ImGui::SeparatorText("Session");
    dirty |= ImGui::Checkbox("Confirm before quitting", &v.confirm_quit);
    if (dirty) rs::set(v);

    ImGui::Spacing();
    ImGui::SeparatorText("Quick access");
    const float pad_x = ImGui::GetStyle().FramePadding.x * 2.0f;
    const float sp_x = ImGui::GetStyle().ItemSpacing.x;
    const float right_edge = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    bool same_row = false;
    const auto place_quick = [&](const char* label) -> bool {
        const float w = ImGui::CalcTextSize(label).x + pad_x;
        if (same_row && (ImGui::GetItemRectMax().x + sp_x + w) <= right_edge) {
            ImGui::SameLine();
        }
        same_row = true;
        return ImGui::Button(label);
    };
    if (rbrew_online::config().online) {
        if (place_quick("Friends (F4)")) {
            s_menu_page = MenuPage::Friends;
            friends_ui::s_focus_input = true;
        }
    }
    if (place_quick("Online Account")) s_menu_page = MenuPage::OnlineLogin;
    if (rbrew_nfp::searching()) {
        if (place_quick("Scan amiibo")) s_menu_page = MenuPage::Amiibo;
    }
}

void build_settings_window() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 center(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                        viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);

    if (s_graphics_page == gx2_graphics_menu::Page::None && s_menu_page == MenuPage::ModPage && s_mod_page_direct) {
        ImGui::GetBackgroundDrawList()->AddRectFilled(
            viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
            IM_COL32(6, 8, 16, 185));
        const ImVec2 win_sz(std::min(viewport->WorkSize.x * 0.90f, 1120.0f),
                            std::min(viewport->WorkSize.y * 0.88f, 700.0f));
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(win_sz, ImGuiCond_Always);
        if (s_focus_direct_canvas) {
            ImGui::SetNextWindowFocus();
            s_focus_direct_canvas = false;
        }
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.08f, 0.13f, 0.96f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.66f, 0.94f, 0.12f, 0.90f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 20.0f));
        if (ImGui::Begin("##mod_direct_canvas", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
            build_mod_custom_page();
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        return;
    }

    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.001f, 0.05f);
    s_menu_anim = std::min(1.0f, s_menu_anim + dt * 12.0f);
    const float ease = 1.0f - (1.0f - s_menu_anim) * (1.0f - s_menu_anim);

    ImGui::GetBackgroundDrawList()->AddRectFilled(
        viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
        IM_COL32(0, 0, 0, int(135.0f * ease)));

    ScopedPauseStyle pause_style;

    const float ui_scale = std::max(1.0f, ImGui::GetIO().FontGlobalScale);
    const float win_w =
        std::clamp(viewport->WorkSize.x * 0.74f, std::min(680.0f * ui_scale, viewport->WorkSize.x - 20.0f),
                   std::min(1040.0f * ui_scale, viewport->WorkSize.x - 20.0f));
    const float win_h =
        std::clamp(viewport->WorkSize.y * 0.78f, std::min(460.0f * ui_scale, viewport->WorkSize.y - 20.0f),
                   std::min(680.0f * ui_scale, viewport->WorkSize.y - 20.0f));
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(win_w, win_h), ImGuiCond_Always);

    bool open = true;
    constexpr ImGuiWindowFlags win_flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                                           ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("###settings", &open, win_flags)) {
        ImGui::End();
        return;
    }

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float rail_w = std::clamp(avail_w * 0.22f, 130.0f * ui_scale, 185.0f * ui_scale);

    if (ImGui::BeginChild("##pause_nav", ImVec2(rail_w, 0), ImGuiChildFlags_Borders)) {
        const float nav_avail = ImGui::GetContentRegionAvail().x;
        ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark), "Pause");
        ImGui::Separator();
        ImGui::Spacing();

        const float bottom_h = (ImGui::GetFrameHeight() + 4.0f) * 2.0f + ImGui::GetStyle().ItemSpacing.y * 3.0f;
        if (ImGui::BeginChild("##nav_scroll", ImVec2(0, -bottom_h), ImGuiChildFlags_None)) {
            const float btn_w = ImGui::GetContentRegionAvail().x;
            const auto select_menu = [&](const char* id, const char* label, MenuPage target, bool is_active) {
                if (nav_item(id, label, is_active, btn_w)) {
                    s_graphics_page = gx2_graphics_menu::Page::None;
                    s_menu_page = target;
                    s_capture = false;
                    s_mod_toggle_error.clear();
                    s_confirm_delete_mod_id.clear();
                    if (target == MenuPage::Friends) friends_ui::s_focus_input = true;
                }
            };
            const auto select_gfx = [&](const char* id, const char* label, gx2_graphics_menu::Page target) {
                if (nav_item(id, label, s_graphics_page == target, btn_w)) {
                    s_graphics_page = target;
                    s_capture = false;
                }
            };

            const bool no_gfx = s_graphics_page == gx2_graphics_menu::Page::None;
            select_menu("##nav_general", "General", MenuPage::Main, no_gfx && s_menu_page == MenuPage::Main);
            select_menu("##nav_display", "Display", MenuPage::Display, no_gfx && s_menu_page == MenuPage::Display);
            select_gfx("##nav_graphics", "Graphics", gx2_graphics_menu::Page::Graphics);
            select_menu("##nav_controls", "Controls",
                        s_menu_page == MenuPage::Controller ? MenuPage::Controller : MenuPage::Keyboard,
                        no_gfx && (s_menu_page == MenuPage::Keyboard || s_menu_page == MenuPage::Controller));
            if (!gx2env::online_mode()) select_gfx("##nav_camera", "Camera", gx2_graphics_menu::Page::Camera);
            select_gfx("##nav_weather", "Weather", gx2_graphics_menu::Page::Weather);
            if (!gx2env::online_mode()) select_gfx("##nav_cinema", "Cinema", gx2_graphics_menu::Page::Cinema);
            if (rbrew_mod::has_mods())
                select_menu("##nav_mods", "Mods", MenuPage::Mods,
                            no_gfx && (s_menu_page == MenuPage::Mods || s_menu_page == MenuPage::ModPage));
            if (rbrew_online::config().online)
                select_menu("##nav_friends", "Friends", MenuPage::Friends, no_gfx && s_menu_page == MenuPage::Friends);
            select_menu("##nav_account", "Account", MenuPage::OnlineLogin,
                        no_gfx && s_menu_page == MenuPage::OnlineLogin);
            if (rbrew_nfp::searching())
                select_menu("##nav_amiibo", "amiibo", MenuPage::Amiibo, no_gfx && s_menu_page == MenuPage::Amiibo);
        }
        ImGui::EndChild();

        ImGui::Separator();
        ImGui::Spacing();
        const float cta_h = ImGui::GetFrameHeight() + 4.0f;
        if (ImGui::Button("Resume", ImVec2(nav_avail, cta_h))) set_menu_open(false);
        if (ImGui::Button("Quit game", ImVec2(nav_avail, cta_h))) open_quit_confirmation();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    if (ImGui::BeginChild("##pause_content", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        std::string page_title = s_graphics_page != gx2_graphics_menu::Page::None
                                     ? gx2_graphics_menu::title(s_graphics_page)
                                     : menu_title(s_menu_page);
        if (s_graphics_page == gx2_graphics_menu::Page::None && s_menu_page == MenuPage::ModPage) {
            page_title = "Mod";
            for (const auto& p : rbrew_mod::list_menu_pages()) {
                if (p.handle == s_selected_mod_page) {
                    page_title = p.title;
                    break;
                }
            }
        }

        if (s_display_font) ImGui::PushFont(s_display_font);
        ImGui::TextUnformatted(page_title.c_str());
        if (s_display_font) ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();

        const float item_w = std::clamp(ImGui::GetContentRegionAvail().x * 0.44f, 120.0f * ui_scale, 280.0f * ui_scale);
        ImGui::PushItemWidth(item_w);

        if (s_graphics_page != gx2_graphics_menu::Page::None) {
            gx2_graphics_menu::build_page_contents(s_graphics_page);
        } else {
            switch (s_menu_page) {
            case MenuPage::Main: build_general_page(); break;
            case MenuPage::Display: build_display_page(); break;
            case MenuPage::Keyboard:
            case MenuPage::Controller: build_controls_page(s_menu_page); break;
            case MenuPage::Friends: friends_ui::build_page(); break;
            case MenuPage::OnlineLogin: online_login_ui::draw_form_body(false); break;
            case MenuPage::Amiibo: build_amiibo_page(); break;
            case MenuPage::Mods: build_mods_page(); break;
            case MenuPage::ModPage: build_mod_custom_page(); break;
            }
        }

        ImGui::PopItemWidth();
    }
    ImGui::EndChild();

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

    ScopedPauseStyle pause_style;
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

void feed_replay_vpad(uint32_t buttons) {
    static uint32_t s_prev_btns = 0;
    static bool s_was_open = false;
    if (!menu_open()) {
        s_prev_btns = 0;
        s_was_open = false;
        return;
    }
    if (!s_was_open) {
        // Latch whatever buttons were already held when the menu opened so they don't re-trigger inside Screen 1.
        s_was_open = true;
        s_prev_btns = buttons;
        return;
    }
    struct BtnMap {
        uint32_t mask;
        SDL_Scancode scancode;
        SDL_Keycode sym;
    };
    static constexpr BtnMap kMap[] = {
        {0x8000u, SDL_SCANCODE_SPACE, SDLK_SPACE}, {0x4000u, SDL_SCANCODE_BACKSPACE, SDLK_BACKSPACE},
        {0x0200u, SDL_SCANCODE_UP, SDLK_UP},       {0x0100u, SDL_SCANCODE_DOWN, SDLK_DOWN},
        {0x0800u, SDL_SCANCODE_LEFT, SDLK_LEFT},   {0x0400u, SDL_SCANCODE_RIGHT, SDLK_RIGHT},
    };
    const uint32_t rising = buttons & ~s_prev_btns;
    const uint32_t falling = ~buttons & s_prev_btns;
    s_prev_btns = buttons;
    for (const auto& m : kMap) {
        if (rising & m.mask) {
            SDL_Event ev{};
            ev.type = SDL_KEYDOWN;
            ev.key.windowID = s_sdl_window_id;
            ev.key.state = SDL_PRESSED;
            ev.key.keysym.scancode = m.scancode;
            ev.key.keysym.sym = m.sym;
            process_event(ev);
        }
        if (falling & m.mask) {
            SDL_Event ev{};
            ev.type = SDL_KEYUP;
            ev.key.windowID = s_sdl_window_id;
            ev.key.state = SDL_RELEASED;
            ev.key.keysym.scancode = m.scancode;
            ev.key.keysym.sym = m.sym;
            process_event(ev);
        }
    }
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
    // F4: the friends page, online only (offline the key stays free, as on main).
    if (event.type == SDL_KEYDOWN && !event.key.repeat && event.key.keysym.scancode == SDL_SCANCODE_F4 &&
        rbrew_online::config().online) {
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
    if (!menu_open() && !startup_login_active()) return false;

    // Allow OS window lifecycle, controller hotplug, and non-capture F11 toggle through.
    switch (event.type) {
    case SDL_KEYDOWN:
        if (!s_capture_active.load(std::memory_order_relaxed) && event.key.keysym.scancode == SDL_SCANCODE_F11)
            return false;
        return true;
    case SDL_KEYUP:
    case SDL_TEXTINPUT:
    case SDL_TEXTEDITING:
    case SDL_MOUSEMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL:
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
    case SDL_CONTROLLERAXISMOTION:
    case SDL_JOYBUTTONDOWN:
    case SDL_JOYBUTTONUP:
    case SDL_JOYAXISMOTION: return true;
    default: return false;
    }
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

void open_mod_page(uint64_t page_handle) {
    if (page_handle == 0) return;
    s_mod_page_requested.store(page_handle, std::memory_order_relaxed);
    set_menu_open(true);
}

void open_startup_login() {
    s_startup_login_open.store(true, std::memory_order_relaxed);
}

bool startup_login_active() {
    return s_startup_login_open.load(std::memory_order_relaxed);
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
    if (blocked && (!any_input_held || std::chrono::steady_clock::now() - closed_at > std::chrono::milliseconds(500))) {
        s_block_until_released.store(false, std::memory_order_relaxed);
        return false;
    }
    return blocked;
}

bool consume_fullscreen_request(bool& fullscreen) {
    if (!s_fullscreen_requested.exchange(false, std::memory_order_relaxed)) return false;
    fullscreen = s_fullscreen_value.load(std::memory_order_relaxed);
    return true;
}

bool consume_borderless_request(bool& borderless) {
    if (!s_borderless_requested.exchange(false, std::memory_order_relaxed)) return false;
    borderless = s_borderless_value.load(std::memory_order_relaxed);
    return true;
}

bool consume_quit_request() {
    return s_quit_requested.exchange(false, std::memory_order_relaxed);
}

void note_fullscreen(bool fullscreen) {
    s_fullscreen_mirror.store(fullscreen, std::memory_order_relaxed);
}

void note_borderless(bool borderless) {
    s_borderless_mirror.store(borderless, std::memory_order_relaxed);
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
    const bool show_login = startup_login_active();
    const bool show_menu = !show_login && menu_open();
    const bool show_simple = simple_overlay();
    if (!visible() && !show_menu && !show_simple && !show_login) {
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

    // Keep the default bitmap font crisp (1x up to 1080p, 2x at 1440p+/4K) with no fractional distortion.
    ImGui::GetIO().FontGlobalScale = extent.height >= 1400 ? 2.0f : 1.0f;

    const bool fresh_menu = show_menu && !s_prev_menu_open;
    if (show_menu != s_prev_menu_open)
        diag::RecordEvent(diag::EventCategory::Input, diag::EventSeverity::Info, 10,
                          show_menu ? "menu opened" : "menu closed");
    s_prev_menu_open = show_menu;
    if (fresh_menu) {
        const bool friends_page = s_friends_request.exchange(false, std::memory_order_relaxed);
        s_menu_page = friends_page ? MenuPage::Friends : MenuPage::Main;
        if (friends_page) friends_ui::s_focus_input = true;
        s_selected_mod_page = 0;
        s_mod_back_pressed = false;
        s_mod_toggle_error.clear();
        s_confirm_delete_mod_id.clear();
        s_graphics_page = gx2_graphics_menu::Page::None;
        s_capture = false;
        s_menu_anim = 0.0f;
    }
    if (!show_menu) s_menu_anim = 0.0f;
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
    if (show_menu) {
        const uint64_t req_page = s_mod_page_requested.exchange(0, std::memory_order_relaxed);
        if (req_page != 0) {
            s_selected_mod_page = req_page;
            s_mod_page_direct = true;
            s_focus_direct_canvas = true;
            s_menu_page = MenuPage::ModPage;
            s_graphics_page = gx2_graphics_menu::Page::None;
            s_capture = false;
        }
    }
    update_shader_activity();
    s_focused_action = -1;
    if (show_login) {
        ScopedPauseStyle login_style;
        online_login_ui::build_startup_window();
    } else if (show_menu && !s_quit_opened_from_gameplay.load(std::memory_order_relaxed)) {
        build_settings_window();
    }
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
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
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

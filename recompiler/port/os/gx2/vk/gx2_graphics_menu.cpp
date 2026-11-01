#include "gx2_graphics_menu.h"

#include "../gx2_env_light.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace gx2_graphics_menu {
namespace {
namespace rs = rbrew_settings;

// One switch for everything a beginner needs. The picture sliders are
// personal and stay as they are.
enum Preset { kOriginal, kBalanced, kRealistic, kCinematic, kPresetCount };
const char* const kPresetLabels[kPresetCount] = {"Original (fastest)", "Balanced", "Realistic", "Cinematic (no HUD)"};

// Turning the shaders on starts in a clean, clear day (no fog, fixed time),
// whatever time or weather was left from an earlier session.
void day_defaults(rs::Values& v) {
    v.weather = rs::kClear;
    v.time_of_day = 13 * 60;
    v.day_cycle = false;
    v.real_time = false;
    v.cloud_amount = -1;
    v.fog = 0;
}

void apply_preset(rs::Values& v, int preset) {
    if (preset == kOriginal) {
        v.shader_quality = rs::kShadersOff;
        return;
    }
    if (v.shader_quality == rs::kShadersOff) day_defaults(v);
    v.shader_quality = preset == kBalanced ? rs::kShadersBalanced : rs::kShadersHigh;
    v.clouds = v.big_cloud = v.sun_rays = v.realistic_look = v.antialiasing = v.film_look = v.motion_blur = true;
    v.bloom = preset == kCinematic ? 35 : 25;
    v.cinema_mode = v.camera_motion = preset == kCinematic;
    v.focus_blur = preset == kCinematic ? 40 : 0;
}

// -1: the options were changed one by one ("Custom").
int preset_of(const rs::Values& v) {
    for (int preset = 0; preset < kPresetCount; ++preset) {
        auto t = v;
        apply_preset(t, preset);
        if (t.shader_quality == v.shader_quality && t.clouds == v.clouds && t.big_cloud == v.big_cloud &&
            t.sun_rays == v.sun_rays && t.realistic_look == v.realistic_look && t.antialiasing == v.antialiasing &&
            t.film_look == v.film_look && t.motion_blur == v.motion_blur && t.bloom == v.bloom &&
            t.cinema_mode == v.cinema_mode && t.camera_motion == v.camera_motion &&
            (preset == kOriginal || t.focus_blur == v.focus_blur))
            return preset;
    }
    return -1;
}

const char* s_help = nullptr; // what the hovered/focused row does, shown at the bottom
int s_look_slot = 0, s_fly_spot = 0, s_spawn_spot = 0;

void help(const char* text) {
    if (ImGui::IsItemHovered() || ImGui::IsItemFocused() || ImGui::IsItemActive()) s_help = text;
    ImGui::SetItemTooltip("%s", text);
}

bool check(const char* label, bool& value, const char* text) {
    const bool changed = ImGui::Checkbox(label, &value);
    help(text);
    return changed;
}

bool slider(const char* label, int& value, int lo, int hi, const char* format, const char* text) {
    const bool changed = ImGui::SliderInt(label, &value, lo, hi, format);
    help(text);
    return changed;
}

bool combo(const char* label, int& value, const char* const* items, int count, const char* text) {
    const bool changed = ImGui::Combo(label, &value, items, count);
    help(text);
    return changed;
}

bool button(const char* label, const char* text) {
    const bool pressed = ImGui::Button(label);
    help(text);
    return pressed;
}

void section(const char* name) {
    ImGui::Spacing();
    ImGui::SeparatorText(name);
}

void footer() {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", s_help ? s_help : "Hover or select a row to see what it does.");
    ImGui::PopTextWrapPos();
}

bool build_graphics(rs::Values& v) {
    bool dirty = false;
    int preset = preset_of(v);
    const char* preview = preset < 0 ? "Custom" : kPresetLabels[preset];
    if (ImGui::BeginCombo("Preset", preview)) {
        for (int p = 0; p < kPresetCount; ++p)
            if (ImGui::Selectable(kPresetLabels[p], p == preset)) {
                apply_preset(v, p);
                dirty = true;
            }
        ImGui::EndCombo();
    }
    help("One switch for everything below. Changing an option makes it Custom.");
    static const char* const kQualities[] = {"Off (original)", "Low", "Balanced", "High"};
    const int quality_before = v.shader_quality;
    dirty |= combo("Shader quality", v.shader_quality, kQualities, rs::kShaderQualityCount,
                   "Higher: finer clouds and light. Off: the original game, unlocks 30/120/180 FPS.");
    if (quality_before == rs::kShadersOff && v.shader_quality != rs::kShadersOff) day_defaults(v);
    section("Sky and light");
    dirty |= check("Realistic colours", v.realistic_look, "Colours and contrast of a modern game.");
    dirty |= check("Volumetric clouds", v.clouds, "Real 3D clouds that drift with the wind.");
    dirty |= check("Big cumulus clouds", v.big_cloud, "Two towering clouds over the plaza (in clear weather).");
    dirty |= check("Sun rays", v.sun_rays, "Light shafts through clouds and around buildings.");
    dirty |= slider("Glow (bloom)", v.bloom, 0, 100, "%d %%", "Glow around the sun, lamps and bright signs.");
    dirty |= slider("Ground fog", v.fog, 0, 100, "%d %%", "Fog over the ground. 0 % = clear air.");
    dirty |= check("Edge smoothing", v.antialiasing, "Smooths jagged edges (FXAA + TAA).");
    dirty |= check("Film look", v.film_look, "Fine film grain and lens effects at the picture edges.");
    section("Picture");
    dirty |= slider("Saturation", v.saturation, 0, 200, "%d %%", "Colour strength. 100 % = normal.");
    dirty |= slider("Contrast", v.contrast, 50, 150, "%d %%", "Difference between dark and bright. 100 % = normal.");
    float ev = v.exposure / 4.0f;
    if (ImGui::SliderFloat("Brightness", &ev, -2.0f, 2.0f, "%+.2f EV")) {
        v.exposure = std::clamp(int(ev * 4.0f + (ev < 0 ? -0.5f : 0.5f)), -8, 8);
        dirty = true;
    }
    help("Overall brightness. 0 = normal.");
    dirty |= slider("Gamma / midtones", v.gamma, 50, 150, "%d %%", "100 % = neutral. Higher brightens midtones.");
    dirty |= slider("Shadow brightness", v.shadow_lift, -25, 25, "%d", "Lift or deepen dark areas. 0 = neutral.");
    dirty |= slider("Highlight softness", v.highlight_softness, 0, 100, "%d %%",
                    "Softens bright highlights while keeping their colour.");
    dirty |= slider("Sharpening", v.sharpness, 0, 200, "%d %%", "0 = off; 100 % = normal; higher adds stronger edges.");
    dirty |= slider("Surface clarity", v.clarity, 0, 200, "%d %%", "Fine local contrast on surfaces. 100 % = normal.");
    dirty |= slider("Colour warmth", v.white_balance, -50, 50, "%d",
                    "Warmer (golden) or cooler (blue) picture, like a camera's white balance.");
    dirty |= slider("Vignette", v.vignette, 0, 100, "%d %%", "Darker picture corners, like a camera lens. 0 % = off.");
    section("Saved looks");
    static const char* const kSlots[] = {"Slot 1", "Slot 2", "Slot 3"};
    combo("Look slot", s_look_slot, kSlots, 3, "Three personal picture presets.");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", v.looks[s_look_slot].saved ? "(saved)" : "(empty)");
    if (button("Save current look", "Save these picture controls in the selected slot.")) {
        rs::save_look(v, s_look_slot);
        dirty = true;
    }
    ImGui::SameLine();
    if (button("Load look", "Recall the selected picture preset.") && v.looks[s_look_slot].saved) {
        rs::load_look(v, s_look_slot);
        dirty = true;
    }
    if (button("Reset colours and brightness", "Reset the picture controls; saved looks stay.")) {
        v.saturation = 100;
        v.contrast = 100;
        v.exposure = 0;
        v.fog = 0;
        v.white_balance = 0;
        v.vignette = 0;
        v.gamma = 100;
        v.shadow_lift = 0;
        v.highlight_softness = 0;
        v.sharpness = 100;
        v.clarity = 100;
        dirty = true;
    }
    return dirty;
}

bool build_weather(rs::Values& v) {
    bool dirty = false;
    static const char* const kWeather[] = {"Clear",        "Overcast",       "Rain",
                                           "Thunderstorm", "After the rain", "Live (changes by itself)"};
    dirty |= combo("Weather", v.weather, kWeather, rs::kWeatherCount,
                   "Live: the weather changes by itself, with storms and sunshine.");
    int hour = v.time_of_day / 60, minute = v.time_of_day % 60;
    char clock[16];
    std::snprintf(clock, sizeof(clock), "%02d:%02d", hour, minute);
    int quarter = v.time_of_day / 15;
    if (ImGui::SliderInt("Time of day", &quarter, 0, 95, clock)) {
        v.time_of_day = quarter * 15;
        dirty = true;
    }
    help("Where the day starts (or the fixed time when the cycle is off).");
    dirty |= check("Day / night cycle", v.day_cycle, "The sun moves: sunrise, sunset and night follow.");
    dirty |= check("Use the PC clock", v.real_time, "The time of day follows your PC's clock.");
    dirty |= slider("Day length", v.day_length, 5, 120, "%d min", "How many real minutes a whole game day takes.");
    section("Clouds");
    bool cloud_auto = v.cloud_amount < 0;
    if (check("Cloud amount from the weather", cloud_auto, "On: the weather decides how cloudy the sky is.")) {
        v.cloud_amount = cloud_auto ? -1 : 30;
        dirty = true;
    }
    if (!cloud_auto) dirty |= slider("Cloud amount", v.cloud_amount, 0, 100, "%d %%", "How cloudy the sky is.");
    bool cumulus_auto = v.cumulus_amount < 0;
    if (check("Dense cumulus from the weather", cumulus_auto, "On: the weather decides the dense cumulus coverage.")) {
        v.cumulus_amount = cumulus_auto ? -1 : 30;
        dirty = true;
    }
    if (!cumulus_auto)
        dirty |=
            slider("Dense cumulus coverage", v.cumulus_amount, 0, 100, "%d %%", "How much dense cumulus there is.");
    dirty |= slider("Cloud speed", v.cloud_speed, 0, 400, "%d %%",
                    "Drift and evolution of all clouds. 0 % = frozen clouds.");
    dirty |= slider("Wind direction", v.wind_direction, 0, 345, "%d deg", "Where the clouds drift to.");
    return dirty;
}

bool build_camera(rs::Values& v) {
    bool dirty = false;
    static const char* const kAim[] = {"Classic (stick)", "Direct (PC)"};
    int aim = v.mouse_direct ? 1 : 0;
    if (combo("Mouse aiming", aim, kAim, 2,
              "Direct turns 1:1 like a PC game. Needs Motion Controls On in the game's options.")) {
        v.mouse_direct = aim == 1;
        dirty = true;
    }
    dirty |= slider("Mouse sensitivity", v.mouse_sensitivity, 25, 400, "%d %%",
                    "How far the camera turns per mouse movement.");
    dirty |=
        slider("Field of view", v.camera_fov, 40, 90, "%d", "Wider makes the plaza feel bigger. The game's own is 55.");
    dirty |= slider("Camera distance", v.camera_distance, 50, 250, "%d %%",
                    "How far behind your character the camera stays. 100 % = original.");
    dirty |= slider("Camera height", v.camera_height, -20, 60, "%d",
                    "Raises the camera above the game's own. 0 = original.");
    dirty |= check("Motion blur (experimental)", v.motion_blur, "Blur when the camera turns quickly.");
    dirty |= check("Camera wiggle", v.camera_motion, "The camera moves gently like a hand-held camera.");
    dirty |= slider("Wiggle strength", v.sway_strength, 25, 300, "%d %%", "How much the camera wiggles.");
    if (button("Reset camera to the original", "Field of view 55, distance 100 %, height 0: the game's own camera.")) {
        v.camera_fov = 55;
        v.camera_distance = 100;
        v.camera_height = 0;
        dirty = true;
    }
    return dirty;
}

bool build_cinema(rs::Values& v) {
    bool dirty = false;
    if (check("Cinema mode (no HUD, free camera)", v.cinema_mode,
              "No HUD and a free camera: mouse looks, WASD moves, Space/Ctrl up/down, Shift fast, F7 back.")) {
        gx2env::request(gx2env::Request::FlyCamera, v.cinema_mode ? 1 : 0);
        dirty = true;
    }
    if (button("Take a photo (F12)", "Saves the picture without menu and overlays to the Screenshots folder."))
        gx2env::request(gx2env::Request::Photo, 0);
    dirty |= slider("Focus blur", v.focus_blur, 0, 100, "%d %%",
                    "Sharp on what you look at, soft in front and behind. 0 % = off.");
    section("Free camera");
    bool fly = gx2env::fly_camera();
    if (check("Free fly camera (F7)", fly,
              "Mouse looks, WASD moves, Space/Ctrl up/down, Shift fast. The character waits."))
        gx2env::request(gx2env::Request::FlyCamera, fly ? 1 : 0);
    int zoom = int(gx2env::fly_fov() + 0.5f);
    if (ImGui::SliderInt("Free camera field of view", &zoom, 15, 110, "%d"))
        gx2env::request(gx2env::Request::FlyZoom, zoom);
    help("Zoom of the free camera. Also the mouse wheel or Q/E while flying.");
    static const char* const kSpots[] = {"Spot 1", "Spot 2", "Spot 3"};
    combo("Fly spot", s_fly_spot, kSpots, 3, "Three camera places to remember.");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", gx2env::fly_spot_saved(s_fly_spot) ? "(saved)" : "(empty)");
    if (button("Save this view", "Remembers where the camera is now."))
        gx2env::request(gx2env::Request::SaveFlySpot, s_fly_spot);
    ImGui::SameLine();
    if (button("Fly there", "Turns the fly camera on at the saved place. F7 returns to the game camera."))
        gx2env::request(gx2env::Request::GoFlySpot, s_fly_spot);
    section("Spawn (hack)");
    combo("Spawn spot", s_spawn_spot, kSpots, 3, "Three places for your character.");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", gx2env::spawn_saved(s_spawn_spot) ? "(saved)" : "(empty)");
    if (button("Save where the character stands", "Remembers your character's position."))
        gx2env::request(gx2env::Request::SaveSpawn, s_spawn_spot);
    if (button("Spawn at the spot", "Puts your character at the saved place."))
        gx2env::request(gx2env::Request::SpawnAt, s_spawn_spot);
    ImGui::SameLine();
    if (button("Spawn at the camera", "Puts your character in front of the (free) camera; it drops to the ground."))
        gx2env::request(gx2env::Request::SpawnAtCamera, 0);
    return dirty;
}

} // namespace

const char* title(Page page) {
    switch (page) {
    case Page::Graphics: return "Graphics (experimental)";
    case Page::Weather: return "Weather & time (experimental)";
    case Page::Camera: return "Camera & mouse";
    case Page::Cinema: return "Cinema, photo & hacks (experimental)";
    default: return "Settings";
    }
}

Page build_main_rows(rs::Values& v, bool& dirty) {
    s_help = nullptr;
    int preset = preset_of(v);
    // Graphics preset, Graphics and Weather pages: hidden while the new shaders do not work
    // (settings force shader_quality off). true brings them back.
    constexpr bool kShaderPages = false;
    if (kShaderPages && ImGui::BeginCombo("Graphics preset (experimental)", preset < 0 ? "Custom" : kPresetLabels[preset])) {
        for (int p = 0; p < kPresetCount; ++p)
            if (ImGui::Selectable(kPresetLabels[p], p == preset)) {
                apply_preset(v, p);
                dirty = true;
            }
        ImGui::EndCombo();
    }
    if (kShaderPages) help("One switch for the whole look. Original = the clean game at 60 FPS.");
    static const char* const kRates[] = {"30 (low-end PC)", "60", "120", "180 (plaza 120)"};
    static const int kRateValues[] = {30, 60, 120, 180};
    int rate = 1;
    for (int i = 0; i < 4; ++i)
        if (kRateValues[i] == v.frame_rate) rate = i;
    ImGui::BeginDisabled(v.shader_quality != rs::kShadersOff);
    if (combo("Frame rate", rate, kRates, 4, "120/180 add in-between frames; needs the graphics preset Original.")) {
        v.frame_rate = kRateValues[rate];
        v.fps120 = v.frame_rate >= 120;
        dirty = true;
    }
    ImGui::EndDisabled();
    dirty |= check("G-Sync / FreeSync", v.gsync, "For G-Sync/FreeSync monitors: frames are shown without waiting.");
    Page open = Page::None;
    if (kShaderPages) {
        if (button("Graphics (experimental)", "New shaders: clouds, sun rays, colours, picture quality."))
            open = Page::Graphics;
        ImGui::SameLine();
        if (button("Weather & time (experimental)", "Weather, time of day, day/night cycle and clouds."))
            open = Page::Weather;
    }
    if (button("Camera & mouse", "Mouse aiming, field of view, camera distance, wiggle.")) open = Page::Camera;
    if (!gx2env::online_mode()) {   // free camera and spawn hacks: offline only
        ImGui::SameLine();
        if (button("Cinema, photo & hacks (experimental)", "Cinema mode, photos, focus blur, free camera, spawn."))
            open = Page::Cinema;
    }
    return open;
}

bool build_page(Page page) {
    s_help = nullptr;
    auto v = rs::get();
    bool dirty = false;
    switch (page) {
    case Page::Graphics: dirty = build_graphics(v); break;
    case Page::Weather: dirty = build_weather(v); break;
    case Page::Camera: dirty = build_camera(v); break;
    case Page::Cinema: dirty = build_cinema(v); break;
    default: break;
    }
    if (dirty) rs::set(v);
    ImGui::Spacing();
    const bool back = ImGui::Button("Back");
    footer();
    return back;
}

} // namespace gx2_graphics_menu

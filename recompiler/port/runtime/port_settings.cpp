#include "port_settings.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
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

namespace rbrew_settings {

// Defaults: the port's original layout. Controller buttons are mapped by
// position like on the GamePad (south = B, east = A, west = Y, north = X).
void default_bindings(int (&keys)[kActionCount], int (&pad)[kActionCount]) {
    for (int i = 0; i < kActionCount; ++i) keys[i] = pad[i] = kUnbound;
    // SDL scancodes
    keys[kActA] = 40 /*Return*/;
    keys[kActB] = 44 /*Space*/;
    keys[kActX] = 20 /*Q*/;
    keys[kActY] = 8 /*E*/;
    keys[kActR] = 21 /*R*/;
    keys[kActPlus] = 9 /*F*/;
    keys[kActMinus] = 10 /*G*/;
    keys[kActHome] = 11 /*H*/;
    keys[kActZR] = kMouseBase + 1 /*left*/;
    keys[kActZL] = kMouseBase + 3 /*right*/;
    keys[kActMoveUp] = 26 /*W*/;
    keys[kActMoveDown] = 22 /*S*/;
    keys[kActMoveLeft] = 4 /*A*/;
    keys[kActMoveRight] = 7 /*D*/;
    // Every GamePad input has a key, like a PC game: special weapon (right
    // stick click) on the middle mouse button, the D-pad (callouts) on 1-4.
    keys[kActStickR] = kMouseBase + 2 /*middle*/;
    keys[kActStickL] = 6 /*C*/;
    keys[kActL] = 29 /*Z*/;
    keys[kActUp] = 30 /*1*/;
    keys[kActDown] = 31 /*2*/;
    keys[kActLeft] = 32 /*3*/;
    keys[kActRight] = 33 /*4*/;
    // SDL_GameControllerButton
    pad[kActB] = 0;
    pad[kActA] = 1;
    pad[kActY] = 2;
    pad[kActX] = 3;
    pad[kActMinus] = 4;
    pad[kActHome] = 5;
    pad[kActPlus] = 6;
    pad[kActStickL] = 7;
    pad[kActStickR] = 8;
    pad[kActL] = 9;
    pad[kActR] = 10;
    pad[kActUp] = 11;
    pad[kActDown] = 12;
    pad[kActLeft] = 13;
    pad[kActRight] = 14;
    pad[kActZL] = kPadTriggerLeft;
    pad[kActZR] = kPadTriggerRight;
}

namespace {
const char* const kActionKeys[kActionCount] = {
    "a",  "b",    "x",    "y",     "l",       "r",       "zl",      "zr",        "plus",      "minus",     "home",
    "up", "down", "left", "right", "stick_l", "stick_r", "move_up", "move_down", "move_left", "move_right"};
std::mutex s_lock;
Values s_values;
int s_active_resolution = k720p;
const char* const kResolutionNames[kResolutionCount] = {"720p", "1080p", "1440p", "native"};

std::filesystem::path file_path() {
#if defined(_WIN32)
    wchar_t module[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (length && length < MAX_PATH) return std::filesystem::path(module).parent_path() / "settings.ini";
#endif
    return "settings.ini";
}
bool env_set(const char* name) {
    const char* v = std::getenv(name);
    return v && *v;
}
bool env_on(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '1';
}
bool env_off(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] == '0';
}

void publish(const Values& v) {
    g_plaza_60fps.store(v.plaza_60fps, std::memory_order_relaxed);
    g_fps120.store(v.frame_rate >= 120, std::memory_order_relaxed);
    g_fps180.store(v.frame_rate == 180, std::memory_order_relaxed);
    g_fps30.store(v.frame_rate == 30, std::memory_order_relaxed);
    g_shader_quality.store(v.shader_quality, std::memory_order_relaxed);
    g_mouse_direct.store(v.mouse_direct, std::memory_order_relaxed);
    g_gsync.store(v.gsync, std::memory_order_relaxed);
    g_gyro.store(v.gyro, std::memory_order_relaxed);
    g_gyro_invert_pitch.store(v.gyro_invert_pitch, std::memory_order_relaxed);
    g_gyro_scale.store(v.gyro_sensitivity / 100.0f, std::memory_order_relaxed);
    g_mouse_scale.store(v.mouse_sensitivity / 100.0f, std::memory_order_relaxed);
    g_volume.store(v.volume / 100.0f, std::memory_order_relaxed);
    for (int i = 0; i < kActionCount; ++i) {
        g_keys[i].store(v.keys[i], std::memory_order_relaxed);
        g_pad[i].store(v.pad[i], std::memory_order_relaxed);
    }
}
} // namespace
void normalize(Values& v) {
    // The new shaders (graphics presets, weather) are hidden in this build: they do not work
    // yet, and online every player sees the same game. The code stays for later.
    v.shader_quality = kShadersOff;
    v.weather = std::clamp(v.weather, 0, kWeatherCount - 1);
    v.time_of_day = std::clamp(v.time_of_day, 0, 1439);
    v.day_length = std::clamp(v.day_length, 5, 120);
    v.bloom = std::clamp(v.bloom, 0, 100);
    v.cloud_amount = std::clamp(v.cloud_amount, -1, 100);
    v.cumulus_amount = std::clamp(v.cumulus_amount, -1, 100);
    v.fog = std::clamp(v.fog, 0, 100);
    v.camera_fov = std::clamp(v.camera_fov, 40, 90);
    v.cloud_speed = std::clamp(v.cloud_speed, 0, 400);
    v.wind_direction = ((v.wind_direction % 360) + 360) % 360;
    v.focus_blur = std::clamp(v.focus_blur, 0, 100);
    v.vignette = std::clamp(v.vignette, 0, 100);
    v.white_balance = std::clamp(v.white_balance, -50, 50);
    v.sway_strength = std::clamp(v.sway_strength, 25, 300);
    v.camera_distance = std::clamp(v.camera_distance, 50, 250);
    v.camera_height = std::clamp(v.camera_height, -20, 60);
    v.saturation = std::clamp(v.saturation, 0, 200);
    v.contrast = std::clamp(v.contrast, 50, 150);
    v.exposure = std::clamp(v.exposure, -8, 8);
    v.gamma = std::clamp(v.gamma, 50, 150);
    v.shadow_lift = std::clamp(v.shadow_lift, -25, 25);
    v.highlight_softness = std::clamp(v.highlight_softness, 0, 100);
    v.sharpness = std::clamp(v.sharpness, 0, 200);
    v.clarity = std::clamp(v.clarity, 0, 200);
    if (v.shader_quality != kShadersOff) {
        v.frame_rate = 60;
        v.plaza_60fps = true;
    }
    v.resolution = std::clamp(v.resolution, 0, kResolutionCount - 1);
    v.gyro_sensitivity = std::clamp(v.gyro_sensitivity, 25, 300);
    v.mouse_sensitivity = std::clamp(v.mouse_sensitivity, 25, 400);
    v.volume = std::clamp(v.volume, 0, 100);
    if (v.frame_rate != 30 && v.frame_rate != 120 && v.frame_rate != 180) v.frame_rate = 60;
    v.fps120 = v.frame_rate >= 120;
    for (int i = 0; i < kActionCount; ++i) {
        const int key = v.keys[i], button = v.pad[i];
        if (key < kUnbound || (key >= 512 && key < kMouseBase) || key > kMouseBase + 5) v.keys[i] = kUnbound;
        if (button < kUnbound || button == kPadTouchpad ||
            (button > 20 && button != kPadTriggerLeft && button != kPadTriggerRight) || i >= kFirstMoveAction)
            v.pad[i] = kUnbound;
    }
}
std::string encode_look(const PictureLook& look) {
    if (!look.saved) return {};
    std::ostringstream out;
    out << 1;
    for (int value : look.values) out << ',' << value;
    return out.str();
}
bool decode_look(std::string_view text, PictureLook& out) {
    std::istringstream in{std::string(text)};
    int version;
    char comma;
    PictureLook parsed;
    if (!(in >> version) || version != 1) return false;
    for (int& value : parsed.values)
        if (!(in >> comma) || comma != ',' || !(in >> value)) return false;
    in >> std::ws;
    if (!in.eof()) return false;
    parsed.saved = true;
    out = parsed;
    return true;
}
bool save_look(Values& v, int slot) {
    if (slot < 0 || slot >= int(v.looks.size())) return false;
    normalize(v);
    v.looks[slot] = {true,
                     {v.saturation, v.contrast, v.exposure, v.gamma, v.shadow_lift, v.highlight_softness, v.sharpness,
                      v.clarity, v.white_balance, v.bloom, v.vignette, int(v.film_look), v.focus_blur}};
    return true;
}
bool load_look(Values& v, int slot) {
    if (slot < 0 || slot >= int(v.looks.size()) || !v.looks[slot].saved) return false;
    const auto& a = v.looks[slot].values;
    v.saturation = a[0];
    v.contrast = a[1];
    v.exposure = a[2];
    v.gamma = a[3];
    v.shadow_lift = a[4];
    v.highlight_softness = a[5];
    v.sharpness = a[6];
    v.clarity = a[7];
    v.white_balance = a[8];
    v.bloom = a[9];
    v.vignette = a[10];
    v.film_look = a[11] != 0;
    v.focus_blur = a[12];
    normalize(v);
    return true;
}
namespace {
// Lines of the existing file whose key this menu does not own (online_server=,
// pseudo=, ...): they are written back so saving the menu never drops them.
std::string foreign_lines() {
    std::string kept;
    std::ifstream in(file_path());
    for (std::string line; in && std::getline(in, line);) {
        if (!line.empty() && line.back() == 13) line.pop_back();   // CR
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        static const char* const kOwned[] = {
            "resolution", "plaza_60fps", "frame_rate", "gsync", "fullscreen", "fps_overlay", "simple_overlay",
            "gyro", "gyro_sensitivity", "gyro_invert_pitch", "mouse_sensitivity", "volume", "shader_quality",
            "weather", "time_of_day", "day_cycle", "day_length", "clouds", "sun_rays", "bloom", "antialiasing",
            "motion_blur", "camera_fov", "focus_blur", "vignette", "white_balance", "sway_strength", "cloud_speed",
            "wind_direction", "mouse_direct", "camera_distance", "camera_height", "realistic_look", "cloud_amount",
            "big_cloud", "cumulus_amount", "real_time", "fog", "saturation", "contrast", "exposure", "gamma",
            "shadow_lift", "highlight_softness", "sharpness", "clarity", "film_look", "camera_motion", "cinema_mode",
            "bindings"};
        bool own = key.rfind("look_", 0) == 0;
        for (const char* name : kOwned) own = own || key == name;
        for (int i = 0; i < kActionCount && !own; ++i)
            own = key == std::string("key_") + kActionKeys[i] || key == std::string("pad_") + kActionKeys[i];
        if (!own) kept += line + '\n';
    }
    return kept;
}
void write(const Values& v) {
    const std::string foreign = foreign_lines();
    std::ofstream out(file_path(), std::ios::trunc);
    if (!out) return;
    out << "# Splatoon PC port settings (ESC menu)\n"
        << "resolution=" << kResolutionNames[v.resolution] << "\n"
        << "plaza_60fps=" << v.plaza_60fps << "\n"
        << "frame_rate=" << v.frame_rate << "\n"
        << "gsync=" << v.gsync << "\n"
        << "fullscreen=" << v.fullscreen << "\n"
        << "fps_overlay=" << v.fps_overlay << "\n"
        << "simple_overlay=" << v.simple_overlay << "\n"
        << "gyro=" << v.gyro << "\n"
        << "gyro_sensitivity=" << v.gyro_sensitivity << "\n"
        << "gyro_invert_pitch=" << v.gyro_invert_pitch << "\n"
        << "mouse_sensitivity=" << v.mouse_sensitivity << "\n"
        << "volume=" << v.volume << "\n"
        << "shader_quality=" << v.shader_quality << "\n"
        << "weather=" << v.weather << "\n"
        << "time_of_day=" << v.time_of_day << "\n"
        << "day_cycle=" << v.day_cycle << "\n"
        << "day_length=" << v.day_length << "\n"
        << "clouds=" << v.clouds << "\n"
        << "sun_rays=" << v.sun_rays << "\n"
        << "bloom=" << v.bloom << "\n"
        << "antialiasing=" << v.antialiasing << "\n"
        << "motion_blur=" << v.motion_blur << "\n"
        << "camera_fov=" << v.camera_fov << "\n"
        << "focus_blur=" << v.focus_blur << "\n"
        << "vignette=" << v.vignette << "\n"
        << "white_balance=" << v.white_balance << "\n"
        << "sway_strength=" << v.sway_strength << "\n"
        << "cloud_speed=" << v.cloud_speed << "\n"
        << "wind_direction=" << v.wind_direction << "\n"
        << "mouse_direct=" << v.mouse_direct << "\n"
        << "camera_distance=" << v.camera_distance << "\n"
        << "camera_height=" << v.camera_height << "\n"
        << "realistic_look=" << v.realistic_look << "\n"
        << "cloud_amount=" << v.cloud_amount << "\n"
        << "big_cloud=" << v.big_cloud << "\n"
        << "cumulus_amount=" << v.cumulus_amount << "\n"
        << "real_time=" << v.real_time << "\n"
        << "fog=" << v.fog << "\n"
        << "saturation=" << v.saturation << "\n"
        << "contrast=" << v.contrast << "\n"
        << "exposure=" << v.exposure << "\n"
        << "gamma=" << v.gamma << "\n"
        << "shadow_lift=" << v.shadow_lift << "\n"
        << "highlight_softness=" << v.highlight_softness << "\n"
        << "sharpness=" << v.sharpness << "\n"
        << "clarity=" << v.clarity << "\n"
        << "film_look=" << v.film_look << "\n"
        << "camera_motion=" << v.camera_motion << "\n"
        << "cinema_mode=" << v.cinema_mode << "\n"
        << "bindings=2\n";
    for (unsigned i = 0; i < v.looks.size(); ++i)
        if (v.looks[i].saved) out << "look_" << i + 1 << '=' << encode_look(v.looks[i]) << '\n';
    for (int i = 0; i < kActionCount; ++i)
        out << "key_" << kActionKeys[i] << "=" << v.keys[i] << "\n" << "pad_" << kActionKeys[i] << "=" << v.pad[i] << "\n";
    out << foreign;
}
} // namespace

void load() {
    std::lock_guard lock(s_lock);
    Values v;
    bool saw_frame_rate = false;
    int bindings_version = 1;
    std::ifstream in(file_path());
    for (std::string line; in && std::getline(in, line);) {
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        const int number = std::atoi(value.c_str());
        if (key == "resolution") {
            for (int i = 0; i < kResolutionCount; ++i)
                if (value == kResolutionNames[i]) v.resolution = i;
        } else if (key == "plaza_60fps")
            v.plaza_60fps = number != 0;
        else if (key == "fps120")
            v.fps120 = number != 0; // before frame_rate existed
        else if (key == "frame_rate") {
            v.frame_rate = number;
            saw_frame_rate = true;
        } else if (key == "gsync")
            v.gsync = number != 0;
        else if (key == "fullscreen")
            v.fullscreen = number != 0;
        else if (key == "fps_overlay")
            v.fps_overlay = number != 0;
        else if (key == "simple_overlay")
            v.simple_overlay = number != 0;
        else if (key == "gyro")
            v.gyro = number != 0;
        else if (key == "gyro_sensitivity")
            v.gyro_sensitivity = number;
        else if (key == "gyro_invert_pitch")
            v.gyro_invert_pitch = number != 0;
        else if (key == "mouse_sensitivity")
            v.mouse_sensitivity = number;
        else if (key == "volume")
            v.volume = number;
        else if (key == "shader_quality")
            v.shader_quality = number;
        else if (key == "weather")
            v.weather = number;
        else if (key == "time_of_day")
            v.time_of_day = number;
        else if (key == "day_cycle")
            v.day_cycle = number != 0;
        else if (key == "day_length")
            v.day_length = number;
        else if (key == "clouds")
            v.clouds = number != 0;
        else if (key == "sun_rays")
            v.sun_rays = number != 0;
        else if (key == "bloom")
            v.bloom = number;
        else if (key == "antialiasing")
            v.antialiasing = number != 0;
        else if (key == "motion_blur")
            v.motion_blur = number != 0;
        else if (key == "camera_fov")
            v.camera_fov = number;
        else if (key == "focus_blur")
            v.focus_blur = number;
        else if (key == "vignette")
            v.vignette = number;
        else if (key == "white_balance")
            v.white_balance = number;
        else if (key == "sway_strength")
            v.sway_strength = number;
        else if (key == "cloud_speed")
            v.cloud_speed = number;
        else if (key == "wind_direction")
            v.wind_direction = number;
        else if (key == "mouse_direct")
            v.mouse_direct = number != 0;
        else if (key == "camera_distance")
            v.camera_distance = number;
        else if (key == "camera_height")
            v.camera_height = number;
        else if (key == "realistic_look")
            v.realistic_look = number != 0;
        else if (key == "cloud_amount")
            v.cloud_amount = number;
        else if (key == "big_cloud")
            v.big_cloud = number != 0;
        else if (key == "cumulus_amount")
            v.cumulus_amount = number;
        else if (key == "real_time")
            v.real_time = number != 0;
        else if (key == "fog")
            v.fog = number;
        else if (key == "saturation")
            v.saturation = number;
        else if (key == "contrast")
            v.contrast = number;
        else if (key == "exposure")
            v.exposure = number;
        else if (key == "gamma")
            v.gamma = number;
        else if (key == "shadow_lift")
            v.shadow_lift = number;
        else if (key == "highlight_softness")
            v.highlight_softness = number;
        else if (key == "sharpness")
            v.sharpness = number;
        else if (key == "clarity")
            v.clarity = number;
        else if (key.size() == 6 && key.rfind("look_", 0) == 0 && key[5] >= '1' && key[5] <= '3')
            decode_look(value, v.looks[key[5] - '1']);
        else if (key == "film_look")
            v.film_look = number != 0;
        else if (key == "camera_motion")
            v.camera_motion = number != 0;
        else if (key == "cinema_mode")
            v.cinema_mode = number != 0;
        else if (key == "bindings")
            bindings_version = number;
        else
            for (int i = 0; i < kActionCount; ++i) {
                if (key == std::string("key_") + kActionKeys[i]) v.keys[i] = number;
                if (key == std::string("pad_") + kActionKeys[i]) v.pad[i] = number;
            }
    }
    if (!saw_frame_rate && v.fps120) v.frame_rate = 120;
    // Settings from before every input had a key (special weapon, D-pad):
    // the new default keys fill the inputs left unbound, once, where no other
    // input uses that key.
    // Before this version the wider 62 degree view was the default; the
    // camera now starts as the game's own and every change is optional.
    if (bindings_version < 2 && v.camera_fov == 62) v.camera_fov = 55;
    if (bindings_version < 2) {
        int keys[kActionCount], pad[kActionCount];
        default_bindings(keys, pad);
        for (const int action : {kActStickR, kActStickL, kActL, kActUp, kActDown, kActLeft, kActRight}) {
            if (v.keys[action] != kUnbound) continue;
            bool used = false;
            for (int other = 0; other < kActionCount; ++other) used |= v.keys[other] == keys[action];
            if (!used) v.keys[action] = keys[action];
        }
    }
    // Environment overrides (see header).
    if (const char* q = std::getenv("RBREW_SHADER_QUALITY"); q && *q) {
        const char* names[] = {"off", "low", "balanced", "high"};
        for (int i = 0; i < kShaderQualityCount; ++i)
            if (!std::strcmp(q, names[i])) v.shader_quality = i;
    }
    if (env_set("RBREW_WEATHER")) v.weather = std::atoi(std::getenv("RBREW_WEATHER"));
    if (env_set("RBREW_MOTION_BLUR")) v.motion_blur = std::getenv("RBREW_MOTION_BLUR")[0] == '1';
    if (env_set("RBREW_CAMERA_DISTANCE")) v.camera_distance = std::atoi(std::getenv("RBREW_CAMERA_DISTANCE"));
    if (env_set("RBREW_CAMERA_HEIGHT")) v.camera_height = std::atoi(std::getenv("RBREW_CAMERA_HEIGHT"));
    if (env_set("RBREW_MOUSE_DIRECT")) v.mouse_direct = std::getenv("RBREW_MOUSE_DIRECT")[0] == '1';
    if (env_set("RBREW_CLOUD_SPEED")) v.cloud_speed = std::atoi(std::getenv("RBREW_CLOUD_SPEED"));
    if (env_set("RBREW_FOCUS_BLUR")) v.focus_blur = std::atoi(std::getenv("RBREW_FOCUS_BLUR"));
    if (env_set("RBREW_VIGNETTE")) v.vignette = std::atoi(std::getenv("RBREW_VIGNETTE"));
    if (env_set("RBREW_WHITE_BALANCE")) v.white_balance = std::atoi(std::getenv("RBREW_WHITE_BALANCE"));
    if (env_set("RBREW_CAMERA_FOV")) v.camera_fov = std::atoi(std::getenv("RBREW_CAMERA_FOV"));
    if (env_set("RBREW_FOG")) v.fog = std::atoi(std::getenv("RBREW_FOG"));
    if (env_set("RBREW_CLOUD_AMOUNT")) v.cloud_amount = std::atoi(std::getenv("RBREW_CLOUD_AMOUNT"));
    if (env_set("RBREW_CUMULUS_AMOUNT")) v.cumulus_amount = std::atoi(std::getenv("RBREW_CUMULUS_AMOUNT"));
    if (env_set("RBREW_LOOK")) v.realistic_look = std::getenv("RBREW_LOOK")[0] == '1';
    if (env_set("RBREW_CINEMA")) v.cinema_mode = std::getenv("RBREW_CINEMA")[0] == '1';
    if (env_set("RBREW_TIME_OF_DAY")) v.time_of_day = std::atoi(std::getenv("RBREW_TIME_OF_DAY"));
    if (env_set("RBREW_DAY_CYCLE")) v.day_cycle = env_on("RBREW_DAY_CYCLE");
    if (env_set("RBREW_DAY_LENGTH")) v.day_length = std::atoi(std::getenv("RBREW_DAY_LENGTH"));
    if (env_set("RBREW_PLAZA_60FPS")) v.plaza_60fps = !env_off("RBREW_PLAZA_60FPS");
    if (const char* f = std::getenv("RBREW_FPS120"); f && *f)
        v.frame_rate = !std::strcmp(f, "interp") ? (v.frame_rate == 180 ? 180 : 120) : 60;
    if (const char* f = std::getenv("RBREW_FRAME_RATE"); f && *f) v.frame_rate = std::atoi(f); // 30, 60, 120, 180
    normalize(v);
    if (const char* m = std::getenv("RBREW_VK_PRESENT_MODE"); m && *m) v.gsync = !std::strcmp(m, "immediate");
    if (env_set("RBREW_FPS_OVERLAY")) v.fps_overlay = !env_off("RBREW_FPS_OVERLAY");
    if (env_set("RBREW_SIMPLE_OVERLAY")) v.simple_overlay = !env_off("RBREW_SIMPLE_OVERLAY");
    if (env_set("RBREW_GYRO")) v.gyro = !env_off("RBREW_GYRO");
    if (env_set("RBREW_GYRO_INVERT_PITCH")) v.gyro_invert_pitch = env_on("RBREW_GYRO_INVERT_PITCH");
    if (const char* r = std::getenv("RBREW_RENDER_RESOLUTION"); r && *r) {
        v.resolution = -1;
        for (int i = 0; i < kResolutionCount; ++i)
            if (!std::strcmp(r, kResolutionNames[i])) v.resolution = i;
        if (!std::strcmp(r, "1280x720")) v.resolution = k720p;
        if (!std::strcmp(r, "1920x1080")) v.resolution = k1080p;
        if (!std::strcmp(r, "2560x1440")) v.resolution = k1440p;
        s_active_resolution = v.resolution < 0 ? k720p : v.resolution;
        if (v.resolution < 0) v.resolution = k720p; // custom WxH: the menu shows 720p
    } else {
        s_active_resolution = v.resolution;
#if defined(_WIN32)
        if (v.resolution != k720p) _putenv_s("RBREW_RENDER_RESOLUTION", kResolutionNames[v.resolution]);
#endif
    }
    s_values = v;
    publish(v);
}
Values get() {
    std::lock_guard lock(s_lock);
    return s_values;
}
void set(const Values& values) {
    Values v = values;
    normalize(v);
    std::lock_guard lock(s_lock);
    s_values = v;
    publish(v);
    write(v);
}
int active_resolution() {
    std::lock_guard lock(s_lock);
    return s_active_resolution;
}

} // namespace rbrew_settings

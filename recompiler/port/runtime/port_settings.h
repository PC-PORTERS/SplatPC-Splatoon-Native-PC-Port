// port_settings.h - player options of the PC port (ESC menu), kept in
// settings.ini next to Splatoon.exe.
//
// Environment switches still win: a variable that is set (RBREW_PLAZA_60FPS,
// RBREW_GYRO, RBREW_RENDER_RESOLUTION, ...) decides the starting value and
// the file is not consulted for it, so tests and benchmark runs stay
// reproducible. Values read on hot paths are atomics.
#pragma once
#include <atomic>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace rbrew_settings {

enum Resolution : int { k720p, k1080p, k1440p, kNative, kResolutionCount };
enum ShaderQuality : int { kShadersOff, kShadersLow, kShadersBalanced, kShadersHigh, kShaderQualityCount };
enum Weather : int { kClear, kCloudy, kRain, kStorm, kAfterRain, kLiveWeather, kWeatherCount };

// GamePad inputs that keyboard/mouse and controller buttons can be bound to.
enum Action : int {
    kActA,
    kActB,
    kActX,
    kActY,
    kActL,
    kActR,
    kActZL,
    kActZR,
    kActPlus,
    kActMinus,
    kActHome,
    kActUp,
    kActDown,
    kActLeft,
    kActRight,
    kActStickL,
    kActStickR,
    kActMoveUp,
    kActMoveDown,
    kActMoveLeft,
    kActMoveRight, // left stick (keyboard only)
    kActionCount
};
constexpr int kFirstMoveAction = kActMoveUp;
constexpr int kUnbound = -1;
// Keyboard bindings are SDL scancodes; kMouseBase + SDL_BUTTON_* is a mouse button.
constexpr int kMouseBase = 1000;
// Controller bindings are SDL_GameControllerButton values, or the triggers.
constexpr int kPadTriggerLeft = 100, kPadTriggerRight = 101;
// The touchpad button opens the menu and cannot be bound.
constexpr int kPadTouchpad = 20;
void default_bindings(int (&keys)[kActionCount], int (&pad)[kActionCount]);

struct PictureLook {
    bool saved = false;
    std::array<int, 13> values{};
};
struct Values {
    int resolution = k720p;  // takes effect at the next start
    bool plaza_60fps = true; // RBREW_PLAZA_60FPS
    bool fps120 = false;     // RBREW_FPS120=interp: in-between frames (frame_rate 120 or 180)
    int frame_rate = 60;     // 60, 120, 180 (two in-between frames, not in the plaza), 30 (low-end)
    bool gsync = false;      // frames go to the display at once (IMMEDIATE) for G-Sync/FreeSync
    bool fullscreen = true;
    bool borderless = false;        // windowed, no border, desktop size (used when fullscreen is off)
    bool gamepad_window = false;    // the GamePad screen in a second window (two screens)
    bool phone_gamepad = false;     // the GamePad screen and touch on a phone browser (pad_remote.cpp)
    int phone_port = 8771;          // its local network port
    bool fps_overlay = false;       // debug overlay (F3), hidden at launch; RBREW_FPS_OVERLAY
    bool simple_overlay = false;    // FPS / frame time / max FPS readout; RBREW_SIMPLE_OVERLAY
    bool confirm_quit = true;       // confirmation popup before quitting
    bool gyro = true;               // RBREW_GYRO
    int gyro_sensitivity = 100;     // percent
    bool gyro_invert_pitch = false; // RBREW_GYRO_INVERT_PITCH
    int mouse_sensitivity = 100;    // percent
    bool mouse_direct = true;       // mouse turns the camera 1:1 (motion aiming), not through the stick
    int volume = 100;               // percent
    int shader_quality =
        kShadersOff; // first start: the clean original game at 60 FPS; the graphics preset turns the shaders on
    int weather = kClear;
    int time_of_day = 13 * 60; // minutes, initial/fixed time (a clear day when the shaders go on)
    bool day_cycle = false;    // fixed time until the player turns the cycle on
    int day_length = 24;       // real minutes per full day
    bool clouds = true;
    int cloud_amount = -1;   // percent, -1 = from the weather
    int wind_direction = 0;  // degrees the clouds drift towards (0 = the game's default)
    int cloud_speed = 100;   // percent of the wind speed the clouds drift with (0 = still)
    bool big_cloud = true;   // the towering cumulus behind the tower
    int cumulus_amount = -1; // dense cloud-bank coverage; -1 follows the weather
    bool real_time = false;  // time of day follows the PC clock
    int fog = 0;             // percent, volumetric height fog
    int saturation = 100;    // percent
    int contrast = 100;      // percent
    int exposure = 0;        // quarter stops (-8..8)
    int gamma = 100, shadow_lift = 0, highlight_softness = 0, sharpness = 100, clarity = 100;
    std::array<PictureLook, 3> looks{};
    bool sun_rays = true;
    int bloom = 25;             // percent
    bool antialiasing = true;   // FXAA on the final image
    bool motion_blur = true;    // camera motion blur (with TAA)
    int camera_fov = 55;        // degrees, the game's own (wider is optional)
    int camera_distance = 100;  // percent of the game's camera distance
    int camera_height = 0;      // game units above the game's camera
    bool realistic_look = true; // colour grade fitted to the realistic reference images
    bool film_look = true;      // fine grain and lens fringes at the edges
    bool camera_motion = false; // a gentle hand-held sway (cinema mode only)
    int focus_blur = 0;         // percent: background/foreground blur around the screen centre (cinema/photo)
    int vignette = 0;           // percent: darker picture corners
    int white_balance = 0;      // -50 cool .. +50 warm
    int sway_strength = 100;    // percent of the cinema camera sway
    bool cinema_mode = false;   // no HUD in the plaza, only the picture
    int keys[kActionCount];     // keyboard/mouse binding per action
    int pad[kActionCount];      // controller binding per action
    Values() { default_bindings(keys, pad); }
};

// Reads settings.ini once (main, before any subsystem reads the environment)
// and exports the start-only values (resolution) to the environment.
void load();
Values get();
// Stores and writes settings.ini.
void set(const Values& values);
// Shared validation, including the mandatory 60 FPS shader mode.
void normalize(Values& values);
std::string encode_look(const PictureLook& look);
bool decode_look(std::string_view text, PictureLook& out);
bool save_look(Values& values, int slot);
bool load_look(Values& values, int slot);
// Resolution the renderer started with (the menu marks a pending change).
int active_resolution();

// Hot-path views of get().
inline std::atomic<bool> g_plaza_60fps{true};
inline std::atomic<bool> g_fps120{false}; // 120 or 180: in-between frames
inline std::atomic<bool> g_fps180{false}; // 180: two in-between frames per step
inline std::atomic<bool> g_fps30{false};  // 30: two steps per drawn frame
inline std::atomic<bool> g_gsync{false};
inline std::atomic<bool> g_gamepad_window{false};
inline std::atomic<bool> g_phone_gamepad{false};
inline std::atomic<int> g_phone_port{8771};
inline std::atomic<bool> g_gyro{true};
inline std::atomic<bool> g_gyro_invert_pitch{false};
inline std::atomic<float> g_gyro_scale{1.0f};
inline std::atomic<float> g_mouse_scale{1.0f};
inline std::atomic<bool> g_mouse_direct{true};
inline std::atomic<float> g_volume{1.0f};
inline std::atomic<bool> g_confirm_quit{true};
inline std::atomic<int> g_shader_quality{kShadersOff}; // load() publishes the player's selection
inline std::atomic<int> g_keys[kActionCount];
inline std::atomic<int> g_pad[kActionCount];

} // namespace rbrew_settings

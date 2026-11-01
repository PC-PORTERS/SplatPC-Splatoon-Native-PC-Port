#include "../../runtime/rbrew_diagnostics.h"
// VPAD (Virtual Pad — Wii U GamePad)
// VPADRead maps SDL2 gamepad/keyboard/mouse input to VPADStatus.
//
// Keyboard + mouse layout (Splatoon / twin-stick-shooter style):
//   W/A/S/D          → left stick (move)
//   Mouse move (TV)  → right stick (aim / camera — relative mode)
//   LMB              → ZR  (fire / shoot)
//   RMB              → ZL  (squid form / aim / swim in ink)
//   Space            → B   (jump)
//   Q                → X   (sub weapon)
//   E                → Y   (special weapon)
//   R                → R   (reset camera / unused)
//   F                → +   (map screen / pause)
//   G                → −   (options)
//   Escape           → Home
//
// Controller (any SDL2 game controller, buttons by position — see
// gx2_context.cpp, which turns off SDL's Nintendo label layout):
//   south/east/west/north → B/A/Y/X, exactly where they sit on a GamePad
//   LB/RB → L/R, LT/RT (analog or digital ZL/ZR) → ZL/ZR
//   stick clicks → L3/R3, Start/Back/Guide → +/−/Home
//
// In Gamepad View (Tab), the mouse is freed for touch-screen simulation.

#include "../os_common.h"
#include "../../runtime/screen_mode.h"
#include "../../runtime/native_control_defaults.h"
#include "../../runtime/benchmark_input.h"
#include "game_controllers.h"
#include "../gx2/vk/gx2_imgui_overlay.h"
#include "../gx2/gx2_env_light.h"
#include "../../runtime/port_settings.h"
#include <algorithm>
#include <fstream>
#include <cstring>
#include <vector>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <mutex>
#include <utility>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// SDL2 types are needed wherever HAVE_SDL2 code uses SDL_GameController, etc.
#if defined(HAVE_SDL2)
#include <SDL2/SDL.h>
#endif

// VPADStatus (big-endian, 0xAC bytes; matches the sample stride the game's
// VPADRead caller uses, 16 samples at device+0x14):
//   +0x00 hold, +0x04 trigger, +0x08 release
//   +0x0C leftStick (x, y), +0x14 rightStick (x, y)   — y > 0 is up
//   +0x1C acc (x, y, z), +0x28 accMagnitude, +0x2C accAcceleration,
//   +0x30 accXY (vertical, 2 floats)
//   +0x38 gyro (x, y, z), +0x44 angle (x, y, z), +0x50 s8 error
//   +0x52 tpNormal, +0x5A tpFiltered1, +0x62 tpFiltered2 (VPADTouchData:
//         u16 x, y, touched, validity)
//   +0x6C direction (3 x vec3: x axis, y axis, z axis), +0x90 headphones
//   +0x94 magnetometer, +0xA0 slideVolume, +0xA1 battery, +0xA2 mic,
//   +0xA3 slideVolumeEx
static constexpr uint32_t kVpadStatusSize = 0xAC;
static constexpr uint32_t kVpadTpNormal = 0x52, kVpadTpFiltered1 = 0x5A, kVpadTpFiltered2 = 0x62;
static constexpr uint32_t kVpadAccXY = 0x30, kVpadDirection = 0x6C;
static constexpr uint32_t kVpadBattery = 0xA1, kVpadSlideVolume = 0xA0, kVpadSlideVolumeEx = 0xA3;

#define VPAD_BUTTON_A 0x00008000u
#define VPAD_BUTTON_B 0x00004000u
#define VPAD_BUTTON_X 0x00002000u
#define VPAD_BUTTON_Y 0x00001000u
#define VPAD_BUTTON_LEFT 0x00000800u
#define VPAD_BUTTON_RIGHT 0x00000400u
#define VPAD_BUTTON_UP 0x00000200u
#define VPAD_BUTTON_DOWN 0x00000100u
#define VPAD_BUTTON_ZL 0x00000080u
#define VPAD_BUTTON_ZR 0x00000040u
#define VPAD_BUTTON_L 0x00000020u
#define VPAD_BUTTON_R 0x00000010u
#define VPAD_BUTTON_PLUS 0x00000008u
#define VPAD_BUTTON_MINUS 0x00000004u
#define VPAD_BUTTON_HOME 0x00000002u
#define VPAD_BUTTON_STICK_R 0x00020000u
#define VPAD_BUTTON_STICK_L 0x00040000u
// GamePad bit per bindable action (rbrew_settings::Action order).
static constexpr uint32_t kActionButtons[rbrew_settings::kFirstMoveAction] = {
    VPAD_BUTTON_A,    VPAD_BUTTON_B,    VPAD_BUTTON_X,     VPAD_BUTTON_Y,       VPAD_BUTTON_L,      VPAD_BUTTON_R,
    VPAD_BUTTON_ZL,   VPAD_BUTTON_ZR,   VPAD_BUTTON_PLUS,  VPAD_BUTTON_MINUS,   VPAD_BUTTON_HOME,   VPAD_BUTTON_UP,
    VPAD_BUTTON_DOWN, VPAD_BUTTON_LEFT, VPAD_BUTTON_RIGHT, VPAD_BUTTON_STICK_L, VPAD_BUTTON_STICK_R};
// Stick direction emulation bits (hold/trigger/release like buttons).
#define VPAD_STICK_R_EMULATION_DOWN 0x00800000u
#define VPAD_STICK_R_EMULATION_UP 0x01000000u
#define VPAD_STICK_R_EMULATION_RIGHT 0x02000000u
#define VPAD_STICK_R_EMULATION_LEFT 0x04000000u
#define VPAD_STICK_L_EMULATION_DOWN 0x08000000u
#define VPAD_STICK_L_EMULATION_UP 0x10000000u
#define VPAD_STICK_L_EMULATION_RIGHT 0x20000000u
#define VPAD_STICK_L_EMULATION_LEFT 0x40000000u

#define VPAD_TP_VALID 0u
#define VPAD_TP_INVALID_XY 3u

// Mouse sensitivity for right-stick camera: stick_delta = pixel_delta * sensitivity.
// ~200 px swipe across the screen ≈ full right-stick deflection at this value.
static constexpr float KBM_MOUSE_SENSITIVITY = 0.005f;
// Mouse aiming through the virtual GamePad (motion controls): degrees of pad
// rotation per mouse count at sensitivity 1. Linear, no clamp, no curve, so
// a mouse motion always turns the camera by the same angle, as in PC games.
// The stick path makes a mouse delta a stick deflection, which the game then
// ramps up (its stick acceleration) and which saturates at full deflection.
static constexpr float KBM_MOUSE_DEGREES = 0.06f;

// True while the mouse, not a controller's right stick, is aiming.
static bool s_mouse_aims = false;

// The behind-the-player camera (GamePlayerBehindCamera) turns back behind the
// player at 0.03 of the gap per frame: walking sideways swings the camera,
// which helps on a controller and fights a mouse. That rate is the rodata
// float 0x100E9C14 in 288, read by the camera update 026621E8 and four other
// camera functions. Measured 2026-10-26 (scripted tutorial, camera matrix
// trace): 2 s of strafing turned the camera +79.9 deg, +2.6 deg with the rate
// at 1%; stick/gyro turning was identical. While the mouse aims the rate is
// 1% of the original; the original returns as soon as a controller stick
// aims. Never written unless the value found is one of the two, and only
// for 288.
static void set_camera_follow(uint8_t* mem, bool follow) {
#if RBREW_GAME_REVISION == 288
    constexpr uint32_t kRate = 0x100E9C14u;
    constexpr float kOriginal = 0.03f, kMouse = 0.0003f;
    static int state = -1;
    if (!mem || state == int(follow)) return;
    const float current = float(rbrew_read_f32(mem, kRate));
    if (current == kOriginal || current == kMouse) rbrew_write_f32(mem, kRate, follow ? kOriginal : kMouse);
    state = int(follow);
#else
    (void)mem; (void)follow;
#endif
}

// Keep the tutorial's working virtual-motion camera, driven by right stick
// or mouse. Outside the tutorial, including Plaza, the active options follow
// the save: sensitivity 0 (neutral) and the player's own motion setting.
// No progression or tutorial-completion flags are changed.
void vpad_apply_motion_policy(uint8_t* mem) {
    // Recording and replay sessions apply it every 6th game frame instead of
    // every 100 ms: a wall-clock step lands on other frames at another frame
    // rate, and replays of the tutorial drifted apart (stuck at the motion
    // prompts).
    if (rbrew_benchmark::recording() || rbrew_benchmark::replaying()) {
        static uint32_t s_last_frame = UINT32_MAX;
        const uint32_t frame = rbrew_benchmark::frame.load(std::memory_order_relaxed);
        if (s_last_frame == UINT32_MAX || frame - s_last_frame >= 6) {
            s_last_frame = frame;
            rbrew_control_defaults::apply_live(mem);
        }
        return;
    }
    static auto s_last_policy = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    auto now = std::chrono::steady_clock::now();
    if (now - s_last_policy > std::chrono::milliseconds(100)) {
        s_last_policy = now;
        rbrew_control_defaults::apply_live(mem);
    }
}

// Write a VPADTouchData entry (8 bytes, big-endian u16 fields) at 'base'.
static void write_touch_data(uint8_t* mem, uint32_t base, uint16_t x, uint16_t y, uint16_t touched, uint16_t validity) {
    rbrew_write16(mem, base + 0, x);
    rbrew_write16(mem, base + 2, y);
    rbrew_write16(mem, base + 4, touched);
    rbrew_write16(mem, base + 6, validity);
}
static void write_float(uint8_t* mem, uint32_t address, float value) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    rbrew_write32(mem, address, bits);
}

// GamePad touch panel: raw 12-bit samples, calibrated to 1280x720 by
// VPADGetTPCalibratedPoint (same default calibration Cemu applies).
static constexpr float kTpOffsetX = 92.0f, kTpOffsetY = 254.0f, kTpRangeX = 3883.0f, kTpRangeY = 3694.0f;
uint16_t vpad_touch_raw_x(uint16_t screen_x) {
    return uint16_t(std::lround(screen_x * kTpRangeX / 854.0f + kTpOffsetX));
}
uint16_t vpad_touch_raw_y(uint16_t screen_y) {
    return uint16_t(std::lround(4095.0f - kTpOffsetY - screen_y * kTpRangeY / 480.0f));
}

// shape_stick defined inline in game_controllers.h
static uint32_t stick_emulation(float x, float y, uint32_t left, uint32_t right, uint32_t up, uint32_t down) {
    constexpr float kThreshold = 0.5f;
    uint32_t bits = 0;
    if (x <= -kThreshold) bits |= left;
    if (x >= kThreshold) bits |= right;
    if (y >= kThreshold) bits |= up;
    if (y <= -kThreshold) bits |= down;
    return bits;
}

// GamePad motion driven by the right stick. A PC controller has no GamePad
// gyro, yet some sections (the first tutorial's "Move GamePad to aim") aim
// with the motion sensors whatever the Motion Controls option says. The
// stick turns a virtual GamePad: yaw about the world up axis, pitch about
// the pad's X axis (Cemu's mouse-motion convention: R = Ry(yaw)·Rx(pitch),
// dir.x/y/z = R's columns, negative pitch tilts up). Angular velocity
// (revolutions/s, device axes), integrated angle and gravity stay consistent
// with that orientation. While the option is off the game aims with the
// stick and does not read these values.
struct Mat3 {
    float m[3][3];
}; // m[row][column]
static Mat3 mat_mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
static Mat3 rotation_x(float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    return {{{1, 0, 0}, {0, c, -s}, {0, s, c}}};
}
static Mat3 rotation_y(float radians) {
    const float c = std::cos(radians), s = std::sin(radians);
    return {{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}};
}
struct VirtualMotion {
    Mat3 base{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};  // orientation at the last VPADSetGyroDirection
    float yaw = 0, pitch = 0;                        // degrees from base
    float angle[3] = {};                             // revolutions, device axes
    float mouse_acc = 0;                             // degrees of yaw that came from the mouse
    uint64_t last_ns = 0;
};
static std::mutex s_motion_lock;
static VirtualMotion s_motion;
// RBREW_MOUSE_VARIANT (diagnostic): how the mouse yaw reaches the game.
// 0 gyro rate + direction + angle, 1 not the rate, 2 not the direction,
// 3 not the angle, 4 gyro and a virtual right-stick push (default), 5 stick push only (yaw).
static int mouse_variant() {
    static const int value = [] { const char* p = std::getenv("RBREW_MOUSE_VARIANT"); return p && *p >= '0' && *p <= '5' ? *p - '0' : 4; }();
    return value;
}
// Degrees per second of mouse turn that fill the right stick (variants 4 and 5).
static float mouse_stick_rate() {
    static const float value = [] {
        const char* p = std::getenv("RBREW_MOUSE_STICK_RATE");
        const float f = p && *p ? std::strtof(p, nullptr) : 300.0f;
        return f >= 30.0f && f <= 3000.0f ? f : 300.0f;
    }();
    return value;
}
// RBREW_GYRO_TRACE=<file> (diagnostic, off by default): direction resets and,
// every 30 updates, the virtual pad's pitch/yaw, its inputs and the gravity.
static FILE* gyro_trace() {
    static FILE* const file = [] { const char* p = std::getenv("RBREW_GYRO_TRACE"); return p && *p ? std::fopen(p, "w") : nullptr; }();
    return file;
}
// Pitch the virtual pad rests at after VPADSetGyroDirection (degrees, negative
// tilts the top back), as a GamePad is held. The game resets the direction to
// identity at every load; resting flat there, the camera spawned looking at
// the ground. Measured on the plaza spawn: 0 = top-down view, -24.5 = behind
// the player as on the console. RBREW_GYRO_REST_PITCH overrides it.
static float rest_pitch() {
    static const float value = [] {
        constexpr float kDefault = -24.5f;
        const char* v = std::getenv("RBREW_GYRO_REST_PITCH");
        if (!v || !*v) return kDefault;
        char* end = nullptr;
        const float f = std::strtof(v, &end);
        return end && !*end && f >= -80.0f && f <= 80.0f ? f : kDefault;
    }();
    return value;
}
static constexpr float kPi = 3.14159265358979f;
static constexpr float kYawRate = 240.0f, kPitchRate = 150.0f, kPitchLimit = 80.0f; // degrees(/s)
// Mouse aiming like a PC game ("Mouse aim: Direct"): each frame's mouse
// movement in degrees turns the virtual GamePad, which the game's motion
// aiming follows 1:1 - no stick speed limit, no dead zone.
static float s_mouse_yaw = 0.0f, s_mouse_pitch = 0.0f; // degrees this frame (under s_motion_lock)
static constexpr float kMouseDegreesPerCount = 0.035f; // 0.09 was far too fast

// RBREW_GYRO=0 (or "Controller gyro: Off" in the ESC menu) ignores a
// controller's gyro sensor (stick/mouse only).
static bool physical_gyro_enabled() {
    return rbrew_settings::g_gyro.load(std::memory_order_relaxed);
}

// Advances the virtual GamePad by dt seconds and writes acc/gyro/angle/
// direction into the VPADStatus at buf. The right stick/mouse always turns
// it; a controller gyro (SDL, rad/s: x = pitch, y = yaw) adds its rotation.
// Previously any gyro reading replaced the stick entirely, so with a
// DualShock/DualSense the tutorial camera ignored the right stick and the
// reported acceleration no longer matched the reported orientation.
void vpad_update_motion(uint8_t* mem, uint32_t buf, float rx, float ry, float dt,
                        const float* phys_gyro = nullptr, const float* /*phys_accel*/ = nullptr,
                        bool tutorial = false, float mouse_yaw = 0.0f, float mouse_pitch = 0.0f) {
    std::lock_guard lock(s_motion_lock);
    VirtualMotion& v = s_motion;
    // Squared response keeps small deflections precise. Outside the tutorial
    // the game already turns the camera with stick X itself; also yawing the
    // virtual pad turned it twice as fast with motion controls on. The
    // tutorial locks stick aiming and aims only with the pad, so there stick
    // X (and the mouse) yaw the pad, otherwise the camera only moves up and
    // down. Stick Y: see below.
    // RBREW_STICK_GYRO_YAW=1 yaws everywhere, =0 never (unset: tutorial only).
    static const int stick_yaw_mode = [] {   // 0 never, 1 always, 2 tutorial
        const char* v = std::getenv("RBREW_STICK_GYRO_YAW");
        return v && v[0] == '1' ? 1 : v && v[0] == '0' ? 0 : 2;
    }();
    const bool stick_yaw = stick_yaw_mode == 1 || (stick_yaw_mode == 2 && tutorial);
    // With a real gyro the stick no longer tilts the pad either: on the
    // console, motion controls aim vertically only by moving the GamePad, but
    // stick Y pitched the virtual pad here, so the camera went up and down
    // by stick with motion controls on. Without a gyro (keyboard/mouse, pads
    // without sensors) stick Y still tilts it, or the tutorial's forced
    // motion aiming could not look up. RBREW_STICK_GYRO_PITCH=1 keeps stick
    // pitch with a gyro.
    static const bool stick_pitch_with_gyro = [] {
        const char* v = std::getenv("RBREW_STICK_GYRO_PITCH");
        return v && v[0] == '1';
    }();
    const bool use_gyro = phys_gyro && physical_gyro_enabled();
    float yaw_rate = stick_yaw ? -rx * std::fabs(rx) * kYawRate : 0.0f;
    float pitch_rate = use_gyro && !stick_pitch_with_gyro ? 0.0f : -ry * std::fabs(ry) * kPitchRate;
    if (dt > 0 && (s_mouse_yaw != 0.0f || s_mouse_pitch != 0.0f)) {
        yaw_rate += s_mouse_yaw / dt;
        pitch_rate += s_mouse_pitch / dt;
        s_mouse_yaw = s_mouse_pitch = 0.0f;
    }
    if (use_gyro) {
        constexpr float kGyroDeadzone = 0.02f; // rad/s: resting sensor noise/drift
        const float scale = rbrew_settings::g_gyro_scale.load(std::memory_order_relaxed); // ESC menu sensitivity
        auto rate = [&](float radians) {
            return std::fabs(radians) < kGyroDeadzone ? 0.0f : radians * (180.0f / kPi) * scale;
        };
        // SDL: +x tilts the far edge up (aim up), which is negative pitch
        // here; +y turns left, which is positive yaw. Adding +x inverted
        // vertical aiming. RBREW_GYRO_INVERT_PITCH=1 / _YAW=1 flip an axis.
        const bool invert_pitch = rbrew_settings::g_gyro_invert_pitch.load(std::memory_order_relaxed);
        static const bool invert_yaw = [] {
            const char* v = std::getenv("RBREW_GYRO_INVERT_YAW");
            return v && v[0] == '1';
        }();
        pitch_rate += invert_pitch ? rate(phys_gyro[0]) : -rate(phys_gyro[0]);
        yaw_rate += invert_yaw ? -rate(phys_gyro[1]) : rate(phys_gyro[1]);
    }
    // Mouse: an angle this step (degrees), reported to the game as rate too.
    if (mouse_variant() == 5) mouse_yaw = 0.0f;   // yaw goes through the stick only
    if (dt > 0) yaw_rate += mouse_yaw / dt;
    else v.yaw += mouse_yaw;
    v.yaw = std::fmod(v.yaw + yaw_rate * dt, 360.0f);
    v.mouse_acc += mouse_yaw;
    const int variant = mouse_variant();
    const float mouse_rate = dt > 0 ? mouse_yaw / dt : 0.0f;
    const float report_rate = yaw_rate - (variant == 1 ? mouse_rate : 0.0f);
    const float dir_yaw = v.yaw - (variant == 2 ? v.mouse_acc : 0.0f);
    // RBREW_MOUSE_TRACE=<file> (diagnostic, off by default): one line per read that carries mouse motion.
    static FILE* const mouse_trace = [] { const char* p = std::getenv("RBREW_MOUSE_TRACE"); return p && *p ? std::fopen(p, "w") : nullptr; }();
    if (mouse_trace && (mouse_yaw != 0.0f || mouse_pitch != 0.0f)) {
        std::fprintf(mouse_trace, "dt=%.4f mouse_yaw=%.3f mouse_pitch=%.3f yaw_rate=%.1f yaw=%.2f tutorial=%d\n", dt, mouse_yaw, mouse_pitch, yaw_rate, v.yaw, int(tutorial));
        std::fflush(mouse_trace);
    }
    const float pitch = std::fmax(-kPitchLimit, std::fmin(kPitchLimit, v.pitch + pitch_rate * dt + mouse_pitch));
    const float applied_pitch_rate = dt > 0 ? (pitch - v.pitch) / dt : 0.0f;
    v.pitch = pitch;
    const Mat3 r = mat_mul(mat_mul(rotation_y(dir_yaw * kPi / 180), v.base), rotation_x(v.pitch * kPi / 180));
    // World angular velocity: yaw about world Y plus pitch about the pad's X axis.
    const Mat3 yawed = mat_mul(rotation_y(dir_yaw * kPi / 180), v.base);
    float world[3];
    for (int i = 0; i < 3; ++i) world[i] = yawed.m[i][0] * applied_pitch_rate + (i == 1 ? report_rate : 0.0f);
    float gyro[3], gravity[3];
    for (int axis = 0; axis < 3; ++axis) {
        gyro[axis] = (r.m[0][axis] * world[0] + r.m[1][axis] * world[1] + r.m[2][axis] * world[2]) / 360.0f;
        gravity[axis] = -r.m[1][axis];  // R^T (0, -1, 0)
        v.angle[axis] += (gyro[axis] - (variant == 3 ? r.m[1][axis] * mouse_rate / 360.0f : 0.0f)) * dt;
    }
    for (int axis = 0; axis < 3; ++axis) {
        write_float(mem, buf + 0x1C + 4 * axis, gravity[axis]);
        write_float(mem, buf + 0x38 + 4 * axis, gyro[axis]);
        write_float(mem, buf + 0x44 + 4 * axis, v.angle[axis]);
        for (int component = 0; component < 3; ++component) // dir.x/y/z = columns of R
            write_float(mem, buf + kVpadDirection + 12 * axis + 4 * component, r.m[component][axis]);
    }
    write_float(mem, buf + 0x28, 1.0f); // accMagnitude: gravity only
    write_float(mem, buf + kVpadAccXY, 1.0f);
}
static float motion_seconds() {
    const uint64_t now = uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    std::lock_guard lock(s_motion_lock);
    const uint64_t last = std::exchange(s_motion.last_ns, now);
    return last ? std::fmin(0.1f, float(now - last) * 1e-9f) : 0.0f;
}
// VPADSetGyroDirection(chan, const VPADDirection* dir): the pad now faces dir.
static void VPADSetGyroDirection(CPUState* cpu) {
    const uint32_t dir = ARG1;
    if (!dir || !rbrew_memory_accessible(MEM, dir, 36)) return;
    std::lock_guard lock(s_motion_lock);
    for (int axis = 0; axis < 3; ++axis)
        for (int component = 0; component < 3; ++component)
            s_motion.base.m[component][axis] = float(rbrew_read_f32(MEM, dir + 12 * axis + 4 * component));
    if (FILE* f = gyro_trace()) {
        const auto& m = s_motion.base.m;
        std::fprintf(f, "SetGyroDirection lr=%08X after pitch=%.1f yaw=%.1f  x(%.2f %.2f %.2f) y(%.2f %.2f %.2f) z(%.2f %.2f %.2f)\n",
                     cpu->lr, s_motion.pitch, s_motion.yaw, m[0][0], m[1][0], m[2][0], m[0][1], m[1][1], m[2][1], m[0][2], m[1][2], m[2][2]);
        std::fflush(f);
    }
    s_motion.yaw = 0;
    s_motion.pitch = rest_pitch();
}

// Edge state for VPADStatus trigger/release (the console reports presses and
// releases once; reporting the whole hold as "trigger" repeats every press).
static std::atomic<uint32_t> s_previous_hold{0};
uint32_t vpad_update_edges(uint32_t hold, uint32_t& trigger, uint32_t& release) {
    const uint32_t previous = s_previous_hold.exchange(hold);
    trigger = hold & ~previous;
    release = previous & ~hold;
    return previous;
}

// Controller acquisition is handled through get_controller(0) for Player 1

// ---------------------------------------------------------------------------
// VPADRead(chan, buffers, count, error) → samplesRead
// ---------------------------------------------------------------------------
// RBREW_CAM_SCAN=<file> (diagnostic, off by default): an in-process value scanner for
// finding the camera angles, in the Cheat Engine manner. Keys (game window):
//   F5 start: remember all of guest memory [0x10000000, 0x40000000)
//   F6 keep the floats that changed since the last step   F7 keep those that did not
//   F8 keep those that increased                          F9 keep those that decreased
//   F10 write the survivors (address, last value) to <file>
// A step that would leave nothing is undone. Counts go to stderr as [cam-scan].
void rbrew_write_probe_arm(uint8_t* mem, uint32_t guest);   // write_probe.cpp
void rbrew_write_probe_report();
namespace cam_scan {
constexpr uint32_t kBase = 0x10000000u, kWords = 0x30000000u / 4;
static std::vector<uint32_t> g_full;                  // raw big-endian words from F5, until the first filter
static std::vector<uint32_t> g_idx, g_prev_idx;       // surviving word indices
static std::vector<double> g_old, g_prev_old;         // their value at the last step
// RBREW_CAM_SCAN_INT=1: read the words as signed 32-bit integers (fixed-point angles) instead of floats.
static const bool g_int = [] { const char* p = std::getenv("RBREW_CAM_SCAN_INT"); return p && *p == '1'; }();
static double from_raw(uint32_t be) {
#if defined(_MSC_VER)
    const uint32_t w = _byteswap_ulong(be);
#else
    const uint32_t w = __builtin_bswap32(be);
#endif
    if (g_int) return double(int32_t(w));
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}
static double load(const uint8_t* mem, uint32_t i) {
    uint32_t be;
    std::memcpy(&be, mem + kBase + 4u * i, 4);
    return from_raw(be);
}
static bool plausible(double f) { return g_int ? true : (f == f && std::fabs(f) < 1.0e4); }
// mode: 0 changed, 1 unchanged, 2 increased, 3 decreased
static bool keep(double now, double was, int mode) {
    const double eps = g_int ? 0.5 : 1e-6;
    switch (mode) {
    case 0: return std::fabs(now - was) > eps;
    case 1: return now == was;
    case 2: return now > was + eps;
    default: return now < was - eps;
    }
}
static void step(uint8_t* mem, int mode) {
    if (!g_full.empty()) {
        if (mode == 1) { std::fprintf(stderr, "[cam-scan] nothing to keep unchanged yet: do F6/F8/F9 first\n"); return; }
        std::vector<uint32_t> idx; std::vector<double> old;
        for (uint32_t i = 0; i < kWords; ++i) {
            uint32_t w; std::memcpy(&w, mem + kBase + 4u * i, 4);
            if (w == g_full[i]) continue;
            const double now = load(mem, i);
            const double was = from_raw(g_full[i]);
            if (plausible(now) && plausible(was) && keep(now, was, mode)) { idx.push_back(i); old.push_back(now); }
        }
        if (idx.empty()) { std::fprintf(stderr, "[cam-scan] empty, step ignored\n"); return; }
        g_idx.swap(idx); g_old.swap(old);
        std::vector<uint32_t>().swap(g_full);
        std::fprintf(stderr, "[cam-scan] %zu candidates\n", g_idx.size());
        return;
    }
    if (g_idx.empty()) { std::fprintf(stderr, "[cam-scan] press F5 first\n"); return; }
    std::vector<uint32_t> idx; std::vector<double> old;
    for (size_t k = 0; k < g_idx.size(); ++k) {
        const double now = load(mem, g_idx[k]);
        if (plausible(now) && keep(now, g_old[k], mode)) { idx.push_back(g_idx[k]); old.push_back(now); }
    }
    if (idx.empty()) { std::fprintf(stderr, "[cam-scan] step would leave nothing, undone (%zu candidates)\n", g_idx.size()); return; }
    g_prev_idx = g_idx; g_prev_old = g_old;
    g_idx.swap(idx); g_old.swap(old);
    std::fprintf(stderr, "[cam-scan] %zu candidates\n", g_idx.size());
}
static void dump(uint8_t* mem, const char* path) {
    if (FILE* f = std::fopen(path, "w")) {
        std::fprintf(f, "%zu candidates\n", g_idx.size());
        for (size_t k = 0; k < g_idx.size() && k < 400000; ++k)
            std::fprintf(f, "%08X %.6f now %.6f\n", kBase + 4u * g_idx[k], g_old[k], load(mem, g_idx[k]));
        std::fclose(f);
        std::fprintf(stderr, "[cam-scan] %zu candidates written to %s\n", g_idx.size(), path);
    }
}
static void start(uint8_t* mem) {
    g_idx.clear(); g_old.clear();
    g_full.resize(kWords);
    std::memcpy(g_full.data(), mem + kBase, 4u * size_t(kWords));
    std::fprintf(stderr, "[cam-scan] memory remembered\n");
}
// RBREW_CAM_OPS=<file>: lines "<frame> <op>" (op S start, C changed, U unchanged, I increased,
// D decreased, X dump), run once the presented-frame counter reaches <frame>. The file is
// reread every 30 reads. Also logs scene changes ([scene]) and the frame every 300 frames.
static void auto_ops(uint8_t* mem, const char* dump_path) {
    static const char* const ops_path = std::getenv("RBREW_CAM_OPS");
    if (!ops_path || !*ops_path) return;
    static uint32_t polls = 0, last_done = 0;
    static size_t done_idx = 0;
    static bool any_done = false;
    struct Op { uint32_t frame; char op; uint32_t addr; float value; };
    static std::vector<Op> ops;
    static int last_scene = -2, last_id = -2;
    const uint32_t frame = rbrew_benchmark::frame.load(std::memory_order_relaxed);
    int id = -1;
    const int scene = rbrew_control_defaults::active_scene_type(mem, &id);
    if (scene != last_scene || id != last_id) {
        std::fprintf(stderr, "[scene] frame=%u kind=%d id=%d\n", frame, scene, id);
        last_scene = scene; last_id = id;
    }
    if (polls++ % 30 == 0) {
        std::fprintf(stderr, "[frame] %u\n", frame);
        std::ifstream in(ops_path);
        ops.clear();
        uint32_t f; char c;
        while (in >> f >> c) {
            Op op{f, c, 0, 0.0f};
            if (c == 'W' || c == 'R' || c == 'P') in >> std::hex >> op.addr >> std::dec;
            if (c == 'W') in >> op.value;
            ops.push_back(op);
        }
    }
    size_t index = 0;
    for (const auto& o : ops) {
        ++index;
        const uint32_t f = o.frame; const char c = o.op;
        if (f > frame) continue;
        if (any_done && (f < last_done || (f == last_done && index <= done_idx))) continue;
        last_done = f; done_idx = index; any_done = true;
        std::fprintf(stderr, "[cam-scan] frame %u op %c\n", frame, c);
        switch (c) {
        case 'S': start(mem); break;
        case 'C': step(mem, 0); break;
        case 'U': step(mem, 1); break;
        case 'I': step(mem, 2); break;
        case 'D': step(mem, 3); break;
        case 'X': dump(mem, dump_path); break;
        case 'P': rbrew_write_probe_arm(mem, o.addr); break;
        case 'Q': rbrew_write_probe_report(); break;
        case 'W': if (g_int) rbrew_write32(mem, o.addr, uint32_t(int32_t(o.value))); else rbrew_write_f32(mem, o.addr, o.value); std::fprintf(stderr, "[cam-scan] poke %08X = %f\n", o.addr, o.value); break;
        case 'R': std::fprintf(stderr, "[cam-scan] read %08X = %f at frame %u\n", o.addr, g_int ? double(int32_t(rbrew_read32(mem, o.addr))) : double(float(rbrew_read_f32(mem, o.addr))), frame); break;
        }
    }
}
static void poll(uint8_t* mem) {
#if defined(HAVE_SDL2)
    static const char* const path = std::getenv("RBREW_CAM_SCAN");
    if (!path || !*path || !mem) return;
    auto_ops(mem, path);
    static bool down[5] = {};
    static const SDL_Scancode keys[5] = {SDL_SCANCODE_F5, SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F8, SDL_SCANCODE_F9};
    const uint8_t* state = SDL_GetKeyboardState(nullptr);
    for (int k = 0; k < 5; ++k) {
        const bool now = state[keys[k]] != 0;
        if (now && !down[k]) {
            if (k == 0) start(mem); else step(mem, k - 1);
        }
        down[k] = now;
    }
    static bool dump_down = false;
    const bool d = state[SDL_SCANCODE_F10] != 0;
    if (d && !dump_down) dump(mem, path);
    dump_down = d;
#else
    (void)mem;
#endif
}
}  // namespace cam_scan

// RBREW_CAM_SNAP=<dir> (diagnostic, off by default): F9 writes the guest
// memory [0x10000000, 0x40000000) to <dir>\\snap-<n>.bin, to find the camera
// fields by differencing two snapshots taken around a camera rotation.
static void cam_snapshot(uint8_t* mem) {
#if defined(HAVE_SDL2)
    static const char* const dir = std::getenv("RBREW_CAM_SNAP");
    if (!dir || !*dir || !mem) return;
    static bool was_down = false;
    static int count = 0;
    const bool down = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_F9] != 0;
    if (down && !was_down) {
        char path[512];
        std::snprintf(path, sizeof path, "%s\\snap-%d.bin", dir, count++);
        if (FILE* f = std::fopen(path, "wb")) {
            std::fwrite(mem + 0x10000000u, 1, 0x30000000u, f);
            std::fclose(f);
            std::fprintf(stderr, "[cam-snap] %s\n", path);
        }
    }
    was_down = down;
#else
    (void)mem;
#endif
}

static void VPADRead(CPUState* cpu) {
    // ARG0 = channel (0=GamePad), ARG1 = VPADStatus* buf, ARG2 = count, ARG3 = error*
    uint32_t buf = ARG1;
    uint32_t count = ARG2;
    uint32_t errp = ARG3;

    if (errp) rbrew_write32(MEM, errp, 0u); // VPAD_READ_SUCCESS
    vpad_apply_motion_policy(MEM);
    cam_snapshot(MEM);
    cam_scan::poll(MEM);

    if (!buf || count == 0 || !rbrew_memory_accessible(MEM, buf, kVpadStatusSize)) {
        RET = 0;
        return;
    }

    static const uint8_t kZero[kVpadStatusSize] = {};
    rbrew_write_memory(MEM, buf, kZero, kVpadStatusSize);

    uint32_t buttons = 0;
    float lx = 0.0f, ly = 0.0f; // left stick (movement), y > 0 = up
    float rx = 0.0f, ry = 0.0f; // right stick (camera)

    float phys_gyro[3] = {};
    float phys_accel[3] = {};
    float motion_dt = -1.0f;   // < 0: measure it here
    bool player_input = false; // a controller button/stick or key (ends a replay)
    bool has_phys_gyro = false, has_phys_accel = false;

#if defined(HAVE_SDL2)
    ControllerDevice* dev = get_controller(0); // Player 1 (GamePad)
    if (dev && dev->gc) {
        SDL_GameController* gc = dev->gc;
        if (dev->has_gyro) {
            if (SDL_GameControllerGetSensorData(gc, SDL_SENSOR_GYRO, phys_gyro, 3) == 0) {
                has_phys_gyro = true;
            }
        }
        if (dev->has_accel) {
            if (SDL_GameControllerGetSensorData(gc, SDL_SENSOR_ACCEL, phys_accel, 3) == 0) {
                has_phys_accel = true;
            }
        }
        // Bindings from the ESC menu's controller page (port_settings.h). The
        // defaults map SDL's A/B/X/Y positions (south/east/west/north) like
        // the GamePad: B south, A east, Y west, X north; triggers are ZL/ZR
        // (digital triggers report 32767).
        auto pressed = [&](int binding) {
            if (binding == rbrew_settings::kPadTriggerLeft)
                return SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000;
            if (binding == rbrew_settings::kPadTriggerRight)
                return SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000;
            return binding >= 0 && SDL_GameControllerGetButton(gc, SDL_GameControllerButton(binding)) != 0;
        };
        for (int action = 0; action < rbrew_settings::kFirstMoveAction; ++action)
            if (pressed(rbrew_settings::g_pad[action].load(std::memory_order_relaxed)))
                buttons |= kActionButtons[action];

        // SDL axes: [-32768, 32767] with y > 0 = down; the GamePad reports y > 0 = up.
        lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
        ly = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
        rx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
        ry = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
        shape_stick(lx, ly);
        shape_stick(rx, ry);
        player_input = buttons != 0 || std::fabs(lx) > 0.5f || std::fabs(ly) > 0.5f || std::fabs(rx) > 0.5f ||
                       std::fabs(ry) > 0.5f;
    }

    const int scene_type = rbrew_control_defaults::active_scene_type(MEM);   // 5 TutForShow, 6 Tutorial
    const bool tutorial = scene_type == 5 || scene_type == 6;
    const bool motion_aim = tutorial || rbrew_control_defaults::motion_controls(MEM) == 1;
    float mouse_yaw = 0.0f, mouse_pitch = 0.0f;   // degrees this read (mouse via the virtual pad)

    // -----------------------------------------------------------------------
    // Keyboard + mouse input (Splatoon layout, active in TV view)
    // KB+M additively overlays controller: pressing a key overrides that axis.
    // -----------------------------------------------------------------------
    if (g_screen_mode == ScreenMode::TV) {
        const uint8_t* keys = SDL_GetKeyboardState(nullptr);

        // Bindings from the ESC menu's keyboard page (port_settings.h):
        // scancodes, or kMouseBase + an SDL mouse button.
        int mx_dummy, my_dummy;
        const uint32_t mstate = (uint32_t)SDL_GetMouseState(&mx_dummy, &my_dummy);
        bool key_held = false; // keys (not mouse buttons) end a replay
        auto bound = [&](int action) {
            const int binding = rbrew_settings::g_keys[action].load(std::memory_order_relaxed);
            if (binding >= rbrew_settings::kMouseBase)
                return (mstate & SDL_BUTTON(binding - rbrew_settings::kMouseBase)) != 0;
            const bool down = binding >= 0 && binding < SDL_NUM_SCANCODES && keys[binding];
            key_held = key_held || down;
            return down;
        };

        // WASD → left stick movement
        float kb_lx = 0.0f, kb_ly = 0.0f;
        if (bound(rbrew_settings::kActMoveRight)) kb_lx += 1.0f;
        if (bound(rbrew_settings::kActMoveLeft)) kb_lx -= 1.0f;
        if (bound(rbrew_settings::kActMoveUp)) kb_ly += 1.0f; // +Y = up on the GamePad stick
        if (bound(rbrew_settings::kActMoveDown)) kb_ly -= 1.0f;
        if (kb_lx != 0.0f && kb_ly != 0.0f) {
            kb_lx *= 0.7071f;
            kb_ly *= 0.7071f;
        } // diag norm
        if (kb_lx != 0.0f || kb_ly != 0.0f) {
            lx = kb_lx;
            ly = kb_ly;
        } // override controller

        // Mouse relative motion → camera / aim; moving the mouse up looks up.
        // With motion aiming (the tutorial, or the game's motion controls on)
        // it turns the virtual GamePad by a fixed angle per count; otherwise
        // it is added to the right stick. A controller's own stick and gyro
        // are unchanged either way.
        int mdx = 0, mdy = 0;
        SDL_GetRelativeMouseState(&mdx, &mdy);
        // Free fly camera (cinema / hacks page, F7): mouse looks, WASD
        // moves, Space up, Ctrl down, Shift fast; the game gets a neutral pad.
        if (gx2env::fly_camera()) {
            const bool ctrl = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];
            // lx/ly: WASD or the controller's left stick; the right stick looks too.
            gx2env::fly_input(mdx + int(rx * 14.f), mdy - int(ry * 14.f), ly, lx,
                              (keys[SDL_SCANCODE_SPACE] ? 1.f : 0.f) - (ctrl ? 1.f : 0.f),
                              keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]);
            mdx = mdy = 0;
        }
        const float mouse_scale = rbrew_settings::g_mouse_scale.load(std::memory_order_relaxed);   // ESC menu
        // rx/ry still hold only the controller's stick here.
        if (mdx || mdy) s_mouse_aims = true;
        else if (std::fabs(rx) > 0.25f || std::fabs(ry) > 0.25f) s_mouse_aims = false;
        if (rbrew_settings::g_mouse_direct.load(std::memory_order_relaxed)) {
            // PC aiming (ESC menu "Mouse aim: Direct"): straight to the motion aiming, not the stick.
            std::lock_guard lock(s_motion_lock);
            s_mouse_yaw += -float(mdx) * kMouseDegreesPerCount * mouse_scale;
            // Negative pitch looks up: mouse up (mdy < 0) must look up.
            s_mouse_pitch += float(mdy) * kMouseDegreesPerCount * mouse_scale;
        } else if (motion_aim) {
            mouse_yaw = -(float)mdx * KBM_MOUSE_DEGREES * mouse_scale;
            mouse_pitch = (float)mdy * KBM_MOUSE_DEGREES * mouse_scale;   // negative pitch looks up
            if (mouse_variant() >= 4) {   // also push the right stick, as if the mouse were a stick (60 reads/s)
                rx += -mouse_yaw * 60.0f / mouse_stick_rate();
                if (rx < -1.0f) rx = -1.0f; if (rx > 1.0f) rx = 1.0f;
            }
        } else {
            float mrx = (float)mdx * KBM_MOUSE_SENSITIVITY * mouse_scale;
            float mry = -(float)mdy * KBM_MOUSE_SENSITIVITY * mouse_scale;
            if (mrx < -1.0f) mrx = -1.0f; if (mrx > 1.0f) mrx = 1.0f;
            if (mry < -1.0f) mry = -1.0f; if (mry > 1.0f) mry = 1.0f;
            // Blend: mouse delta is added to controller stick, clamped
            rx += mrx; if (rx < -1.0f) rx = -1.0f; if (rx > 1.0f) rx = 1.0f;
            ry += mry; if (ry < -1.0f) ry = -1.0f; if (ry > 1.0f) ry = 1.0f;
        }

        // Buttons (defaults: left click ZR = fire, right click ZL = squid,
        // Return A, Space B = jump, Q X, E Y, R, F + = map, G - = options,
        // H HOME; Esc opens the port's settings).
        for (int action = 0; action < rbrew_settings::kFirstMoveAction; ++action)
            if (bound(action)) buttons |= kActionButtons[action];
        player_input = player_input || key_held;
    }

    static const bool s_auto_input = (std::getenv("RBREW_AUTO_INPUT") != nullptr);
    bool auto_touched = false;
    uint16_t auto_tx = 0, auto_ty = 0;
    if (s_auto_input) {
        static uint32_t s_auto_frame = 0;
        ++s_auto_frame;
        // Stage 1 (0..300): Title screen - press ZL + ZR
        if (s_auto_frame < 300) {
            if ((s_auto_frame % 30) < 15) {
                buttons |= (VPAD_BUTTON_ZR | VPAD_BUTTON_ZL);
            }
        }
        // Stage 2 (300..600): "Choose one!" - highlight Girl and select
        else if (s_auto_frame < 600) {
            if ((s_auto_frame % 40) < 15) {
                buttons |= VPAD_BUTTON_LEFT | VPAD_STICK_L_EMULATION_LEFT;
                lx = -1.0f;
            } else if ((s_auto_frame % 40) >= 20 && (s_auto_frame % 40) < 35) {
                buttons |= VPAD_BUTTON_A;
            }
        }
        // Stage 3 (600..900): Character customization - press PLUS / START to proceed
        else if (s_auto_frame < 900) {
            if ((s_auto_frame % 40) < 15) {
                buttons |= VPAD_BUTTON_PLUS;
            } else if ((s_auto_frame % 40) >= 20 && (s_auto_frame % 40) < 35) {
                buttons |= VPAD_BUTTON_A;
            }
        }
        // Stage 4 (900..1300): Confirmation dialog "Happy with this look?" - select Yes and confirm
        else if (s_auto_frame < 1300) {
            if ((s_auto_frame % 40) < 15) {
                buttons |= VPAD_BUTTON_LEFT | VPAD_STICK_L_EMULATION_LEFT;
                lx = -1.0f;
            } else if ((s_auto_frame % 40) >= 20 && (s_auto_frame % 40) < 35) {
                buttons |= VPAD_BUTTON_A;
            }
        }
        // Stage 5 (1300+): Tutorial active gameplay (moving, shooting ink, camera rotation, squid swim)
        else {
            ly = 1.0f;                                         // Walk forward
            rx = std::sin(float(s_auto_frame) * 0.05f) * 0.8f; // Aim / camera pan
            ry = std::cos(float(s_auto_frame) * 0.03f) * 0.3f; // Pitch

            uint32_t act = (s_auto_frame / 90) % 4;
            if (act == 0) {
                buttons |= VPAD_BUTTON_ZR; // Continuous ink shooting
            } else if (act == 1) {
                buttons |= VPAD_BUTTON_ZL; // Move through ink in squid form
            } else if (act == 2) {
                buttons |= VPAD_BUTTON_B | VPAD_BUTTON_ZR; // Jump and shoot
            } else {
                buttons |= VPAD_BUTTON_R; // Throw sub-weapon
            }
        }
    }

    // ESC menu open (or its closing press still held): the game sees a
    // neutral pad. Mouse motion accumulated meanwhile is dropped too.
    {
        // Only buttons keep the block after closing (a drifting stick kept it).
        const bool held = buttons != 0;
        if (gx2_imgui::menu_blocks_input(held) || gx2env::fly_camera()) {
            buttons = 0;
            lx = ly = rx = ry = 0.0f;
            has_phys_gyro = has_phys_accel = false;
            mouse_yaw = mouse_pitch = 0.0f;
            player_input = false;
        }
    }

    rbrew_benchmark::Input scripted;
    if (rbrew_benchmark::scripted_input(scripted)) {
        buttons = scripted.buttons;
        lx = scripted.lx;
        ly = scripted.ly;
        rx = scripted.rx;
        ry = scripted.ry;
        has_phys_gyro = has_phys_accel = false;
        mouse_yaw = mouse_pitch = 0.0f;
        auto_touched = scripted.touched != 0;
        auto_tx = vpad_touch_raw_x(uint16_t(scripted.x));
        auto_ty = vpad_touch_raw_y(uint16_t(scripted.y));
        static const bool actions = std::getenv("RBREW_BENCHMARK_ACTIONS") != nullptr;
        if (actions && rbrew_control_defaults::active_scene_type(MEM) == 6) {
            static const uint32_t first = rbrew_benchmark::frame.load();
            const uint32_t age = rbrew_benchmark::frame.load() - first;
            // Same motion/fire path in every candidate, keyed to game frames.
            lx = 0;
            ly = 0.6f;
            rx = 0.35f * std::sin(float(age) * 0.01f);
            ry = 0.12f * std::sin(float(age) * 0.007f);
            buttons = (age / 120) % 3 == 1 ? VPAD_BUTTON_ZL : VPAD_BUTTON_ZR;
            if ((age / 60) % 4 == 3) buttons |= VPAD_BUTTON_B;
            auto_touched = false;
        }
    }

    // Session recording/replay (RBREW_INPUT_RECORD / RBREW_INPUT_REPLAY):
    // the frame's whole pad, gyro and motion time step included.
    bool live_touch = true;
    if (rbrew_benchmark::recording() || rbrew_benchmark::replaying()) {
        rbrew_benchmark::Pad pad;
        pad.buttons = buttons;
        pad.lx = lx;
        pad.ly = ly;
        pad.rx = rx;
        pad.ry = ry;
        pad.touched = g_touch_state.touched ? 1 : 0;
        pad.x = g_touch_state.touched ? g_touch_state.x : 0;
        pad.y = g_touch_state.touched ? g_touch_state.y : 0;
        if (has_phys_gyro) std::copy_n(phys_gyro, 3, pad.gyro);
        pad.dt = motion_seconds();
        int scene_id = -1;
        const int scene = rbrew_control_defaults::active_scene_type(MEM, &scene_id);
        // Mouse movement and clicks do not end a replay; controller and keys do.
        const bool replayed = rbrew_benchmark::replay_input(pad, scene, scene_id, player_input);
        if (!replayed) rbrew_benchmark::record_input(pad, scene, scene_id);
        motion_dt = pad.dt;
        if (replayed || rbrew_benchmark::recording()) {
            buttons = pad.buttons;
            lx = pad.lx;
            ly = pad.ly;
            rx = pad.rx;
            ry = pad.ry;
            std::copy_n(pad.gyro, 3, phys_gyro);
            has_phys_gyro = has_phys_gyro || replayed;
            has_phys_accel = false;
            if (replayed) mouse_yaw = mouse_pitch = 0.0f;   // recording only: the live mouse stays
            live_touch = false;
            auto_touched = pad.touched != 0;
            auto_tx = vpad_touch_raw_x(uint16_t(pad.x));
            auto_ty = vpad_touch_raw_y(uint16_t(pad.y));
        }
    }

    // --- Simulated touch from mouse in Gamepad View ---
    {
        const bool touched = (live_touch && g_touch_state.touched) || auto_touched;
        const uint16_t tx = touched ? (auto_touched ? auto_tx : vpad_touch_raw_x(g_touch_state.x)) : 0;
        const uint16_t ty = touched ? (auto_touched ? auto_ty : vpad_touch_raw_y(g_touch_state.y)) : 0;
        const uint16_t validity = touched ? VPAD_TP_VALID : VPAD_TP_INVALID_XY;
        write_touch_data(MEM, buf + kVpadTpNormal, tx, ty, touched, validity);
        write_touch_data(MEM, buf + kVpadTpFiltered1, tx, ty, touched, validity);
        write_touch_data(MEM, buf + kVpadTpFiltered2, tx, ty, touched, validity);
    }
#else
    write_touch_data(MEM, buf + kVpadTpNormal, 0, 0, 0, VPAD_TP_INVALID_XY);
    write_touch_data(MEM, buf + kVpadTpFiltered1, 0, 0, 0, VPAD_TP_INVALID_XY);
    write_touch_data(MEM, buf + kVpadTpFiltered2, 0, 0, 0, VPAD_TP_INVALID_XY);
#endif

    buttons |= stick_emulation(lx, ly, VPAD_STICK_L_EMULATION_LEFT, VPAD_STICK_L_EMULATION_RIGHT,
                               VPAD_STICK_L_EMULATION_UP, VPAD_STICK_L_EMULATION_DOWN);
    buttons |= stick_emulation(rx, ry, VPAD_STICK_R_EMULATION_LEFT, VPAD_STICK_R_EMULATION_RIGHT,
                               VPAD_STICK_R_EMULATION_UP, VPAD_STICK_R_EMULATION_DOWN);
    write_float(MEM, buf + 0x0C, lx);
    write_float(MEM, buf + 0x10, ly);
    write_float(MEM, buf + 0x14, rx);
    write_float(MEM, buf + 0x18, ry);

    set_camera_follow(MEM, !s_mouse_aims);
    vpad_update_motion(MEM, buf, rx, ry, motion_dt >= 0 ? motion_dt : motion_seconds(), has_phys_gyro ? phys_gyro : nullptr, has_phys_accel ? phys_accel : nullptr,
                       tutorial, mouse_yaw, mouse_pitch);
    rbrew_write8(MEM, buf + kVpadSlideVolume, 0xFF);
    rbrew_write8(MEM, buf + kVpadSlideVolumeEx, 0xFF);
    rbrew_write8(MEM, buf + kVpadBattery, 0xC0); // full

    uint32_t trigger = 0, release = 0;
    vpad_update_edges(buttons, trigger, release);
    rbrew_write32(MEM, buf + 0x00, buttons);
    rbrew_write32(MEM, buf + 0x04, trigger);
    rbrew_write32(MEM, buf + 0x08, release);

    log_controller_input(0, buttons, lx, ly, rx, ry, has_phys_gyro, phys_gyro);

    RET = 1; // 1 sample read
}

// VPADGetTPCalibratedPoint(chan, VPADTouchData* calibrated, const VPADTouchData* raw):
// raw 12-bit panel samples -> 1280x720 screen coordinates; touch/validity copied.
void vpad_calibrate_touch(uint16_t raw_x, uint16_t raw_y, uint16_t& x, uint16_t& y) {
    const float fx = std::fmax(0.0f, raw_x - kTpOffsetX) * (1280.0f / kTpRangeX);
    const float fy = std::fmax(0.0f, 4095.0f - raw_y - kTpOffsetY) * (720.0f / kTpRangeY);
    x = uint16_t(std::fmin(1279.0f, std::round(fx)));
    y = uint16_t(std::fmin(719.0f, std::round(fy)));
}
static void VPADGetTPCalibratedPoint(CPUState* cpu) {
    const uint32_t outp = ARG1, inp = ARG2;
    if (outp && inp && rbrew_memory_accessible(MEM, inp, 8) && rbrew_memory_accessible(MEM, outp, 8)) {
        uint16_t x = 0, y = 0;
        vpad_calibrate_touch(rbrew_read16(MEM, inp), rbrew_read16(MEM, inp + 2), x, y);
        const uint16_t touched = rbrew_read16(MEM, inp + 4), validity = rbrew_read16(MEM, inp + 6);
        write_touch_data(MEM, outp, x, y, touched, validity);
    }
    RET = 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
// VPADControlMotor(chan, pattern, length): a bit pattern, one bit per 1/120 s
// tick (bit 7 of the first byte first), up to 120 bits. The controller of
// player 1 rumbles for the pattern's duration, at the strength of its share
// of set bits. RBREW_RUMBLE=0 turns rumble off.
static bool rumble_enabled() {
    static const bool on = [] { const char* v = std::getenv("RBREW_RUMBLE"); return !(v && v[0] == '0'); }();
    return on;
}
static void VPADControlMotor(CPUState* cpu) {
    RET = 0;
#if defined(HAVE_SDL2)
    const uint32_t pattern = ARG1, bits = std::min<uint32_t>(ARG2 & 0xFFu, 120u);
    ControllerDevice* dev = get_controller(0);
    if (!rumble_enabled() || !dev || !dev->gc || !bits || !rbrew_memory_accessible(MEM, pattern, (bits + 7) / 8)) return;
    uint32_t on = 0;
    for (uint32_t i = 0; i < bits; ++i) on += (MEM[pattern + i / 8] >> (7 - i % 8)) & 1u;
    const Uint16 strength = Uint16(0xFFFFu * on / bits);
    SDL_GameControllerRumble(dev->gc, strength, strength, (bits * 1000 + 119) / 120);
#endif
}
static void VPADStopMotor(CPUState* cpu) {
    RET = 0;
#if defined(HAVE_SDL2)
    if (ControllerDevice* dev = get_controller(0); dev && dev->gc) SDL_GameControllerRumble(dev->gc, 0, 0, 0);
#endif
}

#include "vpad_addrs.h"

void vpad_register(CPUState* cpu) {
#define REG(name) rbrew_register_func(cpu, ADDR_##name, name)
    REG(VPADRead);
    REG(VPADControlMotor);
    REG(VPADStopMotor);
    REG(VPADGetTPCalibratedPoint);
    REG(VPADSetGyroDirection);
#undef REG
}

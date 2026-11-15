// mouse_camera.cpp - mouse yaw and pitch straight to the game camera (Splatoon v288).
//
// With motion aiming the behind-the-player camera follows the GamePad, through filters made for a
// pad held in the hands. The recompiler wraps them (`[hooks]` of the manifest); each hook lets the
// filter run on the pad's own motion (a controller's gyro, the stick) and adds the mouse after it,
// untouched: the camera moves by the angle the mouse moved, whatever came before and whatever the
// frame time.
//
// Yaw: each frame's angle goes through 02661878 (r3 = camera, f1 = radians; returns f1 = the angle
// to apply). It remembers the recent motion in each direction and scales down a motion that
// follows a fast one the other way, so the player can bring the pad back.
//
// Pitch: the camera turns the pad's tilt into an aim target (-1 bottom stop, +1 top stop) and
// moves its aim, camera+0xFC, toward it by a fifth of the gap per frame. 02662044 (r3 = camera,
// f1 = target, the caller's f16) returns that rate, scaled down the same way after a reversal.
// The caller then moves the aim toward its f16. The hook moves the aim by the mouse's share at
// once and adds the share to the target, so the game's easing only sees the pad's part.
#include "mouse_camera.h"
#include "../runtime/rebrewu_runtime.h"
#include "rebrewu/logger.hpp"

namespace rbrew_mouse_camera {

AngleQueue& yaw_queue() {
    static AngleQueue queue;
    return queue;
}

AngleQueue& pitch_queue() {
    static AngleQueue queue;
    return queue;
}

PitchAim& pitch_aim() {
    static PitchAim aim;
    return aim;
}

#if RBREW_GAME_REVISION == 288
namespace {
constexpr uint32_t kGyroYawFilter = 0x02661878u;
constexpr uint32_t kGyroPitchRate = 0x02662044u;
constexpr uint32_t kCameraAim = 0xFCu; // vertical aim of the camera, -1..1
// Camera degrees per degree of mouse: the turn the game gave the same pad rotation at 60 frames
// per second and neutral motion sensitivity, so the mouse feels as fast as before.
constexpr double kCameraPerMouse = 8.0 / 3.0;
constexpr double kCameraTurn = kCameraPerMouse * 3.14159265358979 / 180.0;
// Camera pitch = -7.5 + 67.5 * aim degrees (measured: +60 at the top stop, -75 at the bottom).
constexpr float kAimPerMouseDegree = float(kCameraPerMouse / 67.5);

void gyro_yaw_filter(CPUState* cpu, uint32_t, HostFunc original) {
    original(cpu);
    cpu->f[1] += double(yaw_queue().take()) * kCameraTurn;
}

void gyro_pitch_rate(CPUState* cpu, uint32_t, HostFunc original) {
    const uint32_t camera = cpu->r[3];
    const float target = float(cpu->f[1]);
    // Negative pitch looks up; a higher aim looks up.
    const float moved = pitch_aim().move(target, -pitch_queue().take() * kAimPerMouseDegree);
    if (moved != 0.0f) {
        const float aim = float(rbrew_read_f32(cpu->mem, camera + kCameraAim));
        rbrew_write_f32(cpu->mem, camera + kCameraAim, std::clamp(aim + moved, -1.0f, 1.0f));
    }
    cpu->f[1] = cpu->f[16] = target + pitch_aim().share();
    original(cpu);
}
} // namespace

void install() {
    if (!rbrew_set_guest_hook(kGyroYawFilter, gyro_yaw_filter) ||
        !rbrew_set_guest_hook(kGyroPitchRate, gyro_pitch_rate))
        RBREW_LOG_ERROR("mouse", "could not register the camera hooks\n");
}
#else
void install() {}
#endif

} // namespace rbrew_mouse_camera

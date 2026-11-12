// mouse_camera.cpp - mouse yaw straight to the game camera (Splatoon v288).
//
// With motion aiming the behind-the-player camera turns by the GamePad's yaw rate. Each frame's
// angle goes through 02661878 (r3 = camera, f1 = radians; returns f1 = the angle to apply), a
// filter made for a pad held in the hands: it remembers the recent motion in each direction and
// scales down a motion that follows a fast one the other way, so the player can bring the pad
// back. A mouse sent as pad rotation got the same treatment: after a fast move to the right, a
// move to the left turned far less.
//
// The recompiler wraps 02661878 (`[hooks]` of the manifest). The hook lets the filter run on the
// pad's own rotation (a controller's gyro) and adds the mouse angle after it, untouched: the
// camera turns by the angle the mouse moved, whatever came before and whatever the frame time.
#include "mouse_camera.h"
#include "../runtime/rebrewu_runtime.h"
#include "rebrewu/logger.hpp"

namespace rbrew_mouse_camera {

YawQueue& yaw_queue() {
    static YawQueue queue;
    return queue;
}

#if RBREW_GAME_REVISION == 288
namespace {
constexpr uint32_t kGyroYawFilter = 0x02661878u;
// Camera radians per degree of mouse yaw: the turn the game gave the same pad rotation at
// 60 frames per second and neutral motion sensitivity, so the mouse feels as fast as before.
constexpr double kCameraTurn = (8.0 / 3.0) * 3.14159265358979 / 180.0;

void gyro_yaw_filter(CPUState* cpu, uint32_t, HostFunc original) {
    original(cpu);
    cpu->f[1] += double(yaw_queue().take()) * kCameraTurn;
}
} // namespace

void install() {
    if (!rbrew_set_guest_hook(kGyroYawFilter, gyro_yaw_filter))
        RBREW_LOG_ERROR("mouse", "could not register the camera hook\n");
}
#else
void install() {}
#endif

} // namespace rbrew_mouse_camera

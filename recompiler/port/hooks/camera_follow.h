// camera_follow.h - how fast the behind-the-player camera swings back, while the mouse aims.
#pragma once
#include "../runtime/game_revision.h"
#include "../runtime/rebrewu_runtime.h"
#include <cstdint>

namespace rbrew_camera_follow {

// Fixes: with a mouse, strafing turned the camera on its own and fought the aim.
// GamePlayerBehindCamera turns back behind the player at 0.03 of the gap per frame.
// That rate is the rodata float 0x100E9C14 in 288, read by the camera update 026621E8 and
// four other camera functions. Measured 2026-10-26 (scripted tutorial, camera matrix
// trace): 2 s of strafing turned the camera +79.9 deg, +2.6 deg with the rate at 1%;
// stick and gyro turning were identical. While the mouse aims the rate is 1% of the
// original; the original returns as soon as a controller stick aims. Written only when
// the value found is one of the two, and only on 288.
inline void set(uint8_t* mem, bool mouse_aims) {
#if RBREW_GAME_REVISION == 288
    constexpr uint32_t kRate = 0x100E9C14u;
    constexpr float kOriginal = 0.03f, kMouse = 0.0003f;
    static int state = -1;
    if (!mem || state == int(mouse_aims)) return;
    const float current = float(rbrew_read_f32(mem, kRate));
    if (current == kOriginal || current == kMouse) rbrew_write_f32(mem, kRate, mouse_aims ? kMouse : kOriginal);
    state = int(mouse_aims);
#else
    (void)mem;
    (void)mouse_aims;
#endif
}

} // namespace rbrew_camera_follow

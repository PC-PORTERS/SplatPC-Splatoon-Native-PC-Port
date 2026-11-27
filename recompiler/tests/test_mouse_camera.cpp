// Mouse angle queues and pitch share between the pad reads and the camera (hooks/mouse_camera.h).
#include "hooks/camera_auto_pitch.h"
#include "hooks/mouse_camera.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>

static void check(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}

static bool near(float a, float b) {
    return std::fabs(a - b) < 1e-5f;
}

static void angle_queue() {
    rbrew_mouse_camera::AngleQueue queue;
    check(!queue.offer(5.0f), "no camera yet: the offer is refused");
    check(queue.take() == 0.0f, "a refused offer is not kept");

    check(queue.offer(2.0f) && queue.offer(-0.5f), "a camera took the angle: offers are accepted");
    check(queue.take() == 1.5f, "the camera gets the sum of the reads, sign included");
    check(queue.take() == 0.0f, "the angle is handed over once");

    check(queue.offer(3.0f) && queue.offer(-3.0f), "one way then the other");
    check(queue.take() == 0.0f, "opposite motions cancel exactly");

    for (int read = 0; read < 4; ++read) check(queue.offer(1.0f), "the camera may skip a few reads");
    check(!queue.offer(1.0f), "the camera stopped: the offer is refused");
    check(queue.take() == 0.0f, "an angle the camera never took is dropped, not replayed later");
    check(queue.offer(1.0f), "the camera is back");
}

static void pitch_aim() {
    rbrew_mouse_camera::PitchAim aim;
    check(near(aim.move(0.2f, 0.3f), 0.3f) && near(aim.share(), 0.3f), "the aim moves by the mouse at once");
    check(near(aim.move(0.2f, -0.1f), -0.1f) && near(aim.share(), 0.2f), "and back by the same amount");
    check(near(aim.move(-0.4f, 0.0f), 0.0f) && near(aim.share(), 0.2f), "the pad's target moves without the share");

    check(near(aim.move(0.5f, 1.0f), 0.3f) && near(aim.share(), 0.5f), "the top stop keeps target + share at 1");
    check(near(aim.move(0.5f, 2.0f), 0.0f), "motion past the stop is dropped");
    check(near(aim.move(0.5f, -0.25f), -0.25f) && near(aim.share(), 0.25f), "moving back answers at once");

    check(near(aim.move(0.9f, 0.0f), -0.15f) && near(aim.share(), 0.1f), "a pad target past the stop trims the share");
    check(near(aim.move(-0.5f, -3.0f), -0.6f) && near(aim.share(), -0.5f),
          "the bottom stop keeps target + share at -1");

    aim.reset();
    check(aim.share() == 0.0f, "the game re-centred the pad: the share goes back to 0");
}

static void auto_pitch_guard() {
    using rbrew_camera_auto_pitch::filter_gyro_target;
    using rbrew_camera_auto_pitch::Guard;

    check(near(filter_gyro_target(0.25f, 0.0f, false, false), 0.0f), "disabled filter must not alter gyro target");
    check(near(filter_gyro_target(0.25f, 0.0f, true, false), 0.25f),
          "idle downward gyro target must clamp to current aim");
    check(near(filter_gyro_target(0.25f, 0.45f, true, false), 0.45f), "upward gyro target must pass through");
    check(near(filter_gyro_target(0.25f, 0.0f, true, true), 0.0f),
          "manual input or Y-reset must allow downward gyro target");

    Guard guard;
    // First frame establishes baseline camera pointer without restoring.
    auto r0 = guard.post_update(0x12340000u, 0.30f, 0.20f, 10.0f, 8.0f, true, false);
    check(!r0.restore_aim && !r0.restore_stick, "first observation must only latch camera address");

    // Gradual downward auto-tilt while moving without input -> restored.
    auto r1 = guard.post_update(0x12340000u, 0.30f, 0.27f, 10.0f, 9.2f, true, false);
    check(r1.restore_aim && near(r1.aim, 0.30f), "downward auto-pitch must restore aim");
    check(r1.restore_stick && near(r1.stick, 10.0f), "downward auto-pitch must restore stick pitch");

    // Disabled -> never restores.
    auto r2 = guard.post_update(0x12340000u, 0.30f, 0.27f, 10.0f, 9.2f, false, false);
    check(!r2.restore_aim && !r2.restore_stick, "disabled guard must not restore");

    // Manual input or Y reset active -> allows downward movement.
    auto r3 = guard.post_update(0x12340000u, 0.30f, 0.27f, 10.0f, 9.2f, true, true);
    check(!r3.restore_aim && !r3.restore_stick, "manual input or Y reset must not be blocked");

    // Instantaneous big jump (cutscene/respawn) -> allowed.
    auto r4 = guard.post_update(0x12340000u, 0.40f, -0.20f, 20.0f, 0.0f, true, false);
    check(!r4.restore_aim && !r4.restore_stick, "large cutscene/respawn snap must not be blocked");
}

int main() {
    try {
        angle_queue();
        pitch_aim();
        auto_pitch_guard();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}

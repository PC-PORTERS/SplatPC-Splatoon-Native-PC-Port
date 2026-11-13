// Mouse angle queues and pitch share between the pad reads and the camera (hooks/mouse_camera.h).
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

int main() {
    try {
        angle_queue();
        pitch_aim();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}

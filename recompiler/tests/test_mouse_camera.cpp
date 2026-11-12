// Mouse yaw queue between the pad reads and the camera (hooks/mouse_camera.h).
#include "hooks/mouse_camera.h"
#include <cstdio>
#include <stdexcept>

static void check(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}

int main() {
    try {
        rbrew_mouse_camera::YawQueue queue;
        check(!queue.offer(5.0f), "no camera yet: the offer is refused");
        check(queue.take() == 0.0f, "a refused offer is not kept");

        check(queue.offer(2.0f) && queue.offer(-0.5f), "a camera took the yaw: offers are accepted");
        check(queue.take() == 1.5f, "the camera gets the sum of the reads, sign included");
        check(queue.take() == 0.0f, "the yaw is handed over once");

        check(queue.offer(3.0f) && queue.offer(-3.0f), "right then left");
        check(queue.take() == 0.0f, "opposite motions cancel exactly");

        for (int read = 0; read < 4; ++read) check(queue.offer(1.0f), "the camera may skip a few reads");
        check(!queue.offer(1.0f), "the camera stopped: the offer is refused");
        check(queue.take() == 0.0f, "yaw the camera never took is dropped, not replayed later");
        check(queue.offer(1.0f), "the camera is back");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}

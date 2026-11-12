// mouse_camera.h - mouse yaw handed straight to the game camera (hooks/mouse_camera.cpp).
#pragma once
#include <mutex>
#include <utility>

namespace rbrew_mouse_camera {

// Mouse yaw waiting for the camera, in degrees (positive turns left, like the virtual GamePad).
// Input offers every pad read's motion; the camera takes it once per update. While no camera
// takes it (menus, a camera that does not aim with motion), offers are refused and the caller
// turns the virtual GamePad instead.
class YawQueue {
public:
    bool offer(float degrees) {
        std::lock_guard lock(m_lock);
        if (m_reads >= kStaleReads) {
            m_pending = 0.0f;
            return false;
        }
        ++m_reads;
        m_pending += degrees;
        return true;
    }
    float take() {
        std::lock_guard lock(m_lock);
        m_reads = 0;
        return std::exchange(m_pending, 0.0f);
    }

private:
    // Pad reads without a camera update before the camera counts as gone; the game reads the
    // pad up to twice per frame.
    static constexpr int kStaleReads = 4;
    std::mutex m_lock;
    float m_pending = 0.0f;
    int m_reads = kStaleReads;
};

YawQueue& yaw_queue();
void install();

} // namespace rbrew_mouse_camera

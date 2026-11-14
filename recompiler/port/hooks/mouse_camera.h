// mouse_camera.h - mouse yaw and pitch handed straight to the game camera (hooks/mouse_camera.cpp).
#pragma once
#include <algorithm>
#include <mutex>
#include <utility>

namespace rbrew_mouse_camera {

// Mouse angle waiting for the camera, in degrees (signs of the virtual GamePad: positive yaw turns
// left, negative pitch looks up). Input offers every pad read's motion; the camera takes it once
// per update. While no camera takes it (menus, a camera that does not aim with motion), offers
// are refused and the caller turns the virtual GamePad instead.
class AngleQueue {
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

// The mouse's share of the camera's vertical aim, in the game's aim units (-1 bottom stop,
// +1 top stop). The game's own target (pad, gyro, stick) stays apart; the aim is target + share,
// kept within the stops: motion past a stop is dropped, so moving back answers at once.
class PitchAim {
public:
    // Adds `delta` to the share against this frame's game target; returns how far the aim moves.
    float move(float target, float delta) {
        std::lock_guard lock(m_lock);
        const float before = m_share;
        m_share = std::clamp(target + m_share + delta, -1.0f, 1.0f) - target;
        return m_share - before;
    }
    float share() {
        std::lock_guard lock(m_lock);
        return m_share;
    }
    // The game re-centred the pad (a load, a respawn): the mouse share goes with it.
    void reset() {
        std::lock_guard lock(m_lock);
        m_share = 0.0f;
    }

private:
    std::mutex m_lock;
    float m_share = 0.0f;
};

AngleQueue& yaw_queue();
AngleQueue& pitch_queue();
PitchAim& pitch_aim();
void install();

} // namespace rbrew_mouse_camera

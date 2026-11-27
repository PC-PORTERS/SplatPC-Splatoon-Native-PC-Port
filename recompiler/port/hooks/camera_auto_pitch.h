// camera_auto_pitch.h - suppresses Splatoon's automatic downward camera tilt
// while moving when no vertical aim input or Y-button recenter is active.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>

namespace rbrew_camera_auto_pitch {

inline constexpr uint32_t kCameraAim = 0xFCu;        // normalized vertical aim, -1..1
inline constexpr uint32_t kCameraStickPitch = 0xE0u; // stick-mode vertical pitch angle

// Countdown (in camera update frames) during which manual pitch input or an
// intentional Y-button / load recenter allows downward camera pitch changes.
inline std::atomic<uint32_t> s_manual_frames{0};
inline std::atomic<uint32_t> s_reset_frames{0};

inline void note_vertical_input() {
    s_manual_frames.store(2, std::memory_order_relaxed);
}

inline void note_reset() {
    s_reset_frames.store(24, std::memory_order_relaxed);
}

inline bool input_or_reset_active() {
    return s_manual_frames.load(std::memory_order_relaxed) > 0 || s_reset_frames.load(std::memory_order_relaxed) > 0;
}

inline bool consume_frame_allow() {
    bool allowed = false;
    uint32_t m = s_manual_frames.load(std::memory_order_relaxed);
    while (m > 0 && !s_manual_frames.compare_exchange_weak(m, m - 1, std::memory_order_relaxed)) {}
    if (m > 0) allowed = true;
    uint32_t r = s_reset_frames.load(std::memory_order_relaxed);
    while (r > 0 && !s_reset_frames.compare_exchange_weak(r, r - 1, std::memory_order_relaxed)) {}
    if (r > 0) allowed = true;
    return allowed;
}

// Clamps gyro target pitch passed to 02662044 so GamePlayerBehindCamera
// does not pull camera+0xFC downward during L_026648FC when auto-pitch is disabled.
inline float filter_gyro_target(float current_aim, float target_aim, bool enabled, bool allow_change) {
    if (enabled && !allow_change && std::isfinite(current_aim) && std::isfinite(target_aim) &&
        target_aim < current_aim) {
        return current_aim;
    }
    return target_aim;
}

class Guard {
public:
    struct PostResult {
        bool restore_aim = false;
        float aim = 0.0f;
        bool restore_stick = false;
        float stick = 0.0f;
    };

    PostResult post_update(uint32_t camera, float aim_before, float aim_after, float stick_before, float stick_after,
                           bool enabled, bool allow_change) {
        if (camera != m_camera) {
            m_camera = camera;
            return {};
        }
        if (!enabled || allow_change) return {};

        PostResult res{};
        // Restore gradual automatic downward pulls only; large instantaneous jumps
        // belong to cutscenes, respawns, or camera state resets.
        if (std::isfinite(aim_before) && std::isfinite(aim_after) && aim_after < aim_before &&
            (aim_before - aim_after) <= 0.35f) {
            res.restore_aim = true;
            res.aim = aim_before;
        }
        if (std::isfinite(stick_before) && std::isfinite(stick_after) && stick_after < stick_before &&
            (stick_before - stick_after) <= 15.0f) {
            res.restore_stick = true;
            res.stick = stick_before;
        }
        return res;
    }

    void reset() { m_camera = 0; }

private:
    uint32_t m_camera = 0;
};

} // namespace rbrew_camera_auto_pitch

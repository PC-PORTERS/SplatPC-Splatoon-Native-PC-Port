#pragma once
#include "../runtime/native_control_defaults.h"
#include <atomic>
#include <cstdint>

// Story-mode credits (scene kind 10, used by one scene only). The game advances one credits page per logic
// step but encodes a page (BC1, drawn through a 0x11F block-encoder colour buffer) only on drawn steps: a
// frame that runs two logic steps during the encode burst leaves a page that is never encoded and shows as
// noise. The burst starts as the scene loads, so from scene entry until the burst is over the speed keeper
// must not ask for two-step frames (at 30 FPS every frame is one, and the first page was lost).
constexpr int kStaffRollSceneKind = 10;
constexpr uint32_t kBc1EncoderTargetFormat = 0x11Fu;
constexpr unsigned kEncodeBurstGraceFrames = 3;  // frames without an encode still counted as the burst
constexpr unsigned kEncodeBurstWaitFrames = 300; // scene entry to first encode; given up past this

inline std::atomic<bool> g_staff_roll_encode_seen{false};

// GX2SetColorBuffer, guest thread.
inline void staff_roll_note_color_buffer(uint32_t format) {
    if (format == kBc1EncoderTargetFormat) g_staff_roll_encode_seen.store(true, std::memory_order_relaxed);
}

struct EncodeBurst {
    bool in_scene = false, waiting = false;
    unsigned left = 0, waited = 0;
    bool update(bool encoded, bool staff_roll) {
        if (!staff_roll) {
            in_scene = waiting = false;
            left = 0;
            return false;
        }
        if (!in_scene) {
            in_scene = waiting = true;
            waited = 0;
        }
        if (encoded) {
            waiting = false;
            left = kEncodeBurstGraceFrames;
            return true;
        }
        if (left) {
            --left;
            return true;
        }
        if (waiting && ++waited < kEncodeBurstWaitFrames) return true;
        waiting = false;
        return false;
    }
};

// Once per swap: true from the credits' scene entry until their pages are encoded.
inline bool staff_roll_encode_burst(uint8_t* mem) {
    static EncodeBurst burst;
    const bool encoded = g_staff_roll_encode_seen.exchange(false, std::memory_order_relaxed);
    return burst.update(encoded, rbrew_control_defaults::active_scene_type(mem) == kStaffRollSceneKind);
}

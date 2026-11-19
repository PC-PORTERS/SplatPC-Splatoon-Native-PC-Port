// camera_nan_guard.h - keeps the behind-the-player camera finite (Splatoon v288, hooks/gx2_camera_hooks.cpp).
//
// Black screen with music (17/11-19/11): in one frame the camera's matrix became +-inf, then NaN
// (cause not found: +0x168 had ramped up just before, but values above 1 are also seen in healthy
// play). The NaN then stays in the camera for good and the projection, the 3D sound listener and
// the TV image follow.
// Writing the last finite values back into the camera of a running game healed the camera and the
// projection within half a second.
//
// The guard runs at each camera update: while the camera is finite it keeps a copy of its float
// fields; when one is not finite it writes the copy back into the non-finite words only and
// resets +0x168 to its init value, the state the camera starts from.
#pragma once
#include <array>
#include <cstdint>
#include <utility>

namespace rbrew_camera_nan_guard {

// Camera fields seen non-finite on 19/11 ([begin, end) byte offsets, 4-byte words).
inline constexpr std::array<std::pair<uint32_t, uint32_t>, 4> kRanges{{
    {0x30, 0x64},   // matrix
    {0xAC, 0xC8},   // look-at state
    {0x140, 0x154}, // scales
    {0x16C, 0x184}, // previous frame copy
}};
inline constexpr uint32_t kBackPointer = 0x00; // the player that owns the camera
inline constexpr uint32_t kRamp = 0x168;       // ramped up just before the bad frame
inline constexpr uint32_t kRampInit = 0;       // 0.0f, what the camera init (02660418) stores

constexpr uint32_t word_count() {
    uint32_t n = 0;
    for (const auto& [begin, end] : kRanges) n += (end - begin) / 4;
    return n;
}

constexpr bool finite_bits(uint32_t bits) {
    return (bits & 0x7F800000u) != 0x7F800000u;
}

// Mem: uint32_t read32(uint32_t addr), void write32(uint32_t addr, uint32_t bits); big-endian floats as bits.
class Guard {
public:
    // Returns the number of words repaired (0: the camera was finite).
    template <class Mem> uint32_t update(Mem& mem, uint32_t camera) {
        Slot& slot = slot_for(camera, mem.read32(camera + kBackPointer));
        std::array<uint32_t, word_count()> now{};
        bool finite = true;
        uint32_t i = 0;
        for (const auto& [begin, end] : kRanges)
            for (uint32_t off = begin; off < end; off += 4, ++i) {
                now[i] = mem.read32(camera + off);
                finite = finite && finite_bits(now[i]);
            }
        if (finite) {
            slot.words = now;
            slot.valid = true;
            return 0;
        }
        if (!slot.valid) return 0; // nothing finite seen yet for this camera
        uint32_t repaired = 0;
        i = 0;
        for (const auto& [begin, end] : kRanges)
            for (uint32_t off = begin; off < end; off += 4, ++i)
                if (!finite_bits(now[i])) {
                    mem.write32(camera + off, slot.words[i]);
                    ++repaired;
                }
        mem.write32(camera + kRamp, kRampInit);
        return repaired;
    }

private:
    struct Slot {
        uint32_t camera = 0;
        uint32_t owner = 0;
        bool valid = false;
        std::array<uint32_t, word_count()> words{};
    };

    // A camera is known by its address and owner: a new scene's camera at the same address starts empty.
    Slot& slot_for(uint32_t camera, uint32_t owner) {
        for (Slot& s : m_slots)
            if (s.camera == camera && s.owner == owner) return s;
        Slot& s = m_slots[m_next++ % m_slots.size()];
        s = Slot{camera, owner};
        return s;
    }

    std::array<Slot, 4> m_slots{};
    uint32_t m_next = 0;
};

} // namespace rbrew_camera_nan_guard

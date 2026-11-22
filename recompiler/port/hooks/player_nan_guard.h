// player_nan_guard.h - keeps a player finite and its animation frame loops bounded (Splatoon v288,
// hooks/gx2_camera_hooks.cpp).
//
// Duo freeze (20/11): in one frame a player's position (+0x248) and ~15 other float fields became NaN.
// The same player update (0264FF04) then ran an animation frame controller (0265BE4C) whose loop
// `while (!(frame < length)) frame -= length` never ends on a NaN frame: the game loop hung for good.
//
// Two guards:
// - before 0265BE4C: a non-finite or huge frame, previous frame or step goes back to frame 0; a looping
//   controller with a non-finite or non-positive length skips this update (its loop could not end);
// - after 0264FF04: while the player's position is finite the guard keeps a copy of the player; when it
//   is not, the words that turned non-finite get their last finite value back.
#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace rbrew_player_nan_guard {

inline constexpr uint32_t kPlayerVtable = 0x100E96F4u; // word at player +0x04
inline constexpr uint32_t kPlayerBytes = 0x1180;       // players are 0x1184 bytes apart
inline constexpr uint32_t kPosition = 0x248;           // x, y, z
inline constexpr uint32_t kIndex = 0x24;               // 0, 1... in a match

constexpr bool finite_bits(uint32_t bits) {
    return (bits & 0x7F800000u) != 0x7F800000u;
}

// 0xFFFFFFFF is the -1 of integer fields (handles, indices), not a value the FPU produces.
constexpr bool bad_float_bits(uint32_t bits) {
    return !finite_bits(bits) && bits != 0xFFFFFFFFu;
}

// A word is written back only if its last copy was finite: integers such as -583 (0xFFFFFDB9) also have
// the bits of a NaN and stay as the game left them.
constexpr bool restores(uint32_t now, uint32_t copy) {
    return bad_float_bits(now) && !bad_float_bits(copy);
}

// Player +0x50: 0 for the player this game controls (0264FF04 then runs its control step 0264FD1C and
// its camera), another value for the copy of a player from another console (2 in the 20/11 duo).
inline constexpr uint32_t kControl = 0x50;

constexpr const char* role(uint32_t control) {
    return control == 0 ? "local player" : "copy of another console's player";
}

// Frame controller of 0265BE4C (r3): +0x08 frame, +0x0C previous frame, +0x10 step, +0x14 loop
// length, byte +0x2E loop flag (word +0x2C, big-endian).
inline constexpr std::array<uint32_t, 3> kFrameFields{0x08, 0x0C, 0x10};
inline constexpr uint32_t kLength = 0x14;
inline constexpr uint32_t kFlagsWord = 0x2C;
inline constexpr float kFrameLimit = 1.0e6f; // beyond ~1e9 a subtraction of the length is lost to rounding

enum class Frame { ok, repaired, skip };

template <class Mem> Frame check_frame(Mem& mem, uint32_t ctrl) {
    uint32_t repaired = 0;
    for (const uint32_t off : kFrameFields) {
        const float v = std::bit_cast<float>(mem.read32(ctrl + off));
        if (!std::isfinite(v) || std::fabs(v) > kFrameLimit) {
            mem.write32(ctrl + off, 0);
            ++repaired;
        }
    }
    const float length = std::bit_cast<float>(mem.read32(ctrl + kLength));
    const bool looping = ((mem.read32(ctrl + kFlagsWord) >> 8) & 0xFFu) != 0;
    if (looping && !(std::isfinite(length) && length > 0.0f)) return Frame::skip;
    return repaired ? Frame::repaired : Frame::ok;
}

constexpr uint32_t kPlayerWords = kPlayerBytes / 4;

// Words: a copy of the player from offset 0 (one 32-bit word per 4 bytes).
template <class Words> bool finite_position(const Words& words) {
    for (uint32_t i = 0; i < 3; ++i)
        if (!finite_bits(words[kPosition / 4 + i])) return false;
    return true;
}

// Mem: uint32_t read32(uint32_t addr), void write32(uint32_t addr, uint32_t bits); big-endian floats as bits.
class PlayerGuard {
public:
    // Returns the number of words repaired (0: the position was finite, or no finite copy yet).
    template <class Mem> uint32_t update(Mem& mem, uint32_t player) {
        Slot& slot = slot_for(player);
        for (uint32_t i = 0; i < kPlayerWords; ++i) m_now[i] = mem.read32(player + 4 * i);
        if (finite_position(m_now)) {
            slot.words = m_now;
            slot.valid = true;
            return 0;
        }
        if (!slot.valid) return 0;
        uint32_t repaired = 0;
        for (uint32_t i = 0; i < kPlayerWords; ++i)
            if (restores(m_now[i], slot.words[i])) {
                mem.write32(player + 4 * i, slot.words[i]);
                ++repaired;
            }
        m_last = &slot;
        return repaired;
    }

    // After an update that repaired words: the player as the game left it, and the copy written back.
    const std::array<uint32_t, kPlayerWords>& seen() const { return m_now; }
    const std::array<uint32_t, kPlayerWords>& restored() const { return m_last->words; }

private:
    struct Slot {
        uint32_t player = 0;
        bool valid = false;
        uint64_t used = 0;
        std::array<uint32_t, kPlayerWords> words{};
    };

    // Eight players in a match; the least recently updated player gives its slot away.
    Slot& slot_for(uint32_t player) {
        ++m_clock;
        Slot* oldest = &m_slots[0];
        for (Slot& s : m_slots) {
            if (s.player == player) {
                s.used = m_clock;
                return s;
            }
            if (s.used < oldest->used) oldest = &s;
        }
        *oldest = Slot{player, false, m_clock};
        return *oldest;
    }

    std::array<Slot, 16> m_slots{};
    std::array<uint32_t, kPlayerWords> m_now{};
    const Slot* m_last = &m_slots[0];
    uint64_t m_clock = 0;
};

} // namespace rbrew_player_nan_guard

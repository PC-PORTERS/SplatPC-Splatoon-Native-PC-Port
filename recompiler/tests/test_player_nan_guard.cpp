// Player NaN guard and animation frame controller guard (hooks/player_nan_guard.h).
#include "hooks/player_nan_guard.h"
#include <cstdio>
#include <map>
#include <stdexcept>

static void check(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}

struct FakeMem {
    std::map<uint32_t, uint32_t> words;
    uint32_t read32(uint32_t a) { return words[a]; }
    void write32(uint32_t a, uint32_t v) { words[a] = v; }
};

constexpr uint32_t kPlayer = 0x36EE180Cu;
constexpr uint32_t kCtrl = 0x41F86004u;
constexpr uint32_t kOne = 0x3F800000u;
constexpr uint32_t kForty = 0x42200000u;
constexpr uint32_t kNaN = 0x7FC00000u;
constexpr uint32_t kHuge = 0x4F800000u; // 2^32
constexpr uint32_t kHandle = 0xFFFFFFFFu;

int main() {
    using namespace rbrew_player_nan_guard;
    try {
        // Frame controller as found in the 20/11 dump: looping, length 40, frame/previous/step NaN.
        FakeMem mem;
        mem.write32(kCtrl + 0x08, kNaN);
        mem.write32(kCtrl + 0x0C, kNaN);
        mem.write32(kCtrl + 0x10, kNaN);
        mem.write32(kCtrl + kLength, kForty);
        mem.write32(kCtrl + kFlagsWord, 0x01000100u);
        check(check_frame(mem, kCtrl) == Frame::repaired, "a NaN frame is repaired");
        check(mem.read32(kCtrl + 0x08) == 0 && mem.read32(kCtrl + 0x10) == 0, "frame and step go back to 0");
        check(check_frame(mem, kCtrl) == Frame::ok, "a finite controller is left alone");
        mem.write32(kCtrl + 0x08, kHuge);
        check(check_frame(mem, kCtrl) == Frame::repaired, "a frame the loop cannot reduce is repaired");
        mem.write32(kCtrl + kLength, kNaN);
        check(check_frame(mem, kCtrl) == Frame::skip, "a looping controller with a NaN length is skipped");
        mem.write32(kCtrl + kLength, 0);
        check(check_frame(mem, kCtrl) == Frame::skip, "a looping controller with a zero length is skipped");
        mem.write32(kCtrl + kFlagsWord, 0x01000000u);
        check(check_frame(mem, kCtrl) == Frame::ok, "a clamping controller needs no length");

        // Player: position and another float turn NaN; a -1 handle and an untouched word stay as they are.
        FakeMem pm;
        for (uint32_t o = 0; o < kPlayerBytes; o += 4) pm.write32(kPlayer + o, kOne);
        pm.write32(kPlayer + 0x104, kHandle);
        PlayerGuard guard;
        check(guard.update(pm, kPlayer) == 0, "a finite player is left alone");
        pm.write32(kPlayer + kPosition, 0x40000000u);
        pm.write32(kPlayer + 0x44C, 0xFFFFFDBAu); // an integer counter at -582: NaN bits, not a float
        check(guard.update(pm, kPlayer) == 0, "a finite move is kept");
        pm.write32(kPlayer + kPosition, kNaN);
        pm.write32(kPlayer + kPosition + 4, kNaN);
        pm.write32(kPlayer + 0x318, kNaN);
        pm.write32(kPlayer + 0x104, kHandle);
        pm.write32(kPlayer + 0x44C, 0xFFFFFDB9u);
        check(guard.update(pm, kPlayer) == 3, "the non-finite floats are repaired");
        check(pm.read32(kPlayer + kPosition) == 0x40000000u, "the last finite position comes back");
        check(pm.read32(kPlayer + 0x318) == kOne && pm.read32(kPlayer + 0x104) == kHandle, "other words kept");
        check(pm.read32(kPlayer + 0x44C) == 0xFFFFFDB9u, "the counter is left as the game set it");
        check(!restores(0xFFFFFDB9u, 0xFFFFFDBAu) && restores(kNaN, kOne), "report and guard use one rule");
        check(role(0)[0] == 'l' && role(2)[0] == 'c', "local player, copy of another console's player");
        check(guard.seen()[kPosition / 4] == kNaN && guard.restored()[kPosition / 4] == 0x40000000u, "report views");

        FakeMem fresh;
        for (uint32_t o = 0; o < kPlayerBytes; o += 4) fresh.write32(0x37000000u + o, kNaN);
        check(guard.update(fresh, 0x37000000u) == 0, "nothing to write back before a finite copy");

        // Eight players in a match all keep their copy.
        FakeMem many;
        PlayerGuard g8;
        for (uint32_t p = 0; p < 8; ++p)
            for (uint32_t o = 0; o < kPlayerBytes; o += 4) many.write32(0x30000000u + p * 0x1184 + o, kOne);
        for (uint32_t p = 0; p < 8; ++p) g8.update(many, 0x30000000u + p * 0x1184);
        many.write32(0x30000000u + kPosition, kNaN);
        check(g8.update(many, 0x30000000u) == 1, "the first player's copy survives seven others");
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
    std::printf("player NaN guard: OK\n");
    return 0;
}

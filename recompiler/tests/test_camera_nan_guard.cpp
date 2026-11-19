// Behind-the-player camera NaN guard (hooks/camera_nan_guard.h).
#include "hooks/camera_nan_guard.h"
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

constexpr uint32_t kCam = 0x463CB188u;
constexpr uint32_t kOne = 0x3F800000u;
constexpr uint32_t kNaN = 0xFFC00000u;
constexpr uint32_t kInf = 0x7F800000u;

int main() {
    using namespace rbrew_camera_nan_guard;
    try {
        FakeMem mem;
        mem.write32(kCam, 0x37903974u);
        for (const auto& [b, e] : kRanges)
            for (uint32_t o = b; o < e; o += 4) mem.write32(kCam + o, kOne);
        mem.write32(kCam + kRamp, 0x3F80C49Cu); // 1.006
        Guard guard;

        check(guard.update(mem, kCam) == 0, "a finite camera is left alone");
        mem.write32(kCam + 0x30, 0x3F000000u);
        check(guard.update(mem, kCam) == 0, "a finite change is kept");

        mem.write32(kCam + 0x30, kInf);
        mem.write32(kCam + 0x48, kNaN);
        mem.write32(kCam + 0x8C, kNaN); // outside the watched fields
        check(guard.update(mem, kCam) == 2, "only the non-finite words are repaired");
        check(mem.read32(kCam + 0x30) == 0x3F000000u, "from the last finite update");
        check(mem.read32(kCam + 0x48) == kOne, "every watched range is repaired");
        check(mem.read32(kCam + 0x8C) == kNaN, "other fields are not touched");
        check(mem.read32(kCam + kRamp) == kRampInit, "the ramp goes back to its init value");

        mem.write32(kCam, 0x11111111u); // new scene, same address
        mem.write32(kCam + 0x30, kNaN);
        check(guard.update(mem, kCam) == 0, "a new camera is never repaired from the old one");
        check(mem.read32(kCam + 0x30) == kNaN, "nothing written without a finite copy");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "camera NaN guard test failed: %s\n", e.what());
        return 1;
    }
    std::puts("camera NaN guard tests passed");
    return 0;
}

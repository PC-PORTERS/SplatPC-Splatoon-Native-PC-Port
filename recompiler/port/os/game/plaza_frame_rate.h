#pragma once
#include <cstdint>
// Revision-288 plaza frame regulator. Addresses and offsets are revision-288 values
// (callsite 0242A8F8 / return 0242B0A0, render manager pointer 101E7BDC, flag +0x304).
constexpr uint32_t kPlazaHalfRateFunction = 0x0242A8F8u;
constexpr uint32_t kPlazaHalfRateReturn = 0x0242B0A0u;
constexpr bool plaza_skip_half_rate(uint32_t function, uint32_t lr, bool enabled) {
    return enabled && function == kPlazaHalfRateFunction && lr == kPlazaHalfRateReturn;
}

struct SpeedKeeper {
    static constexpr double kForgive = 8.0;
    double debt = 0.0;
    bool update(double elapsed_steps, unsigned steps_done) {
        debt += elapsed_steps - double(steps_done);
        if (debt > kForgive) debt = 0.0;
        if (debt < -1.0) debt = -1.0;
        return debt >= 1.0;
    }
};

struct PlazaGovernor {
    static constexpr float kSlowMs = 16.0f;
    static constexpr unsigned kSlowWindow = 30, kSlowLimit = 8;
    static constexpr float kHeadroomMs = 22.0f;
    static constexpr unsigned kFastWindow = 60, kFastNeeded = 54;
    static constexpr unsigned kMinHold = 120, kMaxHold = 1800, kRelapse = 90;
    bool half = false;
    unsigned hold = 0, next_hold = kMinHold, since_one_step = kRelapse;
    unsigned char slow[kSlowWindow] = {}, fast[kFastWindow] = {};
    unsigned slow_head = 0, slow_count = 0, fast_head = 0, fast_count = 0, samples = 0;
    void reset_windows() {
        for (auto& s : slow) s = 0;
        for (auto& f : fast) f = 0;
        slow_head = slow_count = fast_head = fast_count = samples = 0;
    }
    bool update(float work_ms) {
        ++samples;
        if (!half) {
            ++since_one_step;
            const unsigned char s = work_ms > kSlowMs;
            slow_count += s; slow_count -= slow[slow_head];
            slow[slow_head] = s; slow_head = (slow_head + 1) % kSlowWindow;
            if (slow_count >= kSlowLimit) {
                next_hold = since_one_step < kRelapse ? (next_hold * 2 > kMaxHold ? kMaxHold : next_hold * 2) : kMinHold;
                half = true; hold = next_hold; reset_windows();
            }
        } else {
            if (hold) --hold;
            const unsigned char f = work_ms < kHeadroomMs;
            fast_count += f; fast_count -= fast[fast_head];
            fast[fast_head] = f; fast_head = (fast_head + 1) % kFastWindow;
            if (!hold && samples >= kFastWindow && fast_count >= kFastNeeded) { half = false; since_one_step = 0; reset_windows(); }
        }
        return half;
    }
};

#if RBREW_GAME_REVISION == 288
#include "../../runtime/rebrewu_runtime.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>

// 0 = native 30 FPS, 1 = always one step, 2/unset = steady-speed keeper (default).
inline int plaza_mode() {
    static const int mode = [] {
        const char* v = std::getenv("RBREW_PLAZA_60FPS");
        if (!v || !*v) return 2;
        if (v[0] == '0') return 0;
        if (v[0] == '1') return 1;
        return 2;
    }();
    return mode;
}

namespace plaza_regulator_detail {
inline std::atomic<bool> requested{false};
inline std::atomic<bool> keeper_owns{false};
constexpr uint32_t kRenderManager = 0x101E7BDCu;
inline void half_rate(CPUState* cpu, uint32_t address, HostFunc original) {
    if (plaza_skip_half_rate(address, cpu->lr, plaza_mode() != 0)) { requested = true; return; }
    original(cpu);
}
}  // namespace plaza_regulator_detail

inline void plaza_frame_rate_register() {
    if (plaza_mode() == 0) return;
    if (!rbrew_set_guest_hook(kPlazaHalfRateFunction, plaza_regulator_detail::half_rate))
        std::fprintf(stderr, "[timing] plaza frame regulator: hook registration failed\n");
    else
        std::fprintf(stderr, "[timing] plaza frame regulator installed\n");
}

inline void plaza_frame_rate_update(uint8_t* mem, float work_ms) {
    (void)work_ms;
    if (plaza_mode() == 0 || !mem) return;
    const uint32_t manager = rbrew_read32(mem, plaza_regulator_detail::kRenderManager);
    if (!manager || !rbrew_memory_accessible(mem, manager + 0x304, 4)) return;
    if (plaza_mode() == 1 && plaza_regulator_detail::keeper_owns.load(std::memory_order_relaxed)) {
        const uint32_t flags = rbrew_read32(mem, manager + 0x304);
        if (flags & 1u) rbrew_write32(mem, manager + 0x304, flags & ~1u);
        plaza_regulator_detail::keeper_owns = false;
    }
}

inline void speed_keeper_update(uint8_t* mem, float frame_ms, uint32_t flags, uint32_t swap_interval) {
    if (plaza_mode() == 0 || !mem || frame_ms <= 0.0f) return;
    const uint32_t manager = rbrew_read32(mem, plaza_regulator_detail::kRenderManager);
    const bool valid = manager && rbrew_memory_accessible(mem, manager + 0x304, 4);
    const bool owns = plaza_regulator_detail::keeper_owns.load(std::memory_order_relaxed);
    const auto release = [&] {
        if (valid && owns) rbrew_write32(mem, manager + 0x304, rbrew_read32(mem, manager + 0x304) & ~1u);
        plaza_regulator_detail::keeper_owns = false;
    };
    if (!valid) { plaza_regulator_detail::keeper_owns = false; return; }
    if ((flags & 1u) && !owns) { plaza_regulator_detail::keeper_owns = false; return; }
    static SpeedKeeper keeper;
    const double step_ms = 1001.0 / 60.0 * double(swap_interval > 1 ? swap_interval : 1);
    const bool twice = keeper.update(double(frame_ms) / step_ms, (flags & 1u) ? 2u : 1u);
    const uint32_t now = rbrew_read32(mem, manager + 0x304);
    if (twice != bool(now & 1u))
        rbrew_write32(mem, manager + 0x304, twice ? (now | 1u) : (now & ~1u));
    plaza_regulator_detail::keeper_owns = twice;
}
#else
inline void plaza_frame_rate_register() {}
inline void plaza_frame_rate_update(uint8_t*, float) {}
inline void speed_keeper_update(uint8_t*, float, uint32_t, uint32_t) {}
#endif

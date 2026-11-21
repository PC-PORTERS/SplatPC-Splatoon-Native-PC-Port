// Cafe OS runs the game in non-IEEE mode: a single-precision result too small to be normal is zero. Kept as a
// denormal, the squared length of a remote player's speed, which halves each frame once it stops, passed the game's
// `x > 0` test; its Newton square root (1/sqrt(x), then squared in single) overflowed and the player became NaN:
// invisible with the player guard, frozen game without it.
#include "runtime/rebrewu_runtime.h"
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char* name) {
    if (condition) return;
    std::printf("FAIL: %s\n", name);
    ++failures;
}

// The game's square root step, as generated for 026CA810: y = 1/sqrt(x), y*y in single, then 3 - y*y*x.
bool newton_step_finite(double x) {
    const double y = 1.0 / std::sqrt(x);
    const double yy = rbrew_fround(y * y);
    return std::isfinite(rbrew_fround(-std::fma(yy, x, -3.0)));
}

// ps_sumN f5, f1, f3, f2: the sum f1.ps0 + f2.ps1 is an arithmetic result, the other slot is copied from f3.
// Flushing that copy broke the ink and wall rendering of the session host in a duo.
constexpr uint32_t ps_sum(uint32_t xo) {
    return (4u << 26) | (5u << 21) | (1u << 16) | (2u << 11) | (3u << 6) | (xo << 1);
}

template <uint32_t word> void check_sum(bool inline_path, const char* name) {
    const double denormal = double(std::bit_cast<float>(0x00011111u));
    CPUState cpu{};
    cpu.f[1] = denormal; // the sum denormal + 0 is flushed
    cpu.f[3] = denormal;
    cpu.ps1[3] = -denormal;
    if (inline_path)
        rbrew_ps_constant<word>(&cpu);
    else
        rbrew_ps(&cpu, word);
    const bool sum0 = ((word >> 1) & 31) == 10;
    const double sum = sum0 ? cpu.f[5] : cpu.ps1[5], copy = sum0 ? cpu.ps1[5] : cpu.f[5];
    const double copied = sum0 ? -denormal : denormal;
    check(sum == 0.0 && !std::signbit(sum), name);
    check(std::bit_cast<uint64_t>(copy) == std::bit_cast<uint64_t>(copied), name);
}

} // namespace

int main() {
    check(rbrew_fround(1e-40) == 0.0 && !std::signbit(rbrew_fround(1e-40)), "positive denormal result is +0");
    check(rbrew_fround(-1e-40) == 0.0 && std::signbit(rbrew_fround(-1e-40)), "negative denormal result is -0");
    check(rbrew_fround(FLT_MIN) == FLT_MIN, "smallest normal float is kept");
    check(rbrew_fround(1.5) == 1.5 && rbrew_fround(-0.0) == 0.0, "normal values and zero are kept");
    check(std::isnan(rbrew_fround(NAN)) && std::isinf(rbrew_fround(INFINITY)), "NaN and inf are kept");

    double speed = 1e-9;
    bool finite = true;
    for (int frame = 0; frame < 200 && speed != 0.0; ++frame) {
        const double length2 = rbrew_fround(speed * speed);
        if (length2 > 0.0) finite = finite && newton_step_finite(length2);
        speed = rbrew_fround(speed * 0.5);
    }
    check(finite, "a decaying speed never overflows the square root");
    check(speed == 0.0, "a decaying speed reaches zero");

    check_sum<ps_sum(10)>(false, "ps_sum0 keeps its copied slot (interpreter)");
    check_sum<ps_sum(10)>(true, "ps_sum0 keeps its copied slot (inline)");
    check_sum<ps_sum(11)>(false, "ps_sum1 keeps its copied slot (interpreter)");
    check_sum<ps_sum(11)>(true, "ps_sum1 keeps its copied slot (inline)");

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("non-IEEE mode tests passed\n");
    return 0;
}

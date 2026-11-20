// Cafe OS runs the game in non-IEEE mode: a single-precision result too small to be normal is zero. Kept as a
// denormal, the squared length of a remote player's speed, which halves each frame once it stops, passed the game's
// `x > 0` test; its Newton square root (1/sqrt(x), then squared in single) overflowed and the player became NaN:
// invisible with the player guard, frozen game without it.
#include "runtime/rebrewu_runtime.h"
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

    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("non-IEEE mode tests passed\n");
    return 0;
}

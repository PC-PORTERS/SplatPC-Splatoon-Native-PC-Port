// Single-precision arithmetic rounds its result to float before anything reads the register.
// Left in double, a value compared before it is stored differs from the console's: the end of
// an ink rail was read as past its last point and the player was sent back to its start.
#include "ir/ir_builder.hpp"
#include "ir/ir_function.hpp"
#include "ppc/semantics/ppc_semantics.hpp"
#include <cstdint>
#include <cstdio>

namespace {

using namespace rebrewu;

int failures = 0;

void check(bool condition, const char* name, const char* message) {
    if (condition) return;
    std::printf("FAIL: %s: %s\n", name, message);
    ++failures;
}

constexpr uint32_t kResult = 1; // frD of every encoded instruction
constexpr uint32_t kSingle = 59;
constexpr uint32_t kDouble = 63;

constexpr uint32_t a_form(uint32_t primary, uint32_t xo, uint32_t a, uint32_t b, uint32_t c) {
    return primary << 26 | kResult << 21 | a << 16 | b << 11 | c << 6 | xo << 1;
}

struct Lowered {
    bool ok{false};
    int rounds{0};          // FRound of the result register onto itself
    int fills{0};           // PairedFill of the result register
    bool round_first{true}; // no fill came before the rounding
};

Lowered lower(uint32_t word) {
    Lowered out;
    const auto insn = ppc::decode(word, 0x02000000u);
    if (!insn) return out;
    ir::IRFunction func;
    ir::IRBuilder builder(func);
    builder.set_insert_point(func.add_block(0x02000000u));
    out.ok = ppc::lower_to_ir(*insn, builder, func);
    for (const auto& instr : func.blocks.front().instrs) {
        if (instr.opcode == ir::Opcode::PairedFill) ++out.fills;
        if (instr.opcode != ir::Opcode::FRound) continue;
        const bool on_result = instr.result && instr.result->kind == ir::RegKind::FPR && instr.result->index == kResult;
        if (!on_result) continue;
        ++out.rounds;
        if (out.fills) out.round_first = false;
    }
    return out;
}

void expect(const char* name, uint32_t word, int rounds, int fills) {
    const Lowered got = lower(word);
    check(got.ok, name, "not lowered");
    check(got.rounds == rounds, name, "wrong number of float roundings");
    check(got.fills == fills, name, "wrong number of paired-single fills");
    check(got.round_first, name, "the result fills the paired slot before it is rounded");
}

} // namespace

int main() {
    expect("fadds", a_form(kSingle, 21, 2, 3, 0), 1, 1);
    expect("fsubs", a_form(kSingle, 20, 2, 3, 0), 1, 1);
    expect("fmuls", a_form(kSingle, 25, 2, 0, 3), 1, 1);
    expect("fdivs", a_form(kSingle, 18, 2, 3, 0), 1, 1);
    expect("fmadds", a_form(kSingle, 29, 2, 3, 4), 1, 1);
    expect("fmsubs", a_form(kSingle, 28, 2, 3, 4), 1, 1);
    expect("fnmadds", a_form(kSingle, 31, 2, 3, 4), 1, 1);
    expect("fnmsubs", a_form(kSingle, 30, 2, 3, 4), 1, 1);
    expect("fres", a_form(kSingle, 24, 0, 3, 0), 1, 1);

    // Double-precision arithmetic keeps its full result and leaves the paired slot alone.
    expect("fadd", a_form(kDouble, 21, 2, 3, 0), 0, 0);
    expect("fmul", a_form(kDouble, 25, 2, 0, 3), 0, 0);
    expect("fmadd", a_form(kDouble, 29, 2, 3, 4), 0, 0);

    // A float load is already exact, and frsp is itself the rounding.
    expect("lfs", 48u << 26 | kResult << 21 | 3u << 16 | 8u, 0, 1);
    expect("frsp", a_form(kDouble, 12, 0, 3, 0), 0, 1);

    if (failures) return 1;
    std::printf("single precision: all cases passed\n");
    return 0;
}

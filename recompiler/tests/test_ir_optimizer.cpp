// IR optimizer tests: compare-to-branch fusion, constant folding, CSE, DCE
// and carrying-arithmetic equivalence. Each case evaluates a small function
// with a reference model and with the optimized IR.
#include "ir/ir_function.hpp"
#include "ir/ir_module.hpp"
#include "ir/ir_optimizer.hpp"
#include <cstdio>
#include <optional>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition) return;
    std::printf("FAIL: %s\n", message);
    ++failures;
}

using namespace rebrewu::ir;

// Minimal evaluator for the integer subset the optimizer handles. Returns the
// final rD value, or the raw result register contents.
struct Eval {
    uint32_t gpr[32]{};
    uint32_t cr[8]{};
    uint32_t xer{0};
    uint32_t ctr{0};
    std::vector<std::pair<uint32_t, uint32_t>> temps; // (index, value)

    uint32_t operand(const IROperand& op) const {
        if (const auto* imm = std::get_if<ImmOp>(&op)) return uint32_t(imm->value);
        const auto& r = std::get<RegOp>(op).reg;
        switch (r.kind) {
        case RegKind::GPR: return gpr[r.index];
        case RegKind::CR: return cr[r.index];
        case RegKind::XER: return xer;
        case RegKind::CTR: return ctr;
        case RegKind::Temp:
            for (auto it = temps.rbegin(); it != temps.rend(); ++it)
                if (it->first == r.index) return it->second;
            return 0;
        default: return 0;
        }
    }
    void write(const VReg& r, uint32_t v) {
        switch (r.kind) {
        case RegKind::GPR: gpr[r.index] = v; break;
        case RegKind::CR: cr[r.index] = v; break;
        case RegKind::XER: xer = v; break;
        case RegKind::CTR: ctr = v; break;
        case RegKind::Temp: temps.emplace_back(r.index, v); break;
        default: break;
        }
    }
    void run(const IRFunction& func) {
        for (const auto& block : func.blocks) {
            for (const auto& instr : block.instrs) {
                const uint32_t a = instr.operands.size() > 0 ? operand(instr.operands[0]) : 0;
                const uint32_t b = instr.operands.size() > 1 ? operand(instr.operands[1]) : 0;
                const uint32_t c = instr.operands.size() > 2 ? operand(instr.operands[2]) : 0;
                uint32_t result = 0;
                bool has = true;
                switch (instr.opcode) {
                case Opcode::Nop: has = false; break;
                case Opcode::Move: result = a; break;
                case Opcode::Add: result = a + b; break;
                case Opcode::Sub: result = a - b; break;
                case Opcode::And: result = a & b; break;
                case Opcode::Or: result = a | b; break;
                case Opcode::Xor: result = a ^ b; break;
                case Opcode::Not: result = ~a; break;
                case Opcode::Shl: result = a << (b & 31u); break;
                case Opcode::Shr: result = a >> (b & 31u); break;
                case Opcode::CmpSigned: {
                    const int32_t x = int32_t(a), y = int32_t(b);
                    result = x < y ? 8u : x > y ? 4u : 2u;
                    break;
                }
                case Opcode::CmpUnsigned: result = a < b ? 8u : a > b ? 4u : 2u; break;
                case Opcode::AddCarryIn: result = a + b + (c & 1u); break;
                case Opcode::CarryFromAdd: result = uint32_t((uint64_t(a) + b + (c & 1u)) >> 32); break;
                case Opcode::SarResult: {
                    const uint32_t n = b & 63u;
                    result = n >= 32 ? uint32_t(int32_t(a) >> 31) : uint32_t(int32_t(a) >> n);
                    break;
                }
                case Opcode::SarCarryOut: {
                    const uint32_t n = b & 63u;
                    const uint32_t lost = n == 0 ? 0u : n >= 32 ? a : (a & ((1u << n) - 1u));
                    result = (int32_t(a) < 0 && lost != 0) ? 1u : 0u;
                    break;
                }
                case Opcode::Branch:
                case Opcode::BranchCmp:
                case Opcode::Return:
                case Opcode::ConditionalReturn:
                case Opcode::ConditionalReturnCmp:
                case Opcode::ConditionalCall:
                case Opcode::ConditionalCallCmp:
                case Opcode::Jump: has = false; break;
                default: has = false; break;
                }
                if (has && instr.result) write(*instr.result, result);
            }
        }
    }
};

bool has_opcode(const IRFunction& func, Opcode op) {
    for (const auto& block : func.blocks)
        for (const auto& instr : block.instrs)
            if (instr.opcode == op) return true;
    return false;
}

size_t count_opcode(const IRFunction& func, Opcode op) {
    size_t n = 0;
    for (const auto& block : func.blocks)
        for (const auto& instr : block.instrs)
            if (instr.opcode == op) ++n;
    return n;
}

IRFunction& only_function(IRModule& module) {
    module.functions.emplace_back();
    return module.functions.back();
}

// emit_carry_sum's shape: result = a + b + cin, XER.CA from the carry-out.
void test_carry_equivalence() {
    const uint32_t cases[][3] = {
        {0, 0, 0},
        {1, 1, 0},
        {0xFFFFFFFFu, 1, 0},
        {0x80000000u, 0x80000000u, 0},
        {0xFFFFFFFFu, 0xFFFFFFFFu, 1},
        {5, 7, 1},
        {0xFFFFFFFEu, 1, 1},
        {0x7FFFFFFFu, 1, 1},
    };
    for (const auto& tc : cases) {
        for (int complement = 0; complement <= 1; ++complement) {
            IRModule module;
            IRFunction& func = only_function(module);
            BasicBlock block;
            block.id = 0;
            const VReg a = VReg::temp(1), c = VReg::temp(2), result = VReg::temp(3), carry = VReg::temp(4),
                       xer_next = VReg::temp(5);
            block.append({Opcode::Move, a, {imm(tc[1])}});
            block.append({Opcode::Move, c, {imm(tc[2])}});
            // Subtract-from uses the complement of the left operand.
            const IROperand lhs = complement ? IROperand(imm(~tc[0])) : IROperand(imm(tc[0]));
            block.append({Opcode::AddCarryIn, result, {lhs, reg(a), reg(c)}});
            block.append({Opcode::CarryFromAdd, carry, {lhs, reg(a), reg(c)}});
            block.append({Opcode::Or, xer_next, {imm(0), reg(carry)}});
            block.append({Opcode::Move, VReg::gpr(3), {reg(result)}});
            block.append({Opcode::Move, VReg::xer(), {reg(xer_next)}});
            func.blocks.push_back(block);

            Eval reference;
            reference.run(func);
            optimize_function(func);
            Eval optimized;
            optimized.run(func);
            check(reference.gpr[3] == optimized.gpr[3], "carry: sum differs after optimize");
            check(reference.xer == optimized.xer, "carry: XER differs after optimize");
            // Reference model: one widened addition.
            const uint32_t lhs_v = complement ? ~tc[0] : tc[0];
            const uint64_t sum = uint64_t(lhs_v) + tc[1] + (tc[2] & 1u);
            check(optimized.gpr[3] == uint32_t(sum), "carry: sum is not the widened addition");
            check((optimized.xer & 1u) == uint32_t(sum >> 32), "carry: carry-out is not the widened carry");
        }
    }
}

// cmpwi cr0, r3, 0 ; bc -> beq extracts EQ from cr0. The optimizer must fuse
// the chain into BranchCmp and keep the same taken decision.
void test_compare_branch_fusion() {
    struct Case {
        int32_t lhs;
        uint32_t bit; // 3=LT, 2=GT, 1=EQ
        uint32_t sense;
        bool taken;
    };
    const Case cases[] = {
        {-1, 3, 1, true},  {1, 3, 1, false},  {0, 3, 1, false}, // blt
        {1, 2, 1, true},   {-1, 2, 1, false}, {0, 2, 1, false}, // bgt
        {0, 1, 1, true},   {7, 1, 1, false},                    // beq
        {7, 1, 0, true},   {0, 1, 0, false},                    // bne (bit clear)
        {-5, 3, 0, false}, {5, 3, 0, true},                     // bge (LT clear)
    };
    for (const auto& tc : cases) {
        IRModule module;
        IRFunction& func = only_function(module);
        BasicBlock block;
        block.id = 0;
        const VReg cmp = VReg::temp(1), shifted = VReg::temp(2), bit_val = VReg::temp(3), inverted = VReg::temp(4);
        block.append({Opcode::CmpSigned, VReg::cr(0), {reg(VReg::gpr(3)), imm(0)}});
        block.append({Opcode::Shr, shifted, {reg(VReg::cr(0)), imm(tc.bit)}});
        block.append({Opcode::And, bit_val, {reg(shifted), imm(1)}});
        if (tc.sense) {
            block.append({Opcode::Branch, std::nullopt, {reg(bit_val), imm(0x100u), imm(0x200u)}});
        } else {
            block.append({Opcode::Xor, inverted, {reg(bit_val), imm(1)}});
            block.append({Opcode::Branch, std::nullopt, {reg(inverted), imm(0x100u), imm(0x200u)}});
        }
        func.blocks.push_back(block);

        // Reference decision: extract the bit from the compare result.
        Eval pre;
        pre.gpr[3] = uint32_t(tc.lhs);
        pre.run(func);
        const bool bit_set = ((pre.cr[0] >> tc.bit) & 1) != 0;
        const bool reference_taken = tc.sense ? bit_set : !bit_set;

        optimize_function(func);
        check(has_opcode(func, Opcode::BranchCmp), "fusion: BranchCmp not produced");
        check(!has_opcode(func, Opcode::Branch), "fusion: plain Branch left behind");
        check(count_opcode(func, Opcode::Shr) == 0 && count_opcode(func, Opcode::And) == 0,
              "fusion: extraction chain not removed");

        for (const auto& b : func.blocks)
            for (const auto& instr : b.instrs)
                if (instr.opcode == Opcode::BranchCmp) {
                    const uint32_t pred = uint32_t(std::get<ImmOp>(instr.operands[4]).value);
                    const uint32_t kind = (pred >> 4) & 3, bit = (pred >> 1) & 7, sense = pred & 1;
                    check(kind == 0, "fusion: compare kind is not signed");
                    check(bit == tc.bit, "fusion: tested bit changed");
                    check(sense == tc.sense, "fusion: sense changed");
                    // Evaluate the fused test like the emitted C condition.
                    const int32_t x = tc.lhs, y = 0;
                    bool condition = bit == 3 ? x < y : bit == 2 ? x > y : x == y;
                    if (!sense) condition = !condition;
                    check(condition == reference_taken, "fusion: taken decision differs");
                }
    }
}

// Folding, identity removal and dead-temp elimination.
void test_fold_and_dce() {
    IRModule module;
    IRFunction& func = only_function(module);
    BasicBlock block;
    block.id = 0;
    const VReg sum = VReg::temp(1), dead = VReg::temp(2), copy = VReg::temp(3), folded = VReg::temp(4);
    block.append({Opcode::Add, sum, {imm(2), imm(3)}});       // folds to 5
    block.append({Opcode::Mul, dead, {imm(7), imm(9)}});      // dead: dropped
    block.append({Opcode::Move, copy, {reg(sum)}});           // copy: dropped
    block.append({Opcode::Add, folded, {reg(copy), imm(0)}}); // +0 identity
    block.append({Opcode::Move, VReg::gpr(3), {reg(folded)}});
    func.blocks.push_back(block);

    Eval reference;
    reference.run(func);
    optimize_function(func);
    Eval optimized;
    optimized.run(func);
    check(optimized.gpr[3] == 5, "fold: constant folding produced the wrong value");
    check(optimized.gpr[3] == reference.gpr[3], "fold: result differs from reference");
    check(!has_opcode(func, Opcode::Mul), "dce: dead temp not removed");
    check(count_opcode(func, Opcode::Move) <= 1, "fold: redundant moves not removed");
}

// sraw/srawi result and XER.CA carry-out against a direct reference model.
void test_sraw_equivalence() {
    const uint32_t values[] = {0u, 1u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0x80000001u, 0x12345678u, 0x87654321u};
    for (uint32_t v : values) {
        for (uint32_t n = 0; n <= 63; n += 7) {
            IRModule module;
            IRFunction& func = only_function(module);
            BasicBlock block;
            block.id = 0;
            const VReg source = VReg::temp(1), amount = VReg::temp(2), result = VReg::temp(3), carry = VReg::temp(4);
            block.append({Opcode::Move, source, {imm(v)}});
            block.append({Opcode::Move, amount, {imm(n)}});
            block.append({Opcode::SarResult, result, {reg(source), reg(amount)}});
            block.append({Opcode::SarCarryOut, carry, {reg(source), reg(amount)}});
            block.append({Opcode::Move, VReg::gpr(3), {reg(result)}});
            block.append({Opcode::Move, VReg::xer(), {reg(carry)}});
            func.blocks.push_back(block);

            optimize_function(func);
            Eval optimized;
            optimized.run(func);
            // Independent reference: shift one bit at a time, like the PEM
            // describes it (sign fill; CA when a 1-bit leaves a negative rS).
            const uint32_t amount_v = n & 63u;
            int32_t x = int32_t(v);
            bool lost_one = false;
            for (uint32_t step = 0; step < amount_v; ++step) {
                if (uint32_t(x) & 1u) lost_one = true;
                const bool negative = x < 0;
                x = int32_t(uint32_t(x) >> 1) | (negative ? int32_t(0x80000000u) : 0);
            }
            const uint32_t want_result = uint32_t(x);
            const uint32_t want_carry = (int32_t(v) < 0 && lost_one) ? 1u : 0u;
            check(optimized.gpr[3] == want_result, "sraw: result differs from the reference model");
            check(optimized.xer == want_carry, "sraw: XER.CA differs from the reference model");
        }
    }
}

// Unrelated work between the compare and the branch must not block the fusion.
void test_fusion_across_unrelated_work() {
    IRModule module;
    IRFunction& func = only_function(module);
    BasicBlock block;
    block.id = 0;
    const VReg shifted = VReg::temp(2), bit_val = VReg::temp(3), so = VReg::temp(5);
    block.append({Opcode::CmpSigned, VReg::cr(0), {reg(VReg::gpr(3)), imm(0)}});
    block.append({Opcode::Move, VReg::gpr(7), {imm(0x42u)}});     // unrelated write
    block.append({Opcode::Shr, so, {reg(VReg::xer()), imm(31)}}); // emit_cr_so's SO bit
    block.append({Opcode::Or, VReg::cr(0), {reg(VReg::cr(0)), reg(so)}});
    block.append({Opcode::Shr, shifted, {reg(VReg::cr(0)), imm(3)}}); // LT
    block.append({Opcode::And, bit_val, {reg(shifted), imm(1)}});
    block.append({Opcode::Branch, std::nullopt, {reg(bit_val), imm(0x100u), imm(0x200u)}});
    func.blocks.push_back(block);

    Eval reference;
    reference.gpr[3] = 0xFFFFFFFFu;
    reference.run(func);
    optimize_function(func);
    check(has_opcode(func, Opcode::BranchCmp), "fusion: refused unrelated work between cmp and branch");
    Eval optimized;
    optimized.gpr[3] = 0xFFFFFFFFu;
    optimized.run(func);
    for (const auto& b : func.blocks)
        for (const auto& instr : b.instrs)
            if (instr.opcode == Opcode::BranchCmp) {
                const uint32_t pred = uint32_t(std::get<ImmOp>(instr.operands[4]).value);
                check(((pred >> 1) & 7) == 3, "fusion: tested bit changed across unrelated work");
            }
}

// A compare whose operands change before the branch must not be fused.
void test_no_fusion_across_writes() {
    IRModule module;
    IRFunction& func = only_function(module);
    BasicBlock block;
    block.id = 0;
    const VReg shifted = VReg::temp(2), bit_val = VReg::temp(3);
    block.append({Opcode::CmpSigned, VReg::cr(0), {reg(VReg::gpr(3)), imm(0)}});
    block.append({Opcode::Add, VReg::gpr(3), {reg(VReg::gpr(3)), imm(4)}}); // rewrites r3
    block.append({Opcode::Shr, shifted, {reg(VReg::cr(0)), imm(3)}});
    block.append({Opcode::And, bit_val, {reg(shifted), imm(1)}});
    block.append({Opcode::Branch, std::nullopt, {reg(bit_val), imm(0x100u), imm(0x200u)}});
    func.blocks.push_back(block);

    optimize_function(func);
    check(!has_opcode(func, Opcode::BranchCmp), "fusion: fused across a rewrite of the compared register");
    check(has_opcode(func, Opcode::Branch), "fusion: original branch lost");
}

// Equal expressions share one computation inside a block, but only while the
// named registers they read keep their values.
void test_cse() {
    IRModule module;
    IRFunction& func = only_function(module);
    BasicBlock block;
    block.id = 0;
    const VReg left = VReg::temp(1), right = VReg::temp(2), out = VReg::temp(3);
    block.append({Opcode::Xor, left, {reg(VReg::gpr(4)), imm(0xAAAAu)}});
    block.append({Opcode::Xor, right, {reg(VReg::gpr(4)), imm(0xAAAAu)}});
    block.append({Opcode::Add, out, {reg(left), reg(right)}});
    block.append({Opcode::Move, VReg::gpr(3), {reg(out)}});
    func.blocks.push_back(block);

    Eval reference;
    reference.gpr[4] = 0x12345678u;
    reference.run(func);
    optimize_function(func);
    Eval optimized;
    optimized.gpr[4] = 0x12345678u;
    optimized.run(func);
    check(optimized.gpr[3] == reference.gpr[3], "cse: result differs from reference");
    check(count_opcode(func, Opcode::Xor) == 1, "cse: duplicate expression not shared");
}

// A cached expression on a named register must not survive its rewrite, and a
// copy of a named register must not be forwarded across its rewrite.
void test_no_forwarding_across_register_rewrite() {
    { // CSE: r31+4 before and after `r31 = r3` are different values.
        IRModule module;
        IRFunction& func = only_function(module);
        BasicBlock block;
        block.id = 0;
        const VReg first = VReg::temp(1), second = VReg::temp(2), sum = VReg::temp(3);
        block.append({Opcode::Add, first, {reg(VReg::gpr(31)), imm(4)}});
        block.append({Opcode::Move, VReg::gpr(31), {reg(VReg::gpr(3))}});
        block.append({Opcode::Add, second, {reg(VReg::gpr(31)), imm(4)}});
        block.append({Opcode::Add, sum, {reg(first), reg(second)}});
        block.append({Opcode::Move, VReg::gpr(5), {reg(sum)}});
        func.blocks.push_back(block);

        Eval reference;
        reference.gpr[31] = 0x1000u;
        reference.gpr[3] = 0x2000u;
        reference.run(func);
        check(reference.gpr[5] == 0x1004u + 0x2004u, "cse-guard: reference model wrong");
        optimize_function(func);
        Eval optimized;
        optimized.gpr[31] = 0x1000u;
        optimized.gpr[3] = 0x2000u;
        optimized.run(func);
        check(optimized.gpr[5] == reference.gpr[5], "cse-guard: merged across a register rewrite");
        check(count_opcode(func, Opcode::Add) >= 2, "cse-guard: expression wrongly shared");
    }
    { // Copy propagation: t = r3; r3 = r4; r5 = t must keep the old r3.
        IRModule module;
        IRFunction& func = only_function(module);
        BasicBlock block;
        block.id = 0;
        const VReg copy = VReg::temp(1);
        block.append({Opcode::Move, copy, {reg(VReg::gpr(3))}});
        block.append({Opcode::Move, VReg::gpr(3), {reg(VReg::gpr(4))}});
        block.append({Opcode::Move, VReg::gpr(5), {reg(copy)}});
        func.blocks.push_back(block);

        Eval reference;
        reference.gpr[3] = 0x1111u;
        reference.gpr[4] = 0x2222u;
        reference.run(func);
        check(reference.gpr[5] == 0x1111u, "copy-guard: reference model wrong");
        optimize_function(func);
        Eval optimized;
        optimized.gpr[3] = 0x1111u;
        optimized.gpr[4] = 0x2222u;
        optimized.run(func);
        check(optimized.gpr[5] == 0x1111u, "copy-guard: copy forwarded across a register rewrite");
    }
}

} // namespace

int main() {
    test_carry_equivalence();
    test_sraw_equivalence();
    test_compare_branch_fusion();
    test_fold_and_dce();
    test_no_fusion_across_writes();
    test_fusion_across_unrelated_work();
    test_cse();
    test_no_forwarding_across_register_rewrite();
    if (failures) {
        std::printf("FAILED: %d checks\n", failures);
        return 1;
    }
    std::printf("OK: ir optimizer\n");
    return 0;
}
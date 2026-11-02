#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <unordered_map>

// ============================================================================
// RebrewU — Wii U static recompilation framework
// ir_types.hpp — Core IR value/instruction/operand types
//
// The IR is a simple 3-address SSA-style representation.  Each IRInstr
// produces at most one result (a VReg) and consumes zero or more IROperands.
// Guest register names (GPR0..GPR31, FPR0..FPR31, CR0..CR7, LR, CTR, XER)
// are preserved as-is so that the C++ emitter can map them to host variables.
// ============================================================================

namespace rebrewu::ir {

// ============================================================================
// VReg — virtual register
// ============================================================================

enum class RegKind : uint8_t {
    GPR,  // general purpose r0-r31
    FPR,  // floating point f0-f31
    CR,   // condition register field cr0-cr7
    LR,   // link register
    CTR,  // count register
    XER,  // fixed-point exception register
    Temp, // synthetic temporaries introduced by the recompiler
};

struct VReg {
    RegKind kind{RegKind::Temp};
    uint32_t index{0}; // register number (or temp id)

    bool operator==(const VReg&) const noexcept = default;

    static constexpr VReg gpr(uint32_t n) noexcept { return {RegKind::GPR, n}; }
    static constexpr VReg fpr(uint32_t n) noexcept { return {RegKind::FPR, n}; }
    static constexpr VReg cr(uint32_t n) noexcept { return {RegKind::CR, n}; }
    static constexpr VReg lr() noexcept { return {RegKind::LR, 0}; }
    static constexpr VReg ctr() noexcept { return {RegKind::CTR, 0}; }
    static constexpr VReg xer() noexcept { return {RegKind::XER, 0}; }
    static constexpr VReg temp(uint32_t n) noexcept { return {RegKind::Temp, n}; }
};

// ============================================================================
// IROperand — an instruction operand (register, immediate, or block label)
// ============================================================================

struct ImmOp {
    uint64_t value;
    bool operator==(const ImmOp&) const noexcept = default;
};
struct RegOp {
    VReg reg;
    bool operator==(const RegOp&) const noexcept = default;
};
struct LabelOp {
    uint32_t block_id;
    bool operator==(const LabelOp&) const noexcept = default;
}; // branch target block

using IROperand = std::variant<ImmOp, RegOp, LabelOp>;

// Helpers
inline IROperand imm(uint64_t v) {
    return ImmOp{v};
}
inline IROperand reg(VReg r) {
    return RegOp{r};
}
inline IROperand label(uint32_t bid) {
    return LabelOp{bid};
}

// ============================================================================
// Opcode
// ============================================================================

enum class Opcode : uint16_t {
    // Control flow
    Jump,              // unconditional branch to block
    Branch,            // conditional branch (cond, true_block, false_block)
    IndirectJump,      // bctr — indirect branch through CTR
    Call,              // bl — direct call (addr, [ret_reg])
    ConditionalCall,   // bcl/bcla — linked direct call (taken_condition, addr); resumes at PC+4
    IndirectCall,      // bctrl — indirect call through CTR
    Return,            // blr
    ConditionalReturn, // bclr — return to LR if condition, else fall through (cond, fallthrough_addr)
    // Fused compare-and-branch (cmp result stays in host flags; no CR field
    // materialisation). operands: a, b, taken, fallthrough/target, pred
    // pred = kind<<4 | cr_bit<<1 | sense  (kind: 0 signed, 1 unsigned, 2 float;
    // cr_bit: 3=LT, 2=GT, 1=EQ; sense: 1 = branch when the bit is set).
    BranchCmp,
    ConditionalReturnCmp, // (a, b, fallthrough_addr, pred): return when the fused test holds
    ConditionalCallCmp,   // (a, b, target_addr, pred): call when the fused test holds

    // Integer arithmetic
    Add,
    Sub,
    Mul,
    MulHigh,
    MulHighU,
    Div,
    DivU,
    Neg,
    Abs,

    // Bitwise
    And,
    Or,
    Xor,
    Not,
    Shl,
    Shr,
    Sar,
    RotLeft,
    RotRight,
    CountLeadingZeros,
    CountLeadingOnes,
    PopCount,
    ExtractBits, // rlwinm / rlwimi style

    // Comparison
    CmpSigned,
    CmpUnsigned,
    CmpFloat,

    // Memory
    Load8,
    Load8S,
    Load16,
    Load16S,
    Load32,
    LoadReserve32, // establishes a runtime reservation, even if result is unused
    Load64,
    LoadFloat32,
    LoadFloat64,
    Load16Reversed, // lhbrx: byte-reversed halfword load
    Load32Reversed, // lwbrx: byte-reversed word load
    Store8,
    Store16,
    Store32,
    StoreConditional32, // consumes reservation and updates CR0.EQ/SO
    Store64,
    StoreFloat32,
    StoreFloat64,
    StoreFloatInteger32, // stfiwx: raw low FPR word, not a float store
    Store16Reversed,     // sthbrx: byte-reversed halfword store
    Store32Reversed,     // stwbrx: byte-reversed word store
    Zero32Bytes,         // dcbz: zero a 32-byte cache line at ea
    PairedSingle,        // rbrew_ps(cpu, mem, word): paired-single / GQR instruction (imm word)
    PairedFill,          // ps1[n] = f[n]: single-precision scalar result fills slot 1 (imm n)
    Trap,                // rbrew_trap(cpu, word, pc): tw/twi, traps when the TO condition holds

    // Register transfer
    Move,
    ZeroExtend,
    SignExtend,

    // Carrying arithmetic: result = a + b + cin; CarryFromAdd yields the
    // 0/1 carry-out of the same widened sum (the host compiler folds both).
    AddCarryIn,
    CarryFromAdd,

    // sraw/srawi: arithmetic right shift by b (mod 64) and the 0/1 carry-out
    // of the same shift (XER.CA: value negative and 1-bits shifted out).
    SarResult,
    SarCarryOut,

    // Floating point
    FSelect, // nonrecord fsel: condition, nonnegative value, negative/NaN value
    FAdd,
    FSub,
    FMul,
    FDiv,
    FNeg,
    FAbs,
    FSqrt,
    FRound,
    FMadd,
    FMsub,
    FNmadd,
    FNmsub,
    FCvtToInt,
    FCvtToIntZero, // fctiwz integer payload and conversion status; operand 1 is Rc
    FCvtFromInt,
    FCvtPrecision,

    // Misc
    Nop,
    Phi,       // SSA φ-node
    Undefined, // unreachable / undefined behaviour
};

// ============================================================================
// IRInstr — a single IR instruction
// ============================================================================

struct IRInstr {
    Opcode opcode{Opcode::Nop};
    std::optional<VReg> result{};      // destination VReg (empty if void)
    std::vector<IROperand> operands{}; // source operands
    uint32_t guest_addr{0};            // originating guest PC (0 = synthetic)

    // Convenience ctor
    IRInstr(Opcode op, std::optional<VReg> dst, std::vector<IROperand> ops, uint32_t addr = 0)
        : opcode(op), result(dst), operands(std::move(ops)), guest_addr(addr) {}

    IRInstr() = default;
};

// ============================================================================
// BasicBlock — a maximal straight-line sequence of IRInstr
// ============================================================================

struct BasicBlock {
    uint32_t id{0};          // unique block id within a function
    uint32_t guest_start{0}; // guest address of first instruction
    uint32_t guest_end{0};   // guest address past last instruction (exclusive)

    std::vector<IRInstr> instrs{};

    // Successor block IDs (0-2 successors for branch, 0 for return)
    std::vector<uint32_t> successors{};
    // Predecessor block IDs
    std::vector<uint32_t> predecessors{};

    bool is_entry{false};
    bool is_exit{false};

    void append(IRInstr instr) { instrs.push_back(std::move(instr)); }
};

} // namespace rebrewu::ir

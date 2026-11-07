#include "ir_optimizer.hpp"
#include <algorithm>
#include <optional>
#include <unordered_map>
#include <vector>

namespace rebrewu::ir {
namespace {

// Side-effect-free with a droppable result. Loads are droppable but not
// shareable (memory may alias); LoadReserve32 and everything that writes
// memory or control flow state is not droppable.
bool is_droppable(Opcode op) {
    switch (op) {
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::MulHigh:
    case Opcode::MulHighU:
    case Opcode::Div:
    case Opcode::DivU:
    case Opcode::Neg:
    case Opcode::Abs:
    case Opcode::And:
    case Opcode::Or:
    case Opcode::Xor:
    case Opcode::Not:
    case Opcode::Shl:
    case Opcode::Shr:
    case Opcode::Sar:
    case Opcode::RotLeft:
    case Opcode::RotRight:
    case Opcode::CountLeadingZeros:
    case Opcode::CountLeadingOnes:
    case Opcode::PopCount:
    case Opcode::ExtractBits:
    case Opcode::CmpSigned:
    case Opcode::CmpUnsigned:
    case Opcode::CmpFloat:
    case Opcode::Move:
    case Opcode::ZeroExtend:
    case Opcode::SignExtend:
    case Opcode::AddCarryIn:
    case Opcode::CarryFromAdd:
    case Opcode::Load8:
    case Opcode::Load8S:
    case Opcode::Load16:
    case Opcode::Load16S:
    case Opcode::Load32:
    case Opcode::Load64:
    case Opcode::LoadFloat32:
    case Opcode::LoadFloat64:
    case Opcode::Load16Reversed:
    case Opcode::Load32Reversed: return true;
    default: return false;
    }
}

// Equal pure computations can share one result.
bool is_shareable(Opcode op) {
    switch (op) {
    case Opcode::Load8:
    case Opcode::Load8S:
    case Opcode::Load16:
    case Opcode::Load16S:
    case Opcode::Load32:
    case Opcode::Load64:
    case Opcode::LoadFloat32:
    case Opcode::LoadFloat64:
    case Opcode::Load16Reversed:
    case Opcode::Load32Reversed: return false;
    default: return is_droppable(op);
    }
}

bool is_foldable(Opcode op) {
    switch (op) {
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::And:
    case Opcode::Or:
    case Opcode::Xor:
    case Opcode::Not:
    case Opcode::Shl:
    case Opcode::Shr:
    case Opcode::Sar:
    case Opcode::Move:
    case Opcode::CmpSigned:
    case Opcode::CmpUnsigned: return true;
    default: return false;
    }
}

std::optional<uint64_t> fold(Opcode op, const std::vector<uint64_t>& a) {
    switch (op) {
    case Opcode::Move: return a.at(0);
    case Opcode::Not: return ~a.at(0);
    case Opcode::Add: return a.at(0) + a.at(1);
    case Opcode::Sub: return a.at(0) - a.at(1);
    case Opcode::Mul: return uint64_t(uint32_t(a.at(0)) * uint32_t(a.at(1)));
    case Opcode::And: return a.at(0) & a.at(1);
    case Opcode::Or: return a.at(0) | a.at(1);
    case Opcode::Xor: return a.at(0) ^ a.at(1);
    case Opcode::Shl: return uint32_t(a.at(0)) << (a.at(1) & 31u);
    case Opcode::Shr: return uint32_t(a.at(0)) >> (a.at(1) & 31u);
    case Opcode::Sar: return uint32_t(int32_t(uint32_t(a.at(0))) >> (a.at(1) & 31u));
    case Opcode::CmpSigned: {
        const int32_t x = int32_t(uint32_t(a.at(0))), y = int32_t(uint32_t(a.at(1)));
        return x < y ? 8u : x > y ? 4u : 2u;
    }
    case Opcode::CmpUnsigned: {
        const uint32_t x = uint32_t(a.at(0)), y = uint32_t(a.at(1));
        return x < y ? 8u : x > y ? 4u : 2u;
    }
    default: return std::nullopt;
    }
}

struct VRegHash {
    size_t operator()(const VReg& v) const { return (size_t(v.kind) << 32) ^ size_t(v.index); }
};

struct OperandKey {
    Opcode op;
    std::vector<IROperand> operands;
    bool operator==(const OperandKey& o) const { return op == o.op && operands == o.operands; }
    bool references(const VReg& r) const {
        for (const auto& operand : operands)
            if (const auto* reg_op = std::get_if<RegOp>(&operand))
                if (reg_op->reg == r) return true;
        return false;
    }
};

struct OperandKeyHash {
    size_t operator()(const OperandKey& k) const {
        size_t h = size_t(k.op) * 0x9E3779B97F4A7C15ull;
        for (const auto& operand : k.operands) {
            if (const auto* imm = std::get_if<ImmOp>(&operand))
                h ^= std::hash<uint64_t>{}(imm->value + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2));
            else if (const auto* r = std::get_if<RegOp>(&operand))
                h ^= std::hash<uint64_t>{}((uint64_t(r->reg.kind) << 32 | r->reg.index) + 0x9E3779B97F4A7C15ull +
                                           (h << 6) + (h >> 2));
            else if (const auto* l = std::get_if<LabelOp>(&operand))
                h ^= std::hash<uint64_t>{}(l->block_id + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2));
        }
        return h;
    }
};

// Canonical value of a temp: itself, another value, or a constant.
struct Canon {
    enum class Kind { Self, Alias, Const } kind{Kind::Self};
    IROperand value{};
};

class Simplifier {
public:
    explicit Simplifier(IRFunction& func) : m_func(func) {}

    void run() {
        for (const auto& block : m_func.blocks)
            for (const auto& instr : block.instrs)
                for (const auto& operand : instr.operands)
                    if (const auto* r = std::get_if<RegOp>(&operand)) ++m_total_uses[r->reg];
        for (auto& block : m_func.blocks) rewrite_block(block);
        drop_dead();
    }

private:
    // Follow alias chains to the canonical operand (or the value itself).
    IROperand resolve(const IROperand& operand) const {
        const auto* r = std::get_if<RegOp>(&operand);
        if (!r || r->reg.kind != RegKind::Temp) return operand;
        auto it = m_canon.find(r->reg);
        if (it == m_canon.end() || it->second.kind == Canon::Kind::Self) return operand;
        return resolve(it->second.value);
    }

    void rewrite_block(BasicBlock& block) {
        std::unordered_map<OperandKey, VReg, OperandKeyHash> available;
        std::unordered_map<VReg, uint32_t, VRegHash> block_uses;
        for (const auto& instr : block.instrs)
            for (const auto& operand : instr.operands)
                if (const auto* r = std::get_if<RegOp>(&operand)) ++block_uses[r->reg];
        // A temp whose result is reused outside this block cannot be replaced
        // by a block-local alias or shared expression.
        auto block_local = [&](VReg v) { return block_uses[v] == m_total_uses[v]; };

        for (auto& instr : block.instrs) {
            for (auto& operand : instr.operands) operand = resolve(operand);

            // Side effects (calls, stores, paired-singles, ...) can rewrite any
            // named register through the CPU state: drop all cached expressions.
            if (!is_droppable(instr.opcode) && instr.opcode != Opcode::Nop) available.clear();
            // A named register (GPR/FPR/CR/LR/CTR/XER) can be rewritten at any
            // point: expressions cached on its old value must go. Temps are
            // write-once and never invalidate anything.
            if (instr.result && instr.result->kind != RegKind::Temp) {
                for (auto it = available.begin(); it != available.end();)
                    it = it->first.references(*instr.result) ? available.erase(it) : std::next(it);
            }

            if (!instr.result || instr.result->kind != RegKind::Temp) continue;
            const VReg result = *instr.result;

            // Constant folding when every operand is an immediate.
            if (is_foldable(instr.opcode) && !instr.operands.empty()) {
                std::vector<uint64_t> constants;
                for (const auto& operand : instr.operands) {
                    const auto* imm_op = std::get_if<ImmOp>(&operand);
                    if (!imm_op) {
                        constants.clear();
                        break;
                    }
                    constants.push_back(imm_op->value);
                }
                if (constants.size() == instr.operands.size())
                    if (auto value = fold(instr.opcode, constants)) {
                        m_canon[result] = {Canon::Kind::Const, imm(*value)};
                        instr.opcode = Opcode::Nop;
                        instr.result.reset();
                        instr.operands.clear();
                        continue;
                    }
            }
            // Identity forms: Move(x), x+0, x-0, x|0, x&~0, shifts by 0.
            if (is_foldable(instr.opcode) && !instr.operands.empty()) {
                auto alias_to = [&](const IROperand& value) {
                    const bool is_const = std::get_if<ImmOp>(&value) != nullptr;
                    const bool is_temp = std::get_if<RegOp>(&value) && std::get<RegOp>(value).reg.kind == RegKind::Temp;
                    if (is_const) {
                        m_canon[result] = {Canon::Kind::Const, value}; // constant: propagates everywhere
                    } else if (is_temp && block_local(result)) {
                        m_canon[result] = {Canon::Kind::Alias, value}; // temp: write-once, stays in block
                    } else {
                        m_canon[result] = {Canon::Kind::Self, reg(result)};
                        return; // named register: keep the copy
                    }
                    instr.opcode = Opcode::Nop;
                    instr.result.reset();
                    instr.operands.clear();
                };
                if (instr.opcode == Opcode::Move) {
                    alias_to(instr.operands[0]);
                    continue;
                }
                auto imm_is = [&](size_t i, uint64_t v) {
                    const auto* imm_op = std::get_if<ImmOp>(&instr.operands[i]);
                    return imm_op && imm_op->value == v;
                };
                if (instr.operands.size() == 2 &&
                    (instr.opcode == Opcode::Add || instr.opcode == Opcode::Sub || instr.opcode == Opcode::Or ||
                     instr.opcode == Opcode::Shl || instr.opcode == Opcode::Shr || instr.opcode == Opcode::Sar) &&
                    imm_is(1, 0)) {
                    alias_to(instr.operands[0]);
                    continue;
                }
                if (instr.opcode == Opcode::Add && instr.operands.size() == 2 && imm_is(0, 0)) {
                    alias_to(instr.operands[1]);
                    continue;
                }
                if (instr.opcode == Opcode::And && instr.operands.size() == 2 && imm_is(1, 0xFFFFFFFFu)) {
                    alias_to(instr.operands[0]);
                    continue;
                }
            }
            // CSE for equal pure expressions within the block.
            if (is_shareable(instr.opcode) && block_local(result)) {
                OperandKey key{instr.opcode, instr.operands};
                auto it = available.find(key);
                if (it != available.end()) {
                    m_canon[result] = {Canon::Kind::Alias, reg(it->second)};
                    instr.opcode = Opcode::Nop;
                    instr.result.reset();
                    instr.operands.clear();
                    continue;
                }
                available.emplace(std::move(key), result);
            }
            m_canon[result] = {Canon::Kind::Self, reg(result)};
        }
    }

    void drop_dead() {
        std::unordered_map<VReg, uint32_t, VRegHash> uses;
        for (const auto& block : m_func.blocks)
            for (const auto& instr : block.instrs)
                for (const auto& operand : instr.operands)
                    if (const auto* r = std::get_if<RegOp>(&operand)) ++uses[r->reg];
        for (auto& block : m_func.blocks)
            for (auto& instr : block.instrs)
                if (instr.result && instr.result->kind == RegKind::Temp && is_droppable(instr.opcode) &&
                    uses[*instr.result] == 0) {
                    instr.opcode = Opcode::Nop;
                    instr.result.reset();
                    instr.operands.clear();
                }
    }

    IRFunction& m_func;
    std::unordered_map<VReg, Canon, VRegHash> m_canon;
    std::unordered_map<VReg, uint32_t, VRegHash> m_total_uses;
};

uint32_t cmp_kind(Opcode op) {
    switch (op) {
    case Opcode::CmpSigned: return 0;
    case Opcode::CmpUnsigned: return 1;
    case Opcode::CmpFloat: return 2;
    default: return 3;
    }
}

// Opaque instructions can rewrite any guest register through the CPU state
// (calls, paired-singles, reservation ops, traps).
bool is_opaque(Opcode op) {
    switch (op) {
    case Opcode::Call:
    case Opcode::ConditionalCall:
    case Opcode::IndirectCall:
    case Opcode::ConditionalCallCmp:
    case Opcode::LoadReserve32:
    case Opcode::StoreConditional32:
    case Opcode::PairedSingle:
    case Opcode::PairedFill:
    case Opcode::Trap: return true;
    default: return false;
    }
}

bool defines_reg(const IRInstr& instr, const IROperand& operand) {
    if (!instr.result) return false;
    const auto* r = std::get_if<RegOp>(&operand);
    return r && *instr.result == r->reg;
}

// True when the operand can only carry bit 0 (the SO flag emit_cr_so ORs into
// a CR field): an immediate 0/1, or a temp defined as `xer >> 31` or `x & 1`.
bool is_bit0_value(const std::vector<IRInstr>& instrs, const IROperand& operand, size_t before) {
    if (const auto* imm = std::get_if<ImmOp>(&operand)) return imm->value <= 1;
    const auto* r = std::get_if<RegOp>(&operand);
    if (!r || r->reg.kind != RegKind::Temp) return false;
    for (size_t k = before; k-- > 0;) {
        const IRInstr& def = instrs[k];
        if (!def.result || *def.result != r->reg) continue;
        if (def.opcode == Opcode::Shr && def.operands.size() == 2) {
            const auto* src = std::get_if<RegOp>(&def.operands[0]);
            const auto* amount = std::get_if<ImmOp>(&def.operands[1]);
            return src && src->reg.kind == RegKind::XER && amount && amount->value == 31;
        }
        if (def.opcode == Opcode::And && def.operands.size() == 2) {
            const auto* mask = std::get_if<ImmOp>(&def.operands[1]);
            return mask && mask->value == 1;
        }
        return false;
    }
    return false;
}

// Match cmp + CR-bit extract + test + branch (or conditional return/call) and
// return the fused opcode + pred encoding. Indices of the extraction chain
// that the fused opcode replaces are collected in `consumed`.
//
// The fused compare re-reads its operands at the branch, so the chain must be
// adjacent to the compare: any instruction in between (a call, a register
// rewrite) may change what the compare snapshotted. Only the known extraction
// ops and the emit_cr_so `Or cr, cr, so` are allowed between them.
struct FusionMatch {
    Opcode cmp_op;
    IROperand a, b;
    uint32_t pred;
    size_t cmp_idx;
    std::vector<size_t> consumed;
};

std::optional<FusionMatch> match_chain(const std::vector<IRInstr>& instrs, size_t branch_idx, VReg cond) {
    FusionMatch match;
    size_t i = branch_idx;
    VReg test_value = cond;

    // Optional inverting Xor: test_value = x ^ 1
    if (i == 0) return std::nullopt;
    const IRInstr& xor_def = instrs[i - 1];
    if (xor_def.result && *xor_def.result == test_value && xor_def.opcode == Opcode::Xor &&
        xor_def.operands.size() == 2) {
        const auto* imm_op = std::get_if<ImmOp>(&xor_def.operands[1]);
        const auto* src = std::get_if<RegOp>(&xor_def.operands[0]);
        if (!imm_op || imm_op->value != 1 || !src) return std::nullopt;
        match.pred = 0;
        match.consumed.push_back(i - 1);
        test_value = src->reg;
        --i;
    } else {
        match.pred = 1;
    }

    // And(x, 1)
    if (i == 0) return std::nullopt;
    const IRInstr& and_def = instrs[i - 1];
    if (!and_def.result || *and_def.result != test_value || and_def.opcode != Opcode::And ||
        and_def.operands.size() != 2)
        return std::nullopt;
    {
        const auto* imm_op = std::get_if<ImmOp>(&and_def.operands[1]);
        const auto* src = std::get_if<RegOp>(&and_def.operands[0]);
        if (!imm_op || imm_op->value != 1 || !src) return std::nullopt;
        match.consumed.push_back(i - 1);
        test_value = src->reg;
        --i;
    }

    // Shr(source, bit)
    if (i == 0) return std::nullopt;
    const IRInstr& shr_def = instrs[i - 1];
    if (!shr_def.result || *shr_def.result != test_value || shr_def.opcode != Opcode::Shr ||
        shr_def.operands.size() != 2)
        return std::nullopt;
    const auto* bit_imm = std::get_if<ImmOp>(&shr_def.operands[1]);
    const auto* shr_src = std::get_if<RegOp>(&shr_def.operands[0]);
    if (!bit_imm || !shr_src || bit_imm->value == 0 || bit_imm->value > 3) return std::nullopt;
    const uint32_t bit = uint32_t(bit_imm->value);
    match.pred |= bit << 1;
    match.consumed.push_back(i - 1);
    --i;

    // Source of the tested bit: a temp from the compare, or a CR field. The
    // compare must sit right in front of the chain (plus one emit_cr_so Or).
    const IRInstr* cmp = nullptr;
    size_t cmp_idx = 0;
    if (shr_src->reg.kind == RegKind::Temp) {
        for (size_t k = i + 1; k-- > 0;) {
            const IRInstr& def = instrs[k];
            if (!def.result || *def.result != shr_src->reg) continue;
            if (cmp_kind(def.opcode) == 3) return std::nullopt;
            cmp = &def;
            cmp_idx = k;
            break;
        }
    } else if (shr_src->reg.kind == RegKind::CR) {
        // Walk back to the compare that wrote the field, looking through a
        // single emit_cr_so `Or cr, cr, so` (it only sets the SO bit).
        bool or_skipped = false;
        for (size_t k = i + 1; k-- > 0;) {
            const IRInstr& def = instrs[k];
            if (!def.result || *def.result != shr_src->reg) continue;
            if (cmp_kind(def.opcode) != 3) {
                cmp = &def;
                cmp_idx = k;
                break;
            }
            if (!or_skipped && bit != 0 && def.opcode == Opcode::Or && def.operands.size() == 2 &&
                is_bit0_value(instrs, def.operands[1], k)) {
                const auto* dst = std::get_if<RegOp>(&def.operands[0]);
                if (dst && dst->reg == shr_src->reg) {
                    or_skipped = true;
                    continue;
                }
            }
            return std::nullopt; // some other write to the field
        }
    }
    if (!cmp || cmp->operands.size() != 2) return std::nullopt;

    // The fused compare re-reads its operands at the branch, so nothing in
    // between may rewrite them (or run an opaque instruction that could).
    // Unrelated work — the SO extraction, loads, stores, independent moves —
    // is fine and is common when the branch sits a few instructions after the
    // compare.
    for (size_t k = cmp_idx + 1; k < branch_idx; ++k) {
        const IRInstr& between = instrs[k];
        const bool consumed = std::find(match.consumed.begin(), match.consumed.end(), k) != match.consumed.end();
        if (consumed) continue;
        if (is_opaque(between.opcode)) return std::nullopt;
        if (defines_reg(between, cmp->operands[0]) || defines_reg(between, cmp->operands[1])) return std::nullopt;
    }

    match.cmp_op = cmp->opcode;
    match.a = cmp->operands[0];
    match.b = cmp->operands[1];
    match.pred |= cmp_kind(cmp->opcode) << 4;
    match.cmp_idx = cmp_idx;
    return match;
}

void fuse_compare_branches(IRFunction& func) {
    for (auto& block : func.blocks) {
        auto& instrs = block.instrs;
        for (size_t i = 0; i < instrs.size(); ++i) {
            IRInstr& instr = instrs[i];
            Opcode fused = Opcode::Nop;
            size_t cond_idx = 0, first_target = 0, pred_idx = 0;
            switch (instr.opcode) {
            case Opcode::Branch:
                fused = Opcode::BranchCmp;
                cond_idx = 0;
                first_target = 1;
                pred_idx = 4;
                break;
            case Opcode::ConditionalReturn:
                fused = Opcode::ConditionalReturnCmp;
                cond_idx = 0;
                first_target = 1;
                pred_idx = 3;
                break;
            case Opcode::ConditionalCall:
                fused = Opcode::ConditionalCallCmp;
                cond_idx = 0;
                first_target = 1;
                pred_idx = 3;
                break;
            default: continue;
            }
            const auto* cond = std::get_if<RegOp>(&instr.operands[cond_idx]);
            if (!cond || cond->reg.kind != RegKind::Temp) continue;
            auto match = match_chain(instrs, i, cond->reg);
            if (!match) continue;

            IRInstr replacement;
            replacement.opcode = fused;
            replacement.guest_addr = instr.guest_addr;
            replacement.operands.push_back(match->a);
            replacement.operands.push_back(match->b);
            for (size_t t = first_target; t < instr.operands.size(); ++t)
                replacement.operands.push_back(instr.operands[t]);
            replacement.operands.push_back(imm(match->pred));
            (void)pred_idx;

            instr = std::move(replacement);
            for (size_t idx : match->consumed) {
                instrs[idx].opcode = Opcode::Nop;
                instrs[idx].result.reset();
                instrs[idx].operands.clear();
            }
        }
    }
}

} // namespace

void optimize_function(IRFunction& func) {
    Simplifier(func).run();
    fuse_compare_branches(func);
    Simplifier(func).run();
}

} // namespace rebrewu::ir
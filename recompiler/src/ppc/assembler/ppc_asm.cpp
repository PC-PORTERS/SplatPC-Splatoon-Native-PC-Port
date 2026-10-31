#include "ppc_asm.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace rebrewu::ppc {
namespace {

std::string trim(std::string_view text) {
    std::size_t begin = 0, end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(begin, end - begin));
}

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool parse_integer(std::string_view text, int64_t& value) {
    const std::string raw = trim(text);
    std::size_t pos = 0;
    bool negative = false;
    if (!raw.empty() && (raw[0] == '+' || raw[0] == '-')) {
        negative = raw[0] == '-';
        pos = 1;
    }
    int base = 10;
    if (pos + 1 < raw.size() && raw[pos] == '0' && (raw[pos + 1] == 'x' || raw[pos + 1] == 'X')) {
        base = 16;
        pos += 2;
    }
    if (pos >= raw.size()) return false;

    uint64_t magnitude = 0;
    for (; pos < raw.size(); ++pos) {
        const char c = raw[pos];
        int digit = -1;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        if (digit < 0) return false;
        if (magnitude > (std::numeric_limits<uint64_t>::max() - uint64_t(digit)) / uint64_t(base)) return false;
        magnitude = magnitude * uint64_t(base) + uint64_t(digit);
    }

    if (negative) {
        if (magnitude > uint64_t(std::numeric_limits<int64_t>::max()) + 1u) return false;
        value = magnitude == uint64_t(std::numeric_limits<int64_t>::max()) + 1u ? std::numeric_limits<int64_t>::min()
                                                                                : -static_cast<int64_t>(magnitude);
    } else {
        if (magnitude > uint64_t(std::numeric_limits<int64_t>::max())) return false;
        value = static_cast<int64_t>(magnitude);
    }
    return true;
}

bool parse_u32(std::string_view text, uint32_t& value) {
    int64_t parsed = 0;
    if (!parse_integer(text, parsed) || parsed < 0 || parsed > std::numeric_limits<uint32_t>::max()) return false;
    value = static_cast<uint32_t>(parsed);
    return true;
}

bool parse_gpr(const std::string& text, uint32_t& reg) {
    const std::string value = lower(trim(text));
    if (value.size() < 2 || value[0] != 'r') return false;
    int64_t number = 0;
    if (!parse_integer(value.substr(1), number) || number < 0 || number > 31) return false;
    reg = static_cast<uint32_t>(number);
    return true;
}

bool parse_fpr(const std::string& text, uint32_t& reg) {
    const std::string value = lower(trim(text));
    if (value.size() < 2 || value[0] != 'f') return false;
    int64_t number = 0;
    if (!parse_integer(value.substr(1), number) || number < 0 || number > 31) return false;
    reg = static_cast<uint32_t>(number);
    return true;
}

bool parse_cr_field(const std::string& text, uint32_t& field) {
    const std::string value = lower(trim(text));
    if (value.size() != 3 || value.compare(0, 2, "cr") != 0 || value[2] < '0' || value[2] > '7') return false;
    field = static_cast<uint32_t>(value[2] - '0');
    return true;
}

bool parse_memory(const std::string& text, int32_t& displacement, uint32_t& base) {
    const std::string value = trim(text);
    const auto open = value.find('(');
    const auto close = value.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) return false;
    if (!trim(value.substr(close + 1)).empty()) return false;
    if (!parse_gpr(value.substr(open + 1, close - open - 1), base)) return false;

    const std::string offset = trim(value.substr(0, open));
    if (offset.empty()) {
        displacement = 0;
        return true;
    }
    int64_t parsed = 0;
    if (!parse_integer(offset, parsed) || parsed < -32768 || parsed > 32767) return false;
    displacement = static_cast<int32_t>(parsed);
    return true;
}

std::vector<std::string> split_operands(std::string_view text) {
    std::vector<std::string> operands;
    std::string current;
    int depth = 0;
    for (char c : text) {
        if (c == '(')
            ++depth;
        else if (c == ')' && depth > 0)
            --depth;
        if (c == ',' && depth == 0) {
            operands.push_back(trim(current));
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    if (!trim(current).empty()) operands.push_back(trim(current));
    return operands;
}

constexpr uint32_t encode_d(uint32_t opcode, uint32_t reg, uint32_t base, int64_t immediate) {
    return (opcode << 26) | ((reg & 31u) << 21) | ((base & 31u) << 16) | (static_cast<uint32_t>(immediate) & 0xFFFFu);
}

constexpr uint32_t encode_x(uint32_t opcode, uint32_t reg, uint32_t ra, uint32_t rb, uint32_t xo, bool rc = false) {
    return (opcode << 26) | ((reg & 31u) << 21) | ((ra & 31u) << 16) | ((rb & 31u) << 11) | ((xo & 0x3FFu) << 1) |
           (rc ? 1u : 0u);
}

constexpr uint32_t encode_xo(uint32_t opcode, uint32_t reg, uint32_t ra, uint32_t rb, uint32_t xo, bool oe, bool rc) {
    return (opcode << 26) | ((reg & 31u) << 21) | ((ra & 31u) << 16) | ((rb & 31u) << 11) | ((xo & 0x1FFu) << 1) |
           (oe ? (1u << 10) : 0u) | (rc ? 1u : 0u);
}

constexpr uint32_t encode_xl(uint32_t bo, uint32_t bi, uint32_t xo, bool lk) {
    return (19u << 26) | ((bo & 31u) << 21) | ((bi & 31u) << 16) | ((xo & 0x3FFu) << 1) | (lk ? 1u : 0u);
}

constexpr uint32_t encode_spr(uint32_t opcode, uint32_t reg, uint32_t spr, uint32_t xo) {
    return (opcode << 26) | ((reg & 31u) << 21) | ((spr & 31u) << 16) | (((spr >> 5) & 31u) << 11) |
           ((xo & 0x3FFu) << 1);
}

struct DFormSpec {
    const char* name;
    uint32_t opcode;
    bool floating;
};

struct XFormSpec {
    const char* name;
    uint32_t xo;
    bool floating;
};

struct XOFormSpec {
    const char* name;
    uint32_t xo;
    bool swap_fields;
};

constexpr DFormSpec kDForms[] = {
    {"lbz", 34, false},  {"lbzu", 35, false}, {"lhz", 40, false},  {"lhzu", 41, false}, {"lha", 42, false},
    {"lhau", 43, false}, {"lwz", 32, false},  {"lwzu", 33, false}, {"stb", 38, false},  {"stbu", 39, false},
    {"sth", 44, false},  {"sthu", 45, false}, {"stw", 36, false},  {"stwu", 37, false}, {"lmw", 46, false},
    {"stmw", 47, false}, {"lfs", 48, true},   {"lfsu", 49, true},  {"lfd", 50, true},   {"lfdu", 51, true},
    {"stfs", 52, true},  {"stfsu", 53, true}, {"stfd", 54, true},  {"stfdu", 55, true},
};

constexpr XFormSpec kXForms[] = {
    {"lwzx", 23, false},    {"lwzux", 55, false},  {"lbzx", 87, false},   {"lbzux", 119, false}, {"lhzx", 279, false},
    {"lhzux", 311, false},  {"lhax", 343, false},  {"lhaux", 375, false}, {"stwx", 151, false},  {"stwux", 183, false},
    {"stbx", 215, false},   {"stbux", 247, false}, {"sthx", 407, false},  {"sthux", 439, false}, {"lwarx", 20, false},
    {"lfsx", 535, true},    {"lfsux", 567, true},  {"lfdx", 599, true},   {"lfdux", 631, true},  {"stfsx", 663, true},
    {"stfsux", 695, true},  {"stfdx", 727, true},  {"stfdux", 759, true}, {"stfiwx", 983, true}, {"lwbrx", 534, false},
    {"stwbrx", 662, false}, {"stwcx", 150, false},
};

constexpr XOFormSpec kXOForms[] = {
    {"add", 266, false},   {"addc", 10, false},   {"adde", 138, false},  {"addme", 234, false},  {"addze", 202, false},
    {"subf", 40, false},   {"subfc", 8, false},   {"subfe", 136, false}, {"subfme", 232, false}, {"subfze", 200, false},
    {"neg", 104, false},   {"mullw", 235, false}, {"mulhw", 75, false},  {"mulhwu", 11, false},  {"divw", 491, false},
    {"divwu", 459, false}, {"and", 28, true},     {"andc", 60, true},    {"or", 444, true},      {"orc", 412, true},
    {"xor", 316, true},    {"nand", 476, true},   {"nor", 124, true},    {"eqv", 284, true},     {"slw", 24, true},
    {"srw", 536, true},    {"sraw", 792, true},   {"cntlzw", 26, true},  {"extsb", 954, true},   {"extsh", 922, true},
};

constexpr DFormSpec kImmediateForms[] = {
    {"mulli", 7, false},   {"subfic", 8, false}, {"addic", 12, false},
    {"addic.", 13, false}, {"addi", 14, false},  {"addis", 15, false},
};

const DFormSpec* find_d_form(std::string_view name) {
    for (const auto& spec : kDForms)
        if (name == spec.name) return &spec;
    return nullptr;
}

const XFormSpec* find_x_form(std::string_view name) {
    for (const auto& spec : kXForms)
        if (name == spec.name) return &spec;
    return nullptr;
}

const XOFormSpec* find_xo_form(std::string_view name) {
    for (const auto& spec : kXOForms)
        if (name == spec.name) return &spec;
    return nullptr;
}

const DFormSpec* find_immediate_form(std::string_view name) {
    for (const auto& spec : kImmediateForms)
        if (name == spec.name) return &spec;
    return nullptr;
}

bool parse_spr(const std::string& text, uint32_t& spr) {
    static const std::pair<const char*, uint32_t> names[] = {
        {"xer", 1},     {"lr", 8},     {"ctr", 9},     {"dsisr", 18},  {"dar", 19},    {"dec", 22},    {"sdr1", 25},
        {"srr0", 26},   {"srr1", 27},  {"tbl", 268},   {"tbu", 269},   {"sprg0", 272}, {"sprg1", 273}, {"sprg2", 274},
        {"sprg3", 275}, {"pvr", 287},  {"hid0", 1008}, {"hid1", 1009}, {"hid2", 920},  {"gqr0", 912},  {"gqr1", 913},
        {"gqr2", 914},  {"gqr3", 915}, {"gqr4", 916},  {"gqr5", 917},  {"gqr6", 918},  {"gqr7", 919},
    };
    const std::string value = lower(trim(text));
    for (const auto& [name, number] : names) {
        if (value == name) {
            spr = number;
            return true;
        }
    }
    int64_t number = 0;
    if (!parse_integer(value, number) || number < 0 || number > 1023) return false;
    spr = static_cast<uint32_t>(number);
    return true;
}

struct Parser {
    std::string mnemonic;
    std::vector<std::string> operands;
    uint32_t address{};
    std::string* error{};

    bool fail(const std::string& message) const {
        if (error) *error = message;
        return false;
    }

    bool operand_count(std::size_t expected) const { return operands.size() == expected; }

    bool gpr(std::size_t index, uint32_t& reg) const {
        return index < operands.size() && parse_gpr(operands[index], reg);
    }

    bool fpr(std::size_t index, uint32_t& reg) const {
        return index < operands.size() && parse_fpr(operands[index], reg);
    }

    bool integer(std::size_t index, int64_t& value) const {
        return index < operands.size() && parse_integer(operands[index], value);
    }

    bool u32(std::size_t index, uint32_t& value) const {
        return index < operands.size() && parse_u32(operands[index], value);
    }
};

bool fits_unsigned(int64_t value, unsigned bits) {
    return value >= 0 && value < (int64_t(1) << bits);
}

bool fits_immediate16(int64_t value) {
    return value >= -32768 && value <= 65535;
}

bool encode_branch(const Parser& parser, uint32_t target, bool absolute, bool link, uint32_t& word) {
    if (target & 3u) return parser.fail("branch target is not 4-byte aligned");
    if (!absolute) {
        const int32_t delta = static_cast<int32_t>(target - parser.address);
        if (delta < -(1 << 25) || delta > (1 << 25) - 4)
            return parser.fail("branch target is out of range; use ba/bla for an absolute branch");
        const uint32_t li = (static_cast<uint32_t>(delta) >> 2) & 0xFFFFFFu;
        word = (18u << 26) | (li << 2) | (link ? 1u : 0u);
        return true;
    }
    // The 24-bit LI field is a signed word displacement: it only reaches the
    // addresses where bits 26..31 repeat bit 25. Reject the gap in between so a
    // target never silently wraps into a different address.
    if (target > 0x01FFFFFCu && target < 0xFE000000u)
        return parser.fail("absolute branch target does not fit the 24-bit LI word field");
    const uint32_t li = (target >> 2) & 0xFFFFFFu;
    word = (18u << 26) | (li << 2) | 2u | (link ? 1u : 0u);
    return true;
}

bool encode_conditional_branch(const Parser& parser, uint32_t bo, uint32_t bi, uint32_t target, bool absolute,
                               bool link, uint32_t& word) {
    if (target & 3u) return parser.fail("branch target is not 4-byte aligned");
    if (!absolute) {
        const int32_t delta = static_cast<int32_t>(target - parser.address);
        if (delta < -32768 || delta > 32764)
            return parser.fail("branch target is out of range; use bca/bcla for an absolute branch");
        const uint32_t bd = (static_cast<uint32_t>(delta) >> 2) & 0x3FFFu;
        word = (16u << 26) | ((bo & 31u) << 21) | ((bi & 31u) << 16) | (bd << 2) | (link ? 1u : 0u);
        return true;
    }
    // The 14-bit BD field is a signed word displacement, so only targets whose
    // top 18 bits repeat bit 13 are reachable through an absolute conditional.
    if (target > 0x7FFCu && target < 0xFFFF8000u)
        return parser.fail("absolute conditional branch target does not fit the 14-bit BD word field");
    const uint32_t bd = (target >> 2) & 0x3FFFu;
    word = (16u << 26) | ((bo & 31u) << 21) | ((bi & 31u) << 16) | (bd << 2) | 2u | (link ? 1u : 0u);
    return true;
}

bool assemble_system(const Parser& p, uint32_t& word) {
    if (p.mnemonic == "nop") {
        word = 0x60000000u;
        return true;
    }
    if (p.mnemonic == "eieio") {
        word = encode_x(31, 0, 0, 0, 854);
        return true;
    }
    if (p.mnemonic == "sync") {
        word = 0x7C0004ACu;
        return true;
    }
    if (p.mnemonic == "isync") {
        word = 0x4C00012Cu;
        return true;
    }
    if (p.mnemonic == "sc") {
        word = 0x44000002u;
        return true;
    }
    if (p.mnemonic == "rfi") {
        word = 0x4C000064u;
        return true;
    }
    if (p.mnemonic == "tlbsync") {
        word = 0x7C00046Cu;
        return true;
    }
    return false;
}

bool assemble_branch(const Parser& p, uint32_t& word) {
    if (p.mnemonic == "blr" || p.mnemonic == "blrl" || p.mnemonic == "bctr" || p.mnemonic == "bctrl") {
        if (!p.operand_count(0)) return p.fail(p.mnemonic + " takes no operands");
        const bool link = p.mnemonic.back() == 'l';
        const uint32_t xo = p.mnemonic.starts_with("bc") ? 528 : 16;
        word = encode_xl(20, 0, xo, link);
        return true;
    }

    const bool unconditional = p.mnemonic == "b" || p.mnemonic == "bl" || p.mnemonic == "ba" || p.mnemonic == "bla";
    if (unconditional) {
        if (!p.operand_count(1)) return p.fail(p.mnemonic + " needs a target address");
        uint32_t target = 0;
        if (!p.u32(0, target)) return p.fail("bad branch target '" + p.operands[0] + "'");
        const bool absolute = p.mnemonic == "ba" || p.mnemonic == "bla";
        const bool link = p.mnemonic == "bl" || p.mnemonic == "bla";
        return encode_branch(p, target, absolute, link, word);
    }

    static const struct {
        const char* name;
        uint32_t bo;
        uint32_t bi;
    } conditions[] = {
        {"beq", 12, 2}, {"bne", 4, 2},  {"blt", 12, 0}, {"bgt", 12, 1},  {"ble", 4, 1},
        {"bge", 4, 0},  {"bso", 12, 3}, {"bns", 4, 3},  {"bdnz", 16, 0}, {"bdz", 18, 0},
    };

    std::string mnemonic = p.mnemonic;
    bool link = false;
    if (mnemonic.size() > 1 && mnemonic.back() == 'l') {
        mnemonic.pop_back();
        link = true;
    }

    if (mnemonic == "bc" || mnemonic == "bca") {
        if (!p.operand_count(3)) return p.fail(p.mnemonic + " needs BO, BI and a target");
        uint32_t bo = 0, bi = 0, target = 0;
        if (!p.u32(0, bo) || bo > 31) return p.fail("bad BO '" + p.operands[0] + "'");
        if (!p.u32(1, bi) || bi > 31) return p.fail("bad BI '" + p.operands[1] + "'");
        if (!p.u32(2, target)) return p.fail("bad branch target '" + p.operands[2] + "'");
        return encode_conditional_branch(p, bo, bi, target, mnemonic == "bca", link, word);
    }

    if (mnemonic == "bclr") {
        if (!p.operand_count(2)) return p.fail(p.mnemonic + " needs BO and BI");
        uint32_t bo = 0, bi = 0;
        if (!p.u32(0, bo) || bo > 31) return p.fail("bad BO '" + p.operands[0] + "'");
        if (!p.u32(1, bi) || bi > 31) return p.fail("bad BI '" + p.operands[1] + "'");
        word = encode_xl(bo, bi, 16, link);
        return true;
    }

    for (const auto& condition : conditions) {
        if (mnemonic != condition.name) continue;
        const bool has_cr = p.operand_count(2);
        if (!has_cr && !p.operand_count(1)) return p.fail(p.mnemonic + " needs a target address");
        uint32_t cr = 0, target = 0;
        if (has_cr && !parse_cr_field(p.operands[0], cr))
            return p.fail("bad condition register '" + p.operands[0] + "'");
        if (!p.u32(has_cr ? 1u : 0u, target)) return p.fail("bad branch target '" + p.operands[has_cr ? 1u : 0u] + "'");
        const uint32_t bi = condition.name[1] == 'd' ? 0 : cr * 4 + condition.bi;
        return encode_conditional_branch(p, condition.bo, bi, target, false, link, word);
    }
    return false;
}

bool assemble_d_form(const Parser& p, uint32_t& word) {
    const DFormSpec* spec = find_d_form(p.mnemonic);
    if (!spec) return false;
    if (!p.operand_count(2)) return p.fail(p.mnemonic + " needs a register and a memory operand");

    uint32_t reg = 0;
    const bool parsed = spec->floating ? p.fpr(0, reg) : p.gpr(0, reg);
    if (!parsed) return p.fail("bad register '" + p.operands[0] + "'");
    int32_t displacement = 0;
    uint32_t base = 0;
    if (!parse_memory(p.operands[1], displacement, base)) return p.fail("bad memory operand '" + p.operands[1] + "'");
    word = encode_d(spec->opcode, reg, base, displacement);
    return true;
}

bool assemble_immediate(const Parser& p, uint32_t& word) {
    if (p.mnemonic == "li" || p.mnemonic == "lis") {
        if (!p.operand_count(2)) return p.fail(p.mnemonic + " needs a register and an immediate");
        uint32_t reg = 0;
        int64_t immediate = 0;
        if (!p.gpr(0, reg)) return p.fail("bad register '" + p.operands[0] + "'");
        if (!p.integer(1, immediate) || !fits_immediate16(immediate)) return p.fail("bad 16-bit immediate");
        word = encode_d(p.mnemonic == "li" ? 14 : 15, reg, 0, immediate);
        return true;
    }

    if (p.mnemonic == "ori" || p.mnemonic == "oris" || p.mnemonic == "xori" || p.mnemonic == "xoris" ||
        p.mnemonic == "andi." || p.mnemonic == "andis.") {
        if (!p.operand_count(3)) return p.fail(p.mnemonic + " needs a destination, a source and an immediate");
        uint32_t destination = 0, source = 0;
        int64_t immediate = 0;
        if (!p.gpr(0, destination)) return p.fail("bad register '" + p.operands[0] + "'");
        if (!p.gpr(1, source)) return p.fail("bad register '" + p.operands[1] + "'");
        if (!p.integer(2, immediate) || !fits_unsigned(immediate, 16)) return p.fail("bad 16-bit immediate");
        uint32_t opcode = 24;
        if (p.mnemonic == "oris")
            opcode = 25;
        else if (p.mnemonic == "xori")
            opcode = 26;
        else if (p.mnemonic == "xoris")
            opcode = 27;
        else if (p.mnemonic == "andi.")
            opcode = 28;
        else if (p.mnemonic == "andis.")
            opcode = 29;
        word = encode_d(opcode, source, destination, immediate);
        return true;
    }

    if (p.mnemonic == "cmpwi" || p.mnemonic == "cmpi" || p.mnemonic == "cmplwi" || p.mnemonic == "cmpli") {
        bool has_cr = p.operand_count(3);
        if (!has_cr && !p.operand_count(2)) return p.fail(p.mnemonic + " needs a register and an immediate");
        uint32_t cr = 0, reg = 0;
        int64_t immediate = 0;
        if (has_cr && !parse_cr_field(p.operands[0], cr))
            return p.fail("bad condition register '" + p.operands[0] + "'");
        if (!p.gpr(has_cr ? 1u : 0u, reg)) return p.fail("bad register");
        if (!p.integer(has_cr ? 2u : 1u, immediate) || !fits_immediate16(immediate))
            return p.fail("bad 16-bit immediate");
        const bool logical = p.mnemonic == "cmplwi" || p.mnemonic == "cmpli";
        const uint32_t opcode = logical ? 10 : 11;
        word = (opcode << 26) | (cr << 23) | (reg << 16) | (static_cast<uint32_t>(immediate) & 0xFFFFu);
        return true;
    }

    const DFormSpec* spec = find_immediate_form(p.mnemonic);
    if (!spec) return false;
    if (!p.operand_count(3)) return p.fail(p.mnemonic + " needs a register, a base register and an immediate");
    uint32_t reg = 0, base = 0;
    int64_t immediate = 0;
    if (!p.gpr(0, reg)) return p.fail("bad register '" + p.operands[0] + "'");
    if (!p.gpr(1, base)) return p.fail("bad register '" + p.operands[1] + "'");
    if (!p.integer(2, immediate) || !fits_immediate16(immediate)) return p.fail("bad 16-bit immediate");
    word = encode_d(spec->opcode, reg, base, immediate);
    return true;
}

bool assemble_compare(const Parser& p, uint32_t& word) {
    if (p.mnemonic != "cmpw" && p.mnemonic != "cmp" && p.mnemonic != "cmplw" && p.mnemonic != "cmpl") return false;
    const bool has_cr = p.operand_count(3);
    if (!has_cr && !p.operand_count(2)) return p.fail(p.mnemonic + " needs two registers");
    uint32_t cr = 0, ra = 0, rb = 0;
    if (has_cr && !parse_cr_field(p.operands[0], cr)) return p.fail("bad condition register '" + p.operands[0] + "'");
    if (!p.gpr(has_cr ? 1u : 0u, ra)) return p.fail("bad register");
    if (!p.gpr(has_cr ? 2u : 1u, rb)) return p.fail("bad register");
    const uint32_t xo = p.mnemonic == "cmplw" || p.mnemonic == "cmpl" ? 32 : 0;
    word = encode_x(31, cr << 2, ra, rb, xo);
    return true;
}

bool assemble_x_form(const Parser& p, uint32_t& word) {
    std::string mnemonic = p.mnemonic;
    bool rc = false;
    if (mnemonic.size() > 1 && mnemonic.back() == '.') {
        mnemonic.pop_back();
        rc = true;
    }

    const XFormSpec* spec = find_x_form(mnemonic);
    if (spec) {
        if (!p.operand_count(3)) return p.fail(p.mnemonic + " needs three registers");
        uint32_t dest = 0, ra = 0, rb = 0;
        const bool destination = spec->floating ? p.fpr(0, dest) : p.gpr(0, dest);
        if (!destination) return p.fail("bad register '" + p.operands[0] + "'");
        if (!p.gpr(1, ra)) return p.fail("bad register '" + p.operands[1] + "'");
        if (!p.gpr(2, rb)) return p.fail("bad register '" + p.operands[2] + "'");
        const bool force_rc = mnemonic == "stwcx";
        word = encode_x(31, dest, ra, rb, spec->xo, rc || force_rc);
        return true;
    }

    if (mnemonic == "dcbz" || mnemonic == "dcbf" || mnemonic == "dcbst" || mnemonic == "icbi") {
        if (!p.operand_count(2)) return p.fail(mnemonic + " needs two registers");
        uint32_t ra = 0, rb = 0;
        if (!p.gpr(0, ra)) return p.fail("bad register '" + p.operands[0] + "'");
        if (!p.gpr(1, rb)) return p.fail("bad register '" + p.operands[1] + "'");
        uint32_t xo = 1014;
        if (mnemonic == "dcbf")
            xo = 86;
        else if (mnemonic == "dcbst")
            xo = 54;
        else if (mnemonic == "icbi")
            xo = 982;
        word = encode_x(31, 0, ra, rb, xo);
        return true;
    }

    if (mnemonic == "mr" || mnemonic == "not") {
        if (!p.operand_count(2)) return p.fail(p.mnemonic + " needs a destination and a source register");
        uint32_t dest = 0, source = 0;
        if (!p.gpr(0, dest)) return p.fail("bad register '" + p.operands[0] + "'");
        if (!p.gpr(1, source)) return p.fail("bad register '" + p.operands[1] + "'");
        word = encode_x(31, source, dest, source, mnemonic == "mr" ? 444 : 124, rc);
        return true;
    }

    const XOFormSpec* xo = find_xo_form(mnemonic);
    if (xo) {
        const std::string_view name = xo->name;
        if (name == "neg" || name == "addme" || name == "addze" || name == "subfme" || name == "subfze" ||
            name == "cntlzw" || name == "extsb" || name == "extsh") {
            if (!p.operand_count(2)) return p.fail(p.mnemonic + " needs a destination and a source register");
            uint32_t dest = 0, source = 0;
            if (!p.gpr(0, dest)) return p.fail("bad register '" + p.operands[0] + "'");
            if (!p.gpr(1, source)) return p.fail("bad register '" + p.operands[1] + "'");
            word = xo->swap_fields ? encode_x(31, source, dest, 0, xo->xo, rc)
                                   : encode_xo(31, dest, source, 0, xo->xo, false, rc);
            return true;
        }
        if (!p.operand_count(3)) return p.fail(p.mnemonic + " needs three registers");
        uint32_t dest = 0, ra = 0, rb = 0;
        if (!p.gpr(0, dest)) return p.fail("bad register '" + p.operands[0] + "'");
        if (!p.gpr(1, ra)) return p.fail("bad register '" + p.operands[1] + "'");
        if (!p.gpr(2, rb)) return p.fail("bad register '" + p.operands[2] + "'");
        word =
            xo->swap_fields ? encode_x(31, ra, dest, rb, xo->xo, rc) : encode_xo(31, dest, ra, rb, xo->xo, false, rc);
        return true;
    }
    return false;
}

bool assemble_rotate(const Parser& p, uint32_t& word) {
    std::string mnemonic = p.mnemonic;
    bool rc = false;
    if (mnemonic.size() > 1 && mnemonic.back() == '.') {
        mnemonic.pop_back();
        rc = true;
    }
    if (mnemonic != "rlwinm" && mnemonic != "rlwimi" && mnemonic != "rlwnm" && mnemonic != "srawi") return false;

    if (mnemonic == "srawi") {
        if (!p.operand_count(3)) return p.fail(p.mnemonic + " needs a destination, a source and a shift");
        uint32_t dest = 0, source = 0;
        int64_t shift = 0;
        if (!p.gpr(0, dest)) return p.fail("bad register '" + p.operands[0] + "'");
        if (!p.gpr(1, source)) return p.fail("bad register '" + p.operands[1] + "'");
        if (!p.integer(2, shift) || shift < 0 || shift > 31) return p.fail("bad shift amount");
        word = encode_x(31, source, dest, static_cast<uint32_t>(shift), 824, rc);
        return true;
    }

    if (!p.operand_count(5)) return p.fail(p.mnemonic + " needs a destination, a source, a shift and two masks");
    uint32_t dest = 0, source = 0, shift_or_register = 0;
    int64_t mb = 0, me = 0;
    if (!p.gpr(0, dest)) return p.fail("bad register '" + p.operands[0] + "'");
    if (!p.gpr(1, source)) return p.fail("bad register '" + p.operands[1] + "'");
    const bool shift_register = mnemonic == "rlwnm";
    if (shift_register) {
        if (!p.gpr(2, shift_or_register)) return p.fail("bad register '" + p.operands[2] + "'");
    } else {
        int64_t shift = 0;
        if (!p.integer(2, shift) || shift < 0 || shift > 31) return p.fail("bad shift amount");
        shift_or_register = static_cast<uint32_t>(shift);
    }
    if (!p.integer(3, mb) || mb < 0 || mb > 31) return p.fail("bad mask begin");
    if (!p.integer(4, me) || me < 0 || me > 31) return p.fail("bad mask end");
    const uint32_t opcode = mnemonic == "rlwimi" ? 20 : mnemonic == "rlwnm" ? 23 : 21;
    word = (opcode << 26) | (source << 21) | (dest << 16) | (shift_or_register << 11) |
           (static_cast<uint32_t>(mb) << 6) | (static_cast<uint32_t>(me) << 1) | (rc ? 1u : 0u);
    return true;
}

bool assemble_spr(const Parser& p, uint32_t& word) {
    for (const char* alias : {"mflr", "mfctr", "mfxer", "mtlr", "mtctr", "mtxer"}) {
        if (p.mnemonic != alias) continue;
        const bool move_to = p.mnemonic[0] == 'm' && p.mnemonic[1] == 't';
        uint32_t reg = 0, spr = 0;
        if (!p.operand_count(1)) return p.fail(p.mnemonic + " needs one register");
        if (!p.gpr(0, reg)) return p.fail("bad register '" + p.operands[0] + "'");
        if (p.mnemonic.ends_with("lr"))
            spr = 8;
        else if (p.mnemonic.ends_with("ctr"))
            spr = 9;
        else
            spr = 1;
        word = encode_spr(31, reg, spr, move_to ? 467 : 339);
        return true;
    }

    if (p.mnemonic == "mfspr" || p.mnemonic == "mtspr") {
        if (p.mnemonic == "mfspr") {
            if (!p.operand_count(2)) return p.fail("mfspr needs a register and an SPR");
            uint32_t reg = 0, spr = 0;
            if (!p.gpr(0, reg) || !parse_spr(p.operands[1], spr)) return p.fail("bad mfspr operand");
            word = encode_spr(31, reg, spr, 339);
            return true;
        }
        if (!p.operand_count(2)) return p.fail("mtspr needs an SPR and a register");
        uint32_t spr = 0, reg = 0;
        if (!parse_spr(p.operands[0], spr) || !p.gpr(1, reg)) return p.fail("bad mtspr operand");
        word = encode_spr(31, reg, spr, 467);
        return true;
    }

    if (p.mnemonic == "mfcr") {
        if (!p.operand_count(1)) return p.fail("mfcr needs a register");
        uint32_t reg = 0;
        if (!p.gpr(0, reg)) return p.fail("bad register '" + p.operands[0] + "'");
        word = encode_x(31, reg, 0, 0, 19);
        return true;
    }
    return false;
}

bool assemble_float(const Parser& p, uint32_t& word) {
    if (p.mnemonic == "fcmpu" || p.mnemonic == "fcmpo") {
        const bool has_cr = p.operand_count(3);
        if (!has_cr && !p.operand_count(2)) return p.fail(p.mnemonic + " needs two floating-point registers");
        uint32_t cr = 0, fa = 0, fb = 0;
        if (has_cr && !parse_cr_field(p.operands[0], cr))
            return p.fail("bad condition register '" + p.operands[0] + "'");
        if (!p.fpr(has_cr ? 1u : 0u, fa)) return p.fail("bad floating-point register");
        if (!p.fpr(has_cr ? 2u : 1u, fb)) return p.fail("bad floating-point register");
        word = encode_x(63, cr << 2, fa, fb, p.mnemonic == "fcmpo" ? 32 : 0);
        return true;
    }

    std::string mnemonic = p.mnemonic;
    bool rc = false;
    if (mnemonic.size() > 1 && mnemonic.back() == '.') {
        mnemonic.pop_back();
        rc = true;
    }

    static const struct {
        const char* name;
        uint32_t xo;
    } x_forms[] = {{"frsp", 12},  {"fctiw", 14},  {"fctiwz", 15}, {"fneg", 40},
                   {"fabs", 264}, {"fnabs", 136}, {"fmr", 72}};
    for (const auto& spec : x_forms) {
        if (mnemonic != spec.name) continue;
        if (!p.operand_count(2)) return p.fail(p.mnemonic + " needs two floating-point registers");
        uint32_t dest = 0, source = 0;
        if (!p.fpr(0, dest) || !p.fpr(1, source)) return p.fail("bad floating-point register");
        word = encode_x(63, dest, 0, source, spec.xo, rc);
        return true;
    }

    static const struct {
        const char* name;
        uint32_t opcode;
        uint32_t xo;
        bool has_a;
        bool third_is_c;
    } a_forms[] = {
        {"fdivs", 59, 18, true, false},   {"fsubs", 59, 20, true, false}, {"fadds", 59, 21, true, false},
        {"fsqrts", 59, 22, false, false}, {"fmuls", 59, 25, true, true},  {"fdiv", 63, 18, true, false},
        {"fsub", 63, 20, true, false},    {"fadd", 63, 21, true, false},  {"fsqrt", 63, 22, false, false},
        {"fmul", 63, 25, true, true},
    };
    for (const auto& spec : a_forms) {
        if (mnemonic != spec.name) continue;
        const std::size_t expected = spec.has_a ? 3 : 2;
        if (!p.operand_count(expected)) return p.fail(p.mnemonic + " has the wrong number of operands");
        uint32_t dest = 0, fa = 0, third = 0;
        if (!p.fpr(0, dest) || !p.fpr(expected - 1, third)) return p.fail("bad floating-point register");
        if (spec.has_a && !p.fpr(1, fa)) return p.fail("bad floating-point register");
        const uint32_t fb = spec.third_is_c ? 0 : third;
        const uint32_t fc = spec.third_is_c ? third : 0;
        word =
            (spec.opcode << 26) | (dest << 21) | (fa << 16) | (fb << 11) | (fc << 6) | (spec.xo << 1) | (rc ? 1u : 0u);
        return true;
    }
    return false;
}

bool assemble_raw(const Parser& p, uint32_t& word) {
    if (p.mnemonic != ".word" && p.mnemonic != ".long" && p.mnemonic != "word") return false;
    if (!p.operand_count(1)) return p.fail(p.mnemonic + " needs one 32-bit value");
    if (!p.u32(0, word)) return p.fail("bad word value '" + p.operands[0] + "'");
    return true;
}

} // namespace

bool assemble(std::string_view text, uint32_t address, uint32_t& word, std::string* error) {
    const std::string source = trim(text);
    const auto split = source.find_first_of(" \t");
    Parser parser;
    parser.mnemonic = lower(source.substr(0, split));
    parser.operands =
        split == std::string::npos ? std::vector<std::string>{} : split_operands(source.substr(split + 1));
    parser.address = address;
    parser.error = error;
    if (error) error->clear();
    if (parser.mnemonic.empty()) return parser.fail("empty instruction");

    // A handler that recognises the mnemonic reports a specific error and
    // returns false; stop there instead of replacing it with "unsupported".
    const auto failed = [error] { return error && !error->empty(); };
    if (assemble_raw(parser, word)) return true;
    if (failed()) return false;
    if (assemble_system(parser, word)) return true;
    if (failed()) return false;
    if (assemble_branch(parser, word)) return true;
    if (failed()) return false;
    if (assemble_d_form(parser, word)) return true;
    if (failed()) return false;
    if (assemble_immediate(parser, word)) return true;
    if (failed()) return false;
    if (assemble_compare(parser, word)) return true;
    if (failed()) return false;
    if (assemble_x_form(parser, word)) return true;
    if (failed()) return false;
    if (assemble_rotate(parser, word)) return true;
    if (failed()) return false;
    if (assemble_spr(parser, word)) return true;
    if (failed()) return false;
    if (assemble_float(parser, word)) return true;
    if (failed()) return false;

    if (error) *error = "unsupported instruction '" + source + "'";
    return false;
}

} // namespace rebrewu::ppc

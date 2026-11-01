// Native nn::nfp: replays the call sequence of Gambit's NFP worker loop
// (FUN_021B9BBC) against the emulated touchpoint and checks what the game
// reads back, including the checks it applies (FUN_021B9B10, FUN_021B9BAC).
#include "os/nn_act/nn_act.h"
#include "os/nn_nfp/nn_nfp.h"
#include "os/stubs/stubs_addrs.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

void require(bool ok, const char* what) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
}

struct Guest {
    std::vector<uint8_t> memory = std::vector<uint8_t>(0x10000);
    std::vector<HostFunc> table = std::vector<HostFunc>(
        (ADDR_SwitchToAmiiboSettings__Q2_2nn3nfpFRCQ3_2nn3nfp20AmiiboSettingsArgsInPCcUi >> 2) + 1);
    CPUState cpu{};
    Guest() {
        cpu.mem = memory.data();
        cpu.dispatch_table = table.data();
        cpu.dispatch_size = uint32_t(table.size());
        nn_nfp_register(&cpu);
    }
    uint32_t call(uint32_t address, uint32_t arg = 0) {
        cpu.r[3] = arg;
        require(cpu.dispatch_table[address >> 2] != nullptr, "nn_nfp import is registered");
        cpu.dispatch_table[address >> 2](&cpu);
        return cpu.r[3];
    }
    uint8_t at(uint32_t address) const { return memory[address]; }
    uint16_t be16(uint32_t address) const { return uint16_t(memory[address] << 8 | memory[address + 1]); }
};

// A 540-byte NTAG215 amiibo dump: the unencrypted pages of the Inkling Boy
// sample (UID 04 77 45 | BE | BA 7C 48 80, write counter 0, id at 0x54).
std::vector<uint8_t> inkling_boy_dump() {
    std::vector<uint8_t> dump(540);
    const uint8_t head[] = {0x04, 0x77, 0x45, 0xBE, 0xBA, 0x7C, 0x48, 0x80, 0x0E, 0x48,
                            0x0F, 0xE0, 0xF1, 0x10, 0xFF, 0xEE, 0xA5, 0x00, 0x00, 0x00};
    std::copy(std::begin(head), std::end(head), dump.begin());
    const uint8_t id[] = {0x08, 0x00, 0x02, 0x00, 0x00, 0x3F, 0x04, 0x02};
    std::copy(std::begin(id), std::end(id), dump.begin() + 0x54);
    const uint8_t locks[] = {0x01, 0x00, 0x0F, 0xBD, 0x00, 0x00, 0x00, 0x04, 0x5F, 0x00, 0x00, 0x00};
    std::copy(std::begin(locks), std::end(locks), dump.begin() + 0x208);
    return dump;
}

constexpr uint32_t kCtx = 0x1000; // Gambit's NFP context: TagInfo +0x0C, CommonInfo +0x60, RegisterInfo +0xA0
constexpr uint32_t kState = ADDR_GetNfpState__Q2_2nn3nfpFv;
bool failed(uint32_t result) {
    return (result & 0x80000000u) != 0;
}

void dumps() {
    rbrew_nfp::Tag tag;
    std::string error;
    auto dump = inkling_boy_dump();
    require(rbrew_nfp::parse_dump(dump, u"Boy", tag, error), "valid dump parses");
    const std::array<uint8_t, 7> uid{0x04, 0x77, 0x45, 0xBA, 0x7C, 0x48, 0x80};
    require(tag.uid == uid, "UID skips the BCC0 byte");
    require(tag.id[0] == 0x08 && tag.id[2] == 0x02 && tag.id[5] == 0x3F, "identification block");
    require(!rbrew_nfp::parse_dump(std::span(dump).first(100), u"Boy", tag, error), "short file refused");
    dump[0x0C] = 0;
    require(!rbrew_nfp::parse_dump(dump, u"Boy", tag, error), "non-amiibo pages refused");
    require(!rbrew_nfp::load_dump("does/not/exist.bin", tag, error), "missing file refused");
}

void scan_sequence() {
    rbrew_nfp::reset_for_tests();
    rbrew_nfp::Tag boy;
    std::string error;
    require(rbrew_nfp::parse_dump(inkling_boy_dump(), u"Boy", boy, error), "sample dump parses");
    Guest g;
    require(g.call(kState) == uint32_t(rbrew_nfp::State::Uninitialized), "boot state is uninitialized");
    require(!failed(g.call(ADDR_Initialize__Q2_2nn3nfpFv)), "Initialize succeeds");
    g.call(ADDR_StopDetection__Q2_2nn3nfpFv);
    require(g.call(kState) == uint32_t(rbrew_nfp::State::Initialized), "initialized before detection");
    require(!rbrew_nfp::consume_search_started(), "no picker before detection");
    require(!rbrew_nfp::touch(boy), "no amiibo can be placed before detection");

    require(!failed(g.call(ADDR_StartDetection__Q2_2nn3nfpFv)), "StartDetection succeeds");
    require(rbrew_nfp::consume_search_started(), "StartDetection opens the picker");
    require(!rbrew_nfp::consume_search_started(), "the picker opens once per detection");
    require(g.call(kState) == uint32_t(rbrew_nfp::State::Searching), "searching without an amiibo");
    require(failed(g.call(ADDR_GetTagInfo__Q2_2nn3nfpFPQ3_2nn3nfp7TagInfo, kCtx + 0x0C)), "no tag while searching");

    require(rbrew_nfp::touch(boy), "Inkling Boy is placed");
    require(g.call(kState) == uint32_t(rbrew_nfp::State::Found), "found after touch");
    require(!failed(g.call(ADDR_GetTagInfo__Q2_2nn3nfpFPQ3_2nn3nfp7TagInfo, kCtx + 0x0C)), "GetTagInfo succeeds");
    require(g.at(kCtx + 0x0C) == 7 && g.at(kCtx + 0x0D) == 0x04 && g.at(kCtx + 0x10) == 0xBA, "dump UID reported");
    require(g.at(kCtx + 0x0C + 0x21) == 2, "Type 2 tag");
    require(!failed(g.call(ADDR_Mount__Q2_2nn3nfpFv)), "Mount succeeds");
    require(g.call(kState) == uint32_t(rbrew_nfp::State::Mounted), "mounted");

    require(!failed(g.call(ADDR_GetNfpCommonInfo__Q2_2nn3nfpFPQ3_2nn3nfp10CommonInfo, kCtx + 0x60)),
            "GetNfpCommonInfo succeeds");
    // FUN_021B9B10: +0x66 == 8, +0x67 == 0, +0x68 in 1..3 (1 girl, 2 boy, 3 squid).
    require(g.at(kCtx + 0x66) == 0x08 && g.at(kCtx + 0x67) == 0x00 && g.at(kCtx + 0x68) == 2, "Inkling Boy id");
    require(g.at(kCtx + 0x69) == 0x04 && g.be16(kCtx + 0x6A) == 0x003F, "Splatoon series, model 0x003F");
    require(g.be16(kCtx + 0x6E) == 0xD8, "application area size");

    require(!failed(g.call(ADDR_GetNfpRegisterInfo__Q2_2nn3nfpFPQ3_2nn3nfp12RegisterInfo, kCtx + 0xA0)),
            "GetNfpRegisterInfo succeeds");
    // FUN_021B9BAC: registered when the nickname's first unit is nonzero.
    require(g.be16(kCtx + 0x100) == u'B' && g.be16(kCtx + 0x102) == u'o' && g.be16(kCtx + 0x106) == 0,
            "nickname is big-endian UTF-16");
    require(rbrew_ffl_crc16(&g.memory[kCtx + 0xA0], 0x60) == 0, "owner Mii is CRC-valid");

    g.call(ADDR_StopDetection__Q2_2nn3nfpFv);
    require(g.call(kState) == uint32_t(rbrew_nfp::State::Initialized), "amiibo leaves with the session");
    require(!rbrew_nfp::touch(boy), "no amiibo after the session");
    require(failed(g.call(ADDR_Mount__Q2_2nn3nfpFv)), "Mount fails without an amiibo");
}

void errors_and_settings() {
    require(rbrew_nfp::error_code(rbrew_nfp::kResultInvalidTag) == 1680400, "invalid tag error code");
    require(rbrew_nfp::error_code(0) == 1680000, "non-nfp result maps to the base code");

    rbrew_nfp::reset_for_tests();
    Guest g;
    g.memory[0x2000] = 0xA1;
    g.memory[0x2001] = 0xB0;
    g.memory[0x2002] = 0xC8;
    g.memory[0x2003] = 0x00;
    require(g.call(ADDR_GetErrorCode__Q2_2nn3nfpFRCQ2_2nn6Result, 0x2000) == 1680400,
            "GetErrorCode reads by reference");
    for (uint32_t i = 0; i < rbrew_nfp::kSettingsArgsInSize; ++i) g.memory[0x3000 + i] = 0xFF;
    g.memory[0x3000 + rbrew_nfp::kSettingsArgsInSize] = 0x5A;
    require(!failed(g.call(ADDR_InitializeAmiiboSettingsArgsIn__Q2_2nn3nfpFPQ3_2nn3nfp20AmiiboSettingsArgsIn, 0x3000)),
            "InitializeAmiiboSettingsArgsIn succeeds");
    for (uint32_t i = 0; i < rbrew_nfp::kSettingsArgsInSize; ++i) require(g.memory[0x3000 + i] == 0, "args zeroed");
    require(g.memory[0x3000 + rbrew_nfp::kSettingsArgsInSize] == 0x5A, "nothing past the args is touched");
}

} // namespace

// Optional arguments: real dumps to check, each printed with its UID and id.
int main(int argc, char** argv) {
    dumps();
    scan_sequence();
    errors_and_settings();
    for (int i = 1; i < argc; ++i) {
        rbrew_nfp::Tag tag;
        std::string error;
        require(rbrew_nfp::load_dump(argv[i], tag, error), "sample dump loads");
        std::printf("%s: uid", argv[i]);
        for (uint8_t b : tag.uid) std::printf(" %02X", b);
        std::printf(" id");
        for (uint8_t b : tag.id) std::printf(" %02X", b);
        std::printf(" writes %02X%02X\n", tag.write_counter[0], tag.write_counter[1]);
    }
    std::printf("nn_nfp: ok\n");
    return 0;
}

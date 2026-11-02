#include "nn_nfp.h"
#include "../nn_act/nn_act.h"
#include "../stubs/stubs_addrs.h"
#include "../../runtime/rbrew_diagnostics.h"
#include "../../runtime/splatoon_diagnostics.h"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Evidence: Gambit FUN_021B9BBC is the NFP worker loop. It polls GetNfpState
// every 100 ms: Initialized/Removed -> StartDetection, Found -> GetTagInfo +
// Mount, Mounted -> GetNfpCommonInfo (FUN_021B9B10 checks character 08 00
// and variant 1..3), GetNfpRegisterInfo (FUN_021B9BAC requires a nickname,
// else it asks for amiibo Settings), then StopDetection. Splatoon keeps its
// amiibo progress in its own save; it imports no application-area API.
// State transitions mirror Cemu's nn_nfp; layouts follow wut.

namespace rbrew_nfp {
namespace {

// Write and register date reported for every amiibo (Splatoon's release).
constexpr uint16_t kYear = 2015;
constexpr uint8_t kMonth = 5, kDay = 29;
constexpr uint16_t kApplicationAreaSize = 0xD8;
constexpr uint8_t kTagTypeType2 = 1 << 1;

std::mutex g_lock;
bool g_initialized = false;
bool g_detecting = false;
bool g_mounted = false;
std::optional<Tag> g_tag; // on the touchpoint
std::atomic<bool> g_search_started{false};
State g_reported = State::Uninitialized; // last state GetNfpState returned

// Each NFC step goes to the diagnostics event log (events.jsonl in every
// Game/Diagnostics snapshot), so a stuck scan shows how far it got.
void note(int16_t code, const char* message) {
    SplatoonDiagnostics::RecordEvent(SplatoonDiagnostics::EventCategory::Input,
                                     SplatoonDiagnostics::EventSeverity::Info, code, message);
}

void put_be16(uint8_t* out, uint16_t value) {
    out[0] = uint8_t(value >> 8);
    out[1] = uint8_t(value);
}

void put_date(uint8_t* out) {
    put_be16(out, kYear);
    out[2] = kMonth;
    out[3] = kDay;
}

State state_locked() {
    if (!g_initialized) return State::Uninitialized;
    if (g_mounted && g_tag) return State::Mounted;
    if (g_detecting) return g_tag ? State::Found : State::Searching;
    return State::Initialized;
}

void require_guest(const CPUState* cpu, uint32_t address, uint32_t length, const char* what) {
    if (!address || uint64_t(address) + length > cpu->guest_memory_size ||
        !rbrew_memory_accessible(cpu->mem, address, length))
        throw std::runtime_error(std::string("nn::nfp ") + what + " is outside accessible guest memory");
}

template <size_t N>
void write_guest(CPUState* cpu, uint32_t address, const std::array<uint8_t, N>& data, const char* what) {
    require_guest(cpu, address, N, what);
    for (uint32_t i = 0; i < N; ++i) rbrew_write8(cpu->mem, address + i, data[i]);
}

void Initialize(CPUState* cpu) {
    std::lock_guard lock(g_lock);
    g_initialized = true;
    g_detecting = g_mounted = false;
    g_tag.reset();
    note(1, "nfp Initialize");
    cpu->r[3] = 0;
}

void GetNfpState(CPUState* cpu) {
    std::lock_guard lock(g_lock);
    const State now = state_locked();
    if (now != g_reported) {
        char message[32];
        std::snprintf(message, sizeof(message), "nfp state %u", unsigned(now));
        note(8, message);
        g_reported = now;
    }
    cpu->r[3] = uint32_t(now);
}

void StartDetection(CPUState* cpu) {
    std::lock_guard lock(g_lock);
    if (!g_initialized) {
        cpu->r[3] = kResultInvalidState;
        return;
    }
    g_detecting = true;
    g_mounted = false;
    g_tag.reset();
    g_search_started.store(true, std::memory_order_relaxed);
    note(2, "nfp StartDetection");
    cpu->r[3] = 0;
}

// The figure leaves the touchpoint with the detection session: Gambit stops
// detection once it has read a figure, and the next scan must ask again.
void StopDetection(CPUState* cpu) {
    std::lock_guard lock(g_lock);
    note(3, "nfp StopDetection");
    g_detecting = g_mounted = false;
    g_tag.reset();
    cpu->r[3] = 0;
}

void GetTagInfo(CPUState* cpu) {
    std::lock_guard lock(g_lock);
    if (!g_tag) {
        cpu->r[3] = kResultInvalidTag;
        return;
    }
    write_guest(cpu, cpu->r[3], tag_info(*g_tag), "TagInfo output");
    cpu->r[3] = 0;
}

void Mount(CPUState* cpu) {
    std::lock_guard lock(g_lock);
    if (!g_detecting || !g_tag) {
        cpu->r[3] = kResultInvalidTag;
        return;
    }
    g_mounted = true;
    note(4, "nfp Mount");
    cpu->r[3] = 0;
}

void GetNfpCommonInfo(CPUState* cpu) {
    std::lock_guard lock(g_lock);
    if (state_locked() != State::Mounted) {
        cpu->r[3] = kResultInvalidState;
        return;
    }
    write_guest(cpu, cpu->r[3], common_info(*g_tag), "CommonInfo output");
    note(5, "nfp GetNfpCommonInfo");
    cpu->r[3] = 0;
}

void GetNfpRegisterInfo(CPUState* cpu) {
    std::lock_guard lock(g_lock);
    if (state_locked() != State::Mounted) {
        cpu->r[3] = kResultInvalidState;
        return;
    }
    write_guest(cpu, cpu->r[3], register_info(*g_tag), "RegisterInfo output");
    note(6, "nfp GetNfpRegisterInfo");
    cpu->r[3] = 0;
}

// GetErrorCode(const nn::Result&): r3 points at the result.
void GetErrorCode(CPUState* cpu) {
    require_guest(cpu, cpu->r[3], 4, "Result input");
    cpu->r[3] = error_code(rbrew_read32(cpu->mem, cpu->r[3]));
}

void InitializeAmiiboSettingsArgsIn(CPUState* cpu) {
    write_guest(cpu, cpu->r[3], std::array<uint8_t, kSettingsArgsInSize>{}, "AmiiboSettingsArgsIn");
    cpu->r[3] = 0;
}

// The port has no amiibo Settings applet to switch to. Every placed amiibo
// is registered, so Gambit only asks for it on paths the picker never
// produces; failing lets the game show its own error instead of exiting.
void SwitchToAmiiboSettings(CPUState* cpu) {
    note(9, "nfp SwitchToAmiiboSettings");
    cpu->r[3] = kResultSystemError;
}

} // namespace

bool parse_dump(std::span<const uint8_t> data, std::u16string_view nickname, Tag& tag, std::string& error) {
    auto is = [&](size_t at, std::initializer_list<uint8_t> bytes) {
        return std::equal(bytes.begin(), bytes.end(), data.begin() + at);
    };
    if (data.size() < kDumpMinSize) {
        error = "not an amiibo dump (an NTAG215 .bin is 540 bytes)";
        return false;
    }
    // Capability container, static lock and configuration pages of an
    // NTAG215 amiibo (Cemu's checks; the dynamic lock is not checked there).
    if (!is(0x0C, {0xF1, 0x10, 0xFF, 0xEE}) || !is(0x0A, {0x0F, 0xE0}) || !is(0x20C, {0x00, 0x00, 0x00, 0x04}) ||
        !is(0x210, {0x5F, 0x00, 0x00, 0x00})) {
        error = "not an NTAG215 amiibo dump (invalid lock or config pages)";
        return false;
    }
    if (nickname.empty() || nickname.size() > kNicknameMax) {
        error = "nickname must be 1..10 characters";
        return false;
    }
    // UID: serial bytes 0-2 and 4-7 (byte 3 is the BCC0 check byte).
    tag.uid = {data[0], data[1], data[2], data[4], data[5], data[6], data[7]};
    std::copy_n(data.begin() + 0x54, tag.id.size(), tag.id.begin());
    tag.write_counter = {data[0x11], data[0x12]};
    tag.nickname = nickname;
    return true;
}

bool load_dump(const std::filesystem::path& path, Tag& tag, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open the file";
        return false;
    }
    std::vector<uint8_t> data(1024);
    file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
    data.resize(size_t(file.gcount()));
    // Nickname: the file name, as the amiibo Settings nickname would be.
    std::u16string nickname;
    for (char16_t c : path.stem().u16string())
        if (nickname.size() < kNicknameMax) nickname.push_back(c);
    if (nickname.empty()) nickname = u"amiibo";
    return parse_dump(data, nickname, tag, error);
}

std::array<uint8_t, kTagInfoSize> tag_info(const Tag& tag) {
    std::array<uint8_t, kTagInfoSize> out{};
    out[0] = uint8_t(tag.uid.size());
    std::copy(tag.uid.begin(), tag.uid.end(), out.begin() + 1);
    out[0x21] = kTagTypeType2;
    return out;
}

std::array<uint8_t, kCommonInfoSize> common_info(const Tag& tag) {
    std::array<uint8_t, kCommonInfoSize> out{};
    put_date(&out[0x00]);
    out[0x04] = tag.write_counter[0];
    out[0x05] = tag.write_counter[1];
    out[0x06] = tag.id[0];
    out[0x07] = tag.id[1];
    out[0x08] = tag.id[2];
    out[0x09] = tag.id[6];
    out[0x0A] = tag.id[4];
    out[0x0B] = tag.id[5];
    out[0x0C] = tag.id[3];
    put_be16(&out[0x0E], kApplicationAreaSize);
    return out;
}

std::array<uint8_t, kRegisterInfoSize> register_info(const Tag& tag) {
    std::array<uint8_t, kRegisterInfoSize> out{};
    // Owner: the console's offline account Mii (nn_act). Built once: its CRC
    // search took ~140 ms and stalled the game in the middle of a scan.
    static const auto mii = rbrew_act_default_mii(u"default");
    std::copy(mii.begin(), mii.end(), out.begin());
    const std::u16string_view name(tag.nickname);
    if (name.empty() || name.size() > kNicknameMax) throw std::invalid_argument("amiibo nickname must be 1..10 units");
    for (size_t i = 0; i < name.size(); ++i) put_be16(&out[0x60 + i * 2], uint16_t(name[i]));
    put_date(&out[0x78]);
    return out;
}

uint32_t error_code(uint32_t result) {
    const uint32_t level = (result >> 27) & 3;
    uint32_t mask = level == 3 ? 0x7F00000u : 0x1FF00000u;
    if ((result & mask) == 0x1B00000u && (result & 0x80000000u)) {
        mask = level == 3 ? 0x3FFu : 0xFFFFFu;
        return ((result & mask) >> 7) + 1680000;
    }
    return 1680000;
}

State state() {
    std::lock_guard lock(g_lock);
    return state_locked();
}

bool searching() {
    return state() == State::Searching;
}

bool consume_search_started() {
    return g_search_started.load(std::memory_order_relaxed) &&
           g_search_started.exchange(false, std::memory_order_relaxed);
}

bool touch(const Tag& tag) {
    std::lock_guard lock(g_lock);
    if (state_locked() != State::Searching) {
        note(7, "nfp touch refused");
        return false;
    }
    g_tag = tag;
    char message[48];
    std::snprintf(message, sizeof(message), "nfp touch %02X%02X%02X%02X %02X%02X%02X%02X", tag.id[0], tag.id[1],
                  tag.id[2], tag.id[3], tag.id[4], tag.id[5], tag.id[6], tag.id[7]);
    note(7, message);
    return true;
}

void reset_for_tests() {
    std::lock_guard lock(g_lock);
    g_initialized = g_detecting = g_mounted = false;
    g_tag.reset();
    g_search_started = false;
    g_reported = State::Uninitialized;
}

} // namespace rbrew_nfp

#define NFP_REG(fn, mangled) rbrew_register_func(cpu, ADDR_##mangled, rbrew_nfp::fn)
void nn_nfp_register(CPUState* cpu) {
    NFP_REG(Initialize, Initialize__Q2_2nn3nfpFv);
    NFP_REG(GetNfpState, GetNfpState__Q2_2nn3nfpFv);
    NFP_REG(StartDetection, StartDetection__Q2_2nn3nfpFv);
    NFP_REG(StopDetection, StopDetection__Q2_2nn3nfpFv);
    NFP_REG(GetTagInfo, GetTagInfo__Q2_2nn3nfpFPQ3_2nn3nfp7TagInfo);
    NFP_REG(Mount, Mount__Q2_2nn3nfpFv);
    NFP_REG(GetNfpCommonInfo, GetNfpCommonInfo__Q2_2nn3nfpFPQ3_2nn3nfp10CommonInfo);
    NFP_REG(GetNfpRegisterInfo, GetNfpRegisterInfo__Q2_2nn3nfpFPQ3_2nn3nfp12RegisterInfo);
    NFP_REG(GetErrorCode, GetErrorCode__Q2_2nn3nfpFRCQ2_2nn6Result);
    NFP_REG(InitializeAmiiboSettingsArgsIn,
            InitializeAmiiboSettingsArgsIn__Q2_2nn3nfpFPQ3_2nn3nfp20AmiiboSettingsArgsIn);
    NFP_REG(SwitchToAmiiboSettings, SwitchToAmiiboSettings__Q2_2nn3nfpFRCQ3_2nn3nfp20AmiiboSettingsArgsInPCcUi);
}
#undef NFP_REG

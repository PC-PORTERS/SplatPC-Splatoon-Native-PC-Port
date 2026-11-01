#pragma once
#include "rebrewu_runtime.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

// Native nn::nfp provider: an emulated GamePad NFC touchpoint. The game drives
// it exactly as on hardware (Initialize, StartDetection, poll GetNfpState,
// GetTagInfo, Mount, GetNfpCommonInfo, GetNfpRegisterInfo, StopDetection);
// the PC picker places an amiibo dump (NTAG215 .bin) on the touchpoint with
// rbrew_nfp::touch(). Structure layouts follow wut (nn/nfp/nfp_cpp.h,
// amiibo_settings_cpp.h); the dump layout and checks follow Cemu's nn_nfp.
namespace rbrew_nfp {

// nn::nfp::NfpState
enum class State : uint32_t { Uninitialized = 0, Initialized = 1, Searching = 2, Found = 3, Removed = 4, Mounted = 5 };

// Failures carry the nn_nfp module (0x1B) at status level; the low bits are
// wut's RESULT_* descriptions. Gambit only tests the sign bit.
constexpr uint32_t kResultBase = 0xA1B00000u;
constexpr uint32_t kResultInvalidState = kResultBase | 0x06400u;
constexpr uint32_t kResultInvalidTag = kResultBase | 0x0C800u;
constexpr uint32_t kResultSystemError = kResultBase | 0x3E880u;

constexpr uint32_t kTagInfoSize = 0x54;
constexpr uint32_t kCommonInfoSize = 0x40;
constexpr uint32_t kRegisterInfoSize = 0xA8;
constexpr uint32_t kSettingsArgsInSize = 0x164;
constexpr uint32_t kNicknameMax = 10;
constexpr size_t kDumpMinSize = 532; // NTAG215 pages 0..0x84 (540/572-byte dumps add signature/password)

// The unencrypted parts of an amiibo dump that nn::nfp reports. The owner,
// nickname and dates live in the encrypted settings block (it needs the
// console's amiibo keys), so the owner is the console account and the
// nickname comes from the file name.
struct Tag {
    std::array<uint8_t, 7> uid{};
    // Identification block (0x54): character id (3), figure type, model
    // number (big-endian), series, format version.
    std::array<uint8_t, 8> id{};
    std::array<uint8_t, 2> write_counter{}; // 0x11, big-endian
    std::u16string nickname;                // 1..kNicknameMax units
};

// Validates an NTAG215 amiibo dump; on failure returns false with `error` set.
bool parse_dump(std::span<const uint8_t> data, std::u16string_view nickname, Tag& tag, std::string& error);
// Reads `path` and parses it (nickname: the file name, cut to kNicknameMax).
bool load_dump(const std::filesystem::path& path, Tag& tag, std::string& error);

// Encoders for the guest structures (big-endian), exposed for tests.
std::array<uint8_t, kTagInfoSize> tag_info(const Tag& tag);
std::array<uint8_t, kCommonInfoSize> common_info(const Tag& tag);
std::array<uint8_t, kRegisterInfoSize> register_info(const Tag& tag);
// nn::nfp::GetErrorCode for a result value (Cemu's mapping, 168xxxx codes).
uint32_t error_code(uint32_t result);

// State as GetNfpState reports it.
State state();
// True while the game waits for an amiibo and none is on the touchpoint.
bool searching();
// True once for each StartDetection (any thread): the picker opens on it.
bool consume_search_started();
// Places `tag` on the touchpoint; false unless the game is searching.
bool touch(const Tag& tag);

// Test hook: back to the boot state.
void reset_for_tests();

} // namespace rbrew_nfp

void nn_nfp_register(CPUState* cpu);

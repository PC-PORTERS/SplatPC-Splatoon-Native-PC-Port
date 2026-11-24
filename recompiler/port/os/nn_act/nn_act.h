#pragma once
#include "rebrewu_runtime.h"
#include <array>
#include <cstdint>
#include <string_view>

// Native nn::act provider for one offline console account in slot 1.
// Behavior follows the selected local reference profile (Cemu's default
// offline account): no NNID, principal/simple-address id 0, country "NN",
// and the deterministic default Mii. Revision 288 also has a deterministic
// local UUID. This provider does not import a console identity or account.
constexpr uint32_t RBREW_ACT_SLOT_COUNT = 12;
constexpr uint8_t RBREW_ACT_SLOT_CURRENT = 0xFE;
constexpr uint32_t RBREW_ACT_RESULT_ACCOUNT_DOES_NOT_EXIST = 0xA071F480u;
// Failure result for NEX token requests on the offline account. Same ACT
// module/level as above; the description value is the port's choice, Gambit
// (02B32E5C) only tests the sign bit.
constexpr uint32_t RBREW_ACT_RESULT_NEX_UNAVAILABLE = 0xA071F500u;
constexpr uint32_t RBREW_ACT_MII_NAME_MAX = 10;

using RbrewFflStoreData = std::array<uint8_t, 0x60>;

// FFL store-data CRC16 (CCITT polynomial 0x1021, zero seed, CRC appended).
// A valid record yields 0 when the trailing CRC is included.
uint16_t rbrew_ffl_crc16(const uint8_t* data, size_t length);
// Default Mii as generated for a new offline account. Throws for an empty
// name or one longer than RBREW_ACT_MII_NAME_MAX UTF-16 units.
RbrewFflStoreData rbrew_act_default_mii(std::u16string_view name);

void nn_act_register(CPUState* cpu);
// Test hook: forget Initialize/Finalize balance.
void rbrew_act_reset_for_tests();

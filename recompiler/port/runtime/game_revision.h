#pragma once
#include <stdint.h>
#ifndef RBREW_GAME_REVISION
#define RBREW_GAME_REVISION 16
#endif
// Generated from the manifest by recompiler/tools/generate_revision_header.py. Address metadata only.
constexpr uint32_t rbrew_game_address(uint32_t address) {
    return address;
}
constexpr uint32_t RBREW_GAME_ALLOCATOR = 0x101D2554u;
constexpr uint32_t RBREW_GAME_SDA = 0x10256C80u;
constexpr uint32_t RBREW_GAME_CTORS = 0x101BFA54u;
constexpr uint32_t RBREW_GAME_CTORS_DONE = 0x101CC020u;
constexpr uint32_t RBREW_GAME_TLS_HANDLE = 0x101C98F0u;
constexpr uint32_t RBREW_GAME_PLT_BEGIN = 0x02D00000u;
constexpr uint32_t RBREW_GAME_PLT_END = 0x02D10000u;
constexpr uint32_t RBREW_GAME_HEAP_ALLOC_SLOT = 0x02D0B8F8u;
constexpr uint32_t RBREW_GAME_HEAP_ALLOC_EX_SLOT = 0x02D0B900u;
constexpr uint32_t RBREW_GAME_HEAP_FREE_SLOT = 0x02D0B908u;
constexpr uint32_t RBREW_GAME_FOPEN_MAX_SLOT = 0x02D0B948u;
constexpr uint32_t RBREW_GAME_BLANK_STUB = 0x02D051A0u;

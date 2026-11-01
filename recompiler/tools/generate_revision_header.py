"""Regenerate the public address-only revision header from its reviewed profile."""
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def render(profile):
    assert profile['schema']==1 and profile['title_version']==288
    pairs=sorted((int(a,16),int(b,16)) for a,b in profile['addresses'].items())
    lines=['#pragma once','#include <stdint.h>','#ifndef RBREW_GAME_REVISION','#define RBREW_GAME_REVISION 16','#endif','// Address metadata only; no game instructions or data.','constexpr uint32_t rbrew_game_address(uint32_t address) {','#if RBREW_GAME_REVISION == 288','    switch (address) {']
    lines += [f'    case 0x{a:08X}u: return 0x{b:08X}u;' for a,b in pairs]
    lines += ['    default: return address;','    }','#else','    return address;','#endif','}','#if RBREW_GAME_REVISION == 288',f'constexpr uint32_t RBREW_GAME_ALLOCATOR = 0x{profile["allocator"]:08X}u;',f'constexpr uint32_t RBREW_GAME_SDA = 0x{profile["sda_base"]:08X}u;',f'constexpr uint32_t RBREW_GAME_CTORS = 0x{profile["constructor_table"]:08X}u;',f'constexpr uint32_t RBREW_GAME_CTORS_DONE = 0x{profile["constructors_done"]:08X}u;',f'constexpr uint32_t RBREW_GAME_TLS_HANDLE = 0x{profile["tls_allocator_handle"]:08X}u;','constexpr uint32_t RBREW_GAME_PLT_BEGIN = 0x02DD0000u;','constexpr uint32_t RBREW_GAME_PLT_END = 0x02DE0000u;','#else','constexpr uint32_t RBREW_GAME_ALLOCATOR = 0x101D2554u;','constexpr uint32_t RBREW_GAME_SDA = 0x10256C80u;','constexpr uint32_t RBREW_GAME_CTORS = 0x101BFA54u;','constexpr uint32_t RBREW_GAME_CTORS_DONE = 0x101CC020u;','constexpr uint32_t RBREW_GAME_TLS_HANDLE = 0x101C98F0u;','constexpr uint32_t RBREW_GAME_PLT_BEGIN = 0x02D00000u;','constexpr uint32_t RBREW_GAME_PLT_END = 0x02D10000u;','#endif','']
    return '\n'.join(lines)
if __name__=='__main__':
    profile=json.loads((ROOT/'profiles/agmp-v288.json').read_text())
    (ROOT/'recompiler/port/runtime/game_revision.h').write_text(render(profile))

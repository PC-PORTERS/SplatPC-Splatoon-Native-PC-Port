// branch_tables.cpp - branch-table jumps of Splatoon v288 .
//
// Three game functions (022302D8, 02B32454, 0228BD98) pick a case with `bctr` into a table of `b`
// instructions. RebrewU translated only the first slot, so any other case dispatched to an
// unregistered address; the dispatcher returned silently and the function left without popping its
// frame or restoring r30: stack and registers were wrong afterwards (freezes and crashes).
// The case blocks are generated with `rebrewu --hints-file profiles/agmp-v288.tables` (their entries
// are listed there) and registered by gambit_tables_register(); this file points each table slot at
// the block it branches to. Address metadata only. Before the first generation that includes the
// hints, gambit_tables_register falls back to a no-op and the slots stay unregistered, as before.
#include "../../runtime/rebrewu_runtime.h"
#include "rebrewu/logger.hpp"
#include "../vpad/vpad_addrs.h"
#include <cstdio>
#include <vector>

// The PLT headers are emitted into the source tree from the manifest of the build tree configured
// last. Building one revision against the other image's map would register every import at the
// wrong address; stop the build instead.
static_assert(ADDR_VPADRead == rbrew_game_address(0x02D08048u),
              "PLT headers were generated for another game revision: reconfigure this build tree");

extern "C" void gambit_tables_register(CPUState* cpu);
extern "C" void gambit_tables_register_none(CPUState*) {}
#pragma comment(linker, "/alternatename:gambit_tables_register=gambit_tables_register_none")

namespace {
struct BranchTable { uint32_t base; unsigned count; std::vector<uint32_t> targets; };
const BranchTable kTables[] = {
    {0x02B32480u, 22, {0x02B324D8u, 0x02B32508u, 0x02B32538u, 0x02B32568u, 0x02B32538u, 0x02B32688u, 0x02B32688u, 0x02B32688u, 0x02B32688u, 0x02B32688u, 0x02B32688u, 0x02B32598u, 0x02B325C8u, 0x02B32598u, 0x02B325C8u, 0x02B32598u, 0x02B325C8u, 0x02B325F8u, 0x02B32628u, 0x02B325F8u, 0x02B32628u, 0x02B32658u}},   // jump table of 02B32454
    {0x02230378u, 106, {0x022308B0u, 0x022308B0u, 0x0223078Cu, 0x022308B0u, 0x022308B0u, 0x0223078Cu, 0x022308B0u, 0x022308B0u, 0x022308B0u, 0x0223078Cu, 0x0223078Cu, 0x022308B0u, 0x0223078Cu, 0x022308B0u, 0x0223078Cu, 0x0223078Cu, 0x0223078Cu, 0x0223078Cu, 0x02230B54u, 0x02230B54u, 0x022308B0u, 0x022308B0u, 0x022308B0u, 0x022308B0u, 0x02230B54u, 0x02230B54u, 0x02230B54u, 0x02230B54u, 0x022308B0u, 0x022308B0u, 0x022305A0u, 0x02230600u, 0x022305A0u, 0x022305A0u, 0x02230600u, 0x02230600u, 0x022305A0u, 0x02230B54u, 0x02230660u, 0x02230660u, 0x02230660u, 0x0223072Cu, 0x0223072Cu, 0x0223072Cu, 0x0223072Cu, 0x0223072Cu, 0x0223072Cu, 0x02230B54u, 0x022308B0u, 0x022308B0u, 0x022308B0u, 0x022308B0u, 0x022308B0u, 0x022306C0u, 0x022306C0u, 0x022306C0u, 0x02230B54u, 0x02230B54u, 0x02230B54u, 0x02230B54u, 0x02230B54u, 0x022306C0u, 0x0223072Cu, 0x0223072Cu, 0x0223072Cu, 0x0223078Cu, 0x0223078Cu, 0x0223078Cu, 0x02230DE0u, 0x02230DE0u, 0x0223072Cu, 0x02230DE0u, 0x02230DE0u, 0x02230DE0u, 0x02230DE0u, 0x02230DE0u, 0x02230B54u, 0x02230B54u, 0x02230DE0u, 0x02230DE0u, 0x022307F8u, 0x022307F8u, 0x02230858u, 0x022307F8u, 0x02230DE0u, 0x02230DE0u, 0x022308B0u, 0x022308B0u, 0x02230A8Cu, 0x02230AE8u, 0x02230AE8u, 0x02230B48u, 0x02230DE0u, 0x02230DE0u, 0x02230DE0u, 0x02230DE0u, 0x02230DE0u, 0x02230D98u, 0x02230914u, 0x02230914u, 0x02230B54u, 0x02230970u, 0x02230DE0u, 0x02230970u, 0x02230A30u, 0x022309D0u}},   // jump table of 022302D8
    {0x0228BDF4u, 32, {0x0228BF28u, 0x0228BF14u, 0x0228BF28u, 0x0228BF28u, 0x0228BF14u, 0x0228BF14u, 0x0228BF28u, 0x0228BF30u, 0x0228BF14u, 0x0228BF14u, 0x0228BF14u, 0x0228BF28u, 0x0228BF14u, 0x0228BF28u, 0x0228BF28u, 0x0228BF14u, 0x0228BF14u, 0x0228BF28u, 0x0228BF30u, 0x0228BEFCu, 0x0228BEFCu, 0x0228BEFCu, 0x0228BF28u, 0x0228BEFCu, 0x0228BEFCu, 0x0228BEFCu, 0x0228BF30u, 0x0228BEFCu, 0x0228BEFCu, 0x0228BEFCu, 0x0228BF30u, 0x0228BF14u}},   // jump table of 0228BD98
};
}

void branch_tables_install(CPUState* cpu) {
#if RBREW_GAME_REVISION == 288
    gambit_tables_register(cpu);
    unsigned linked = 0;
    for (const auto& table : kTables) {
        unsigned slot = 0;
        for (uint32_t target : table.targets) {
            const uint32_t from = table.base + 4u * slot++;
            const uint32_t from_index = from >> 2, to_index = target >> 2;
            if (from_index < cpu->dispatch_size && to_index < cpu->dispatch_size &&
                !cpu->dispatch_table[from_index] && cpu->dispatch_table[to_index]) {
                cpu->dispatch_table[from_index] = cpu->dispatch_table[to_index];
                ++linked;
            }
        }
    }
    RBREW_LOG_INFO("branch-tables", "%u table slots linked to their case blocks\n", linked);
#else
    (void)cpu;
#endif
}

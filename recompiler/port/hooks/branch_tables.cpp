// branch_tables.cpp - branch-table jumps of Splatoon v288.
//
// Three game functions (022302D8, 02B32454, 0228BD98) pick a case with `bctr` into a table of `b`
// instructions. RebrewU translated only the first slot, so any other case dispatched to an
// unregistered address; the dispatcher returned silently and the function left without popping its
// frame or restoring r30: stack and registers were wrong afterwards (freezes and crashes).
// The case blocks are generated from the manifest `[entries]` section (`force` entries) and registered
// by gambit_tables_register(); this file points each table slot at the block it branches to, from the
// manifest `[branch_tables]` section (generated header game_branch_tables.h).
#include "../runtime/rebrewu_runtime.h"
#include "../os/vpad/vpad_addrs.h"
#include "../runtime/game_branch_tables.h"
#include <cstdio>
#include <vector>

// The PLT headers are emitted into the source tree from the manifest of the build tree configured
// last. Building one revision against the other image's map would register every import at the
// wrong address; stop the build instead.
static_assert(ADDR_VPADRead >= RBREW_GAME_PLT_BEGIN && ADDR_VPADRead < RBREW_GAME_PLT_END,
              "PLT headers were generated for another game revision: reconfigure this build tree");

#if defined(_MSC_VER)
extern "C" void gambit_tables_register(CPUState* cpu);
extern "C" void gambit_tables_register_none(CPUState*) {}
#pragma comment(linker, "/alternatename:gambit_tables_register=gambit_tables_register_none")
#else
// GCC/Clang: a weak no-op that the generated definition overrides.
extern "C" __attribute__((weak)) void gambit_tables_register(CPUState*) {}
#endif

void branch_tables_install(CPUState* cpu) {
#if RBREW_GAME_REVISION == 288
    gambit_tables_register(cpu);
    unsigned linked = 0;
    for (unsigned index = 0; index < kRbrewBranchTableCount; ++index) {
        const RbrewBranchTable& table = kRbrewBranchTables[index];
        for (unsigned slot = 0; slot < table.count; ++slot) {
            const uint32_t target = table.targets[slot];
            const uint32_t from = table.base + 4u * slot;
            const uint32_t from_index = from >> 2, to_index = target >> 2;
            if (from_index < cpu->dispatch_size && to_index < cpu->dispatch_size && !cpu->dispatch_table[from_index] &&
                cpu->dispatch_table[to_index]) {
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

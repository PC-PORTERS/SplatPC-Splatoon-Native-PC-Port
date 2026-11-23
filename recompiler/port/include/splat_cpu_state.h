#pragma once

// Shared Espresso CPUState and dispatch callback types used by the RebrewU
// host runtime, generated code, and standalone C/C++ plugin mods.

#include <stdbool.h>
#include <stdint.h>

#ifndef SPLAT_DEFAULT_GUEST_MEMORY_SIZE
#define SPLAT_DEFAULT_GUEST_MEMORY_SIZE 0xFA000000u
#endif

struct CPUState;

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*HostFunc)(struct CPUState* cpu);

typedef struct RbrewGuestCodeRange {
    uint32_t begin, end;
} RbrewGuestCodeRange;

#ifdef __cplusplus
}
#endif

typedef struct CPUState {
    uint32_t r[32]; // GPRs
    double f[32];   // FPRs (double precision on Espresso)
    uint32_t cr[8]; // Condition Register fields CR0-CR7 (4 bits each, packed as uint32)
    uint32_t lr;    // Link Register
    uint32_t ctr;   // Count Register
    uint32_t xer;   // Integer Exception Register
#ifdef __cplusplus
    uint32_t fpscr{}; // Conversion status; other FP/control instructions remain partial
#else
    uint32_t fpscr;
#endif

    uint8_t* mem; // pointer to base of the flat guest memory arena
#ifdef __cplusplus
    uint32_t pc{};
    uint32_t guest_memory_size{SPLAT_DEFAULT_GUEST_MEMORY_SIZE};
    const RbrewGuestCodeRange* guest_code_ranges{};
    uint32_t guest_code_range_count{};
    bool ppc_fallback_enabled{};
    uint64_t ppc_instruction_budget{100000};
    bool differential_validation{};
#else
    uint32_t pc;
    uint32_t guest_memory_size;
    const RbrewGuestCodeRange* guest_code_ranges;
    uint32_t guest_code_range_count;
    bool ppc_fallback_enabled;
    uint64_t ppc_instruction_budget;
    bool differential_validation;
#endif

    // Host function dispatch table: guest address -> host implementation
    HostFunc* dispatch_table; // indexed by guest_addr >> 2
    uint32_t dispatch_size;   // number of entries

    // Scratch fields used by rbrew_dispatch / rbrew_call_indirect
    uint32_t dispatch_ret_addr;

    // One local reservation per emulated CPU; versions are shared by arena.
    uint8_t* reservation_mem;
    uint32_t reservation_addr;
    uint64_t reservation_version;
    bool reservation_valid;

    // Espresso paired singles: f[n] is ps0, ps1[n] the second slot.
#ifdef __cplusplus
    double ps1[32]{};
    uint32_t gqr[8]{};
#else
    double ps1[32];
    uint32_t gqr[8];
#endif
} CPUState;

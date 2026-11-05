#pragma once
#include "game_revision.h"
#include "rbrew_diagnostics.h"
// =============================================================================
// RebrewU Runtime — Wii U → PC
//
// Included by every decompiled Gambit_part*.cpp.  Defines the CPU state,
// memory accessors (big-endian ↔ host byte-swap), and dispatch helpers.
// =============================================================================

#include <stdint.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <atomic>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

#ifndef RBREW_HOST_API
#define RBREW_HOST_API
#endif

// ---------------------------------------------------------------------------
// Forward declaration
// ---------------------------------------------------------------------------
struct CPUState;

// ---------------------------------------------------------------------------
// Host function type
// ---------------------------------------------------------------------------
typedef void (*HostFunc)(CPUState* cpu);

// ---------------------------------------------------------------------------
// Memory size constants
// Wii U address space used by this game:
//   0x02000000 - 0x026143A5   .text (game code — executed natively on PC)
//   0x10000000 - 0x1098xxxx   .rodata
//   0x101C0BC0 - 0x101F0000   .data / .bss / OS structs
//   0xC0000000+               import stubs (handled by dispatch table)
// Reserve [0x00000000, WIIU_MEM_SIZE) as a flat virtual aperture. The native
// application arena is committed initially; Cafe maps the optional overlay.
// ---------------------------------------------------------------------------
#include "memory_layout.h"
#define WIIU_MEM_SIZE RBREW_ARENA_END

// The virtual aperture is sparse: its reserved hole and disabled overlay are
// not guest RAM. Ordinary low accesses stay inline without a registry lookup.
// Arena owners publish overlay accessibility only after the host map/unmap
// succeeds, and clear it before destroying or reusing the arena allocation.
extern "C" RBREW_HOST_API bool rbrew_overlay_accessible(const uint8_t* mem);
extern "C" void rbrew_set_overlay_access(uint8_t* mem, bool enabled);
#if defined(_MSC_VER)
static __declspec(noinline) bool rbrew_memory_accessible(const uint8_t* mem, uint32_t address, uint64_t size) {
#else
static inline bool rbrew_memory_accessible(const uint8_t* mem, uint32_t address, uint64_t size) {
#endif
    if (!mem || size > WIIU_MEM_SIZE) return false;
    const uint64_t end = uint64_t(address) + size; // widened, bounded size
    if (end <= RBREW_APPLICATION_ARENA_END) return true;
    if (address >= RBREW_SHARED_DATA_BASE) return end <= uint64_t(RBREW_SHARED_DATA_BASE) + RBREW_SHARED_DATA_SIZE;
    if (address >= RBREW_MEM1_BASE) return end <= uint64_t(RBREW_MEM1_BASE) + RBREW_MEM1_SIZE;
    if (address >= RBREW_FG_FREE_BASE) return end <= uint64_t(RBREW_FG_FREE_BASE) + RBREW_FG_FREE_SIZE;
    return address >= RBREW_OVERLAY_BASE && end <= uint64_t(RBREW_OVERLAY_BASE) + RBREW_OVERLAY_SIZE &&
           rbrew_overlay_accessible(mem);
}

// ---------------------------------------------------------------------------
// CPUState — mirrors PowerPC Espresso register file
// ---------------------------------------------------------------------------
struct RbrewGuestCodeRange {
    uint32_t begin, end;
}; // exclusive end

struct CPUState {
    uint32_t r[32];   // GPRs
    double f[32];     // FPRs (double precision on Espresso)
    uint32_t cr[8];   // Condition Register fields CR0–CR7 (4 bits each, packed as uint32)
    uint32_t lr;      // Link Register
    uint32_t ctr;     // Count Register
    uint32_t xer;     // Integer Exception Register
    uint32_t fpscr{}; // Conversion status; other FP/control instructions remain partial

    uint8_t* mem; // pointer to base of the flat guest memory arena
    // Hybrid execution is opt-in. PC is authoritative only inside the stepper;
    // legacy whole-function C++ does not yet expose every continuation.
    uint32_t pc{};
    uint32_t guest_memory_size{WIIU_MEM_SIZE};
    const RbrewGuestCodeRange* guest_code_ranges{};
    uint32_t guest_code_range_count{};
    bool ppc_fallback_enabled{};
    uint64_t ppc_instruction_budget{100000};
    bool differential_validation{}; // detached pure replay: reject host dispatch

    // Host function dispatch table: guest address → host implementation
    // Populated at startup for all OS import addresses.
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
    // GQR0-7 drive psq_l/psq_st quantization (SPR 912-919 / UGQR 896-903).
    double ps1[32]{};
    uint32_t gqr[8]{};
};

// Paired-single instruction (opcode 4/56/57/60/61, or mtspr/mfspr of a GQR)
// executed on the register file; implemented per decaf-emu's interpreter.
// The _mem form is what generated code calls: it passes the arena base
// explicitly so host compilers know a paired-single may touch guest memory.
extern "C" RBREW_HOST_API void rbrew_ps_mem(CPUState* cpu, uint8_t* mem, uint32_t word);
extern "C" RBREW_HOST_API void rbrew_ps(CPUState* cpu, uint32_t word);
// tw/twi: evaluate the TO condition (signed lt/gt, eq, unsigned lt/gt) and,
// when it holds, stop with a guest-trap error instead of continuing.
extern "C" RBREW_HOST_API void rbrew_trap(CPUState* cpu, uint32_t word, uint32_t pc);
// Thread start values set by coreinit (decaf OSCreateThread): GQR2-5 hold
// the u8/u16/s8/s16 quantization types, the rest are 0.
static inline void rbrew_init_thread_gqrs(CPUState* cpu) {
    for (auto& g : cpu->gqr) g = 0;
    cpu->gqr[2] = 0x40004u;
    cpu->gqr[3] = 0x50005u;
    cpu->gqr[4] = 0x60006u;
    cpu->gqr[5] = 0x70007u;
}

extern "C" RBREW_HOST_API uint32_t rbrew_lwarx_mem(CPUState* cpu, uint8_t* mem, uint32_t addr);
extern "C" RBREW_HOST_API void rbrew_stwcx_mem(CPUState* cpu, uint8_t* mem, uint32_t addr, uint32_t value);
extern "C" RBREW_HOST_API uint32_t rbrew_lwarx(CPUState* cpu, uint32_t addr);
extern "C" RBREW_HOST_API void rbrew_stwcx(CPUState* cpu, uint32_t addr, uint32_t value);
// Guest-memory writers use this shared monitor so even an ABA write breaks
// an outstanding reservation. bytes are already in guest endian order.
extern "C" RBREW_HOST_API void rbrew_write_memory(uint8_t* mem, uint32_t addr, const void* bytes, uint32_t size);
// A plain guest store needs rbrew_write_memory only while strict reservations
// are on or this thread has a write observer (rebrewu_memory_observer.h);
// the setters maintain both. Static TLS, constant-initialized.
extern RBREW_HOST_API std::atomic<bool> rbrew_strict_reservations_active;
#if defined(_MSC_VER)
extern __declspec(thread) bool rbrew_thread_write_observed;
#else
extern thread_local bool rbrew_thread_write_observed;
#endif

// FPRs carry non-floating bit payloads too (fctiwz -> stfiwx/stfd).
static inline uint32_t rbrew_fpr_low_word(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (uint32_t)bits;
}

static inline void rbrew_fctiwz(CPUState* cpu, double& destination, double source, bool record) {
    // PowerPC PEM 8-81 / 3-38: truncate regardless of RN, saturate invalid
    // conversions when VE is clear, and suppress the destination when VE is set.
    // MSR-controlled FP exception delivery is not modeled by this runtime.
    uint64_t source_bits;
    memcpy(&source_bits, &source, sizeof(source_bits));
    const bool nan = (source_bits & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull;
    const bool snan = nan && !(source_bits & 0x0008000000000000ull);
    const bool invalid = nan || source > 2147483647.0 || source < -2147483648.0;
    const uint32_t previous = cpu->fpscr;
    cpu->fpscr &= ~0x00060000u; // FR=0 (toward zero never increments), FI=0
    uint32_t word;
    if (invalid) {
        cpu->fpscr |= 0x00000100u;           // VXCVI, sticky
        if (snan) cpu->fpscr |= 0x01000000u; // VXSNAN, sticky
        word = !nan && source > 0.0 ? 0x7FFFFFFFu : 0x80000000u;
    } else {
        const double integral = trunc(source);
        word = (uint32_t)(int32_t)integral;                // range checked before host conversion
        if (integral != source) cpu->fpscr |= 0x02020000u; // XX sticky, FI
    }
    if (invalid && (previous & 0x80u)) {
        cpu->fpscr = (cpu->fpscr & ~0x0001F000u) | 0x00011000u; // QNaN FPRF
    } else {
        // High word is architecturally undefined; choose a deterministic tag.
        uint64_t payload = 0xFFF8000000000000ull | word;
        memcpy(&destination, &payload, sizeof(payload));
        // FPRF is undefined for a completed integer conversion; retain it.
    }
    // Recompute summary bits. FX is sticky on newly raised exception bits.
    if ((cpu->fpscr & ~previous) & 0x1FF80700u) cpu->fpscr |= 0x80000000u;
    cpu->fpscr &= ~0x60000000u;
    if (cpu->fpscr & 0x01F80700u) cpu->fpscr |= 0x20000000u;
    if (((cpu->fpscr & 0x20000000u) && (cpu->fpscr & 0x80u)) || ((cpu->fpscr & 0x10000000u) && (cpu->fpscr & 0x40u)) ||
        ((cpu->fpscr & 0x08000000u) && (cpu->fpscr & 0x20u)) || ((cpu->fpscr & 0x04000000u) && (cpu->fpscr & 0x10u)) ||
        ((cpu->fpscr & 0x02000000u) && (cpu->fpscr & 0x08u)))
        cpu->fpscr |= 0x40000000u;
    if (record) cpu->cr[1] = (cpu->fpscr >> 28) & 15u;
}

// ---------------------------------------------------------------------------
// Memory access — Wii U is big-endian, host may be little-endian
//
// Generated code calls one of these for every guest load and store, mostly
// from large functions where the compiler keeps them out of line. Each
// helper therefore stays small: a test for the application arena (below
// RBREW_APPLICATION_ARENA_END, where code, stacks and heaps live) and the
// access itself. Every other address takes the *_checked function, which is
// the original logic including the out-of-bounds behaviour. Plain stores
// copy directly; with a write observer on this thread or strict reservations
// they go through rbrew_write_memory as before.
//
// Guest memory is shared between the host threads that run guest code:
// every access must happen exactly once, in program order. The raw helpers
// therefore use volatile packed references — same single instruction as a
// memcpy, but the compiler may not cache, merge or hoist them (a guest spin
// loop must see another thread's store).
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct RbrewRaw16 {
    uint16_t v;
};
struct RbrewRaw32 {
    uint32_t v;
};
struct RbrewRaw64 {
    uint64_t v;
};
#pragma pack(pop)

static inline uint8_t rbrew_load8_raw(const uint8_t* mem, uint32_t addr) {
    return *reinterpret_cast<const volatile uint8_t*>(mem + addr);
}
static inline uint16_t rbrew_load16_raw(const uint8_t* mem, uint32_t addr) {
    return reinterpret_cast<const volatile RbrewRaw16*>(mem + addr)->v;
}
static inline uint32_t rbrew_load32_raw(const uint8_t* mem, uint32_t addr) {
    return reinterpret_cast<const volatile RbrewRaw32*>(mem + addr)->v;
}
static inline uint64_t rbrew_load64_raw(const uint8_t* mem, uint32_t addr) {
    return reinterpret_cast<const volatile RbrewRaw64*>(mem + addr)->v;
}
static inline void rbrew_store8_raw(uint8_t* mem, uint32_t addr, uint8_t v) {
    *reinterpret_cast<volatile uint8_t*>(mem + addr) = v;
}
static inline void rbrew_store16_raw(uint8_t* mem, uint32_t addr, uint16_t v) {
    reinterpret_cast<volatile RbrewRaw16*>(mem + addr)->v = v;
}
static inline void rbrew_store32_raw(uint8_t* mem, uint32_t addr, uint32_t v) {
    reinterpret_cast<volatile RbrewRaw32*>(mem + addr)->v = v;
}
static inline void rbrew_store64_raw(uint8_t* mem, uint32_t addr, uint64_t v) {
    reinterpret_cast<volatile RbrewRaw64*>(mem + addr)->v = v;
}

#if defined(_MSC_VER)
#define RBREW_NOINLINE __declspec(noinline)
#else
#define RBREW_NOINLINE __attribute__((noinline))
#endif

static inline uint16_t rbrew_bswap16(uint16_t v) {
#if defined(_MSC_VER)
    return _byteswap_ushort(v);
#else
    return (uint16_t)((v << 8) | (v >> 8));
#endif
}
static inline uint32_t rbrew_bswap32(uint32_t v) {
#if defined(_MSC_VER)
    return _byteswap_ulong(v);
#else
    return __builtin_bswap32(v);
#endif
}
static inline uint64_t rbrew_bswap64(uint64_t v) {
#if defined(_MSC_VER)
    return _byteswap_uint64(v);
#else
    return __builtin_bswap64(v);
#endif
}
// rbrew_memory_accessible's first case: [addr, addr + size) in the arena.
// HYPER_OPTIMIZE: the host aperture covers the whole 32-bit guest address
// space (plus access-size slop) and is committed at startup, so every guest
// effective address is a plain offset with no translation and no check.
static inline bool rbrew_arena_access(const uint8_t* mem, uint32_t addr, uint32_t size) {
#if defined(RBREW_HYPER_OPTIMIZE)
    (void)addr;
    (void)size;
    return mem != nullptr;
#else
    return mem != nullptr && addr <= RBREW_APPLICATION_ARENA_END - size;
#endif
}

static RBREW_NOINLINE uint8_t rbrew_read8_checked(uint8_t* mem, uint32_t addr) {
    if (!rbrew_memory_accessible(mem, addr, 1)) {
        RBREW_LOG_INFO("rbrew", "OOB read8  addr=0x%08X\n", addr);
        return 0;
    }
    return rbrew_load8_raw(mem, addr);
}
static RBREW_NOINLINE uint16_t rbrew_read16_checked(uint8_t* mem, uint32_t addr) {
    if (!rbrew_memory_accessible(mem, addr, 2)) {
        RBREW_LOG_INFO("rbrew", "OOB read16 addr=0x%08X\n", addr);
        return 0;
    }
    return rbrew_bswap16(rbrew_load16_raw(mem, addr));
}
static RBREW_NOINLINE uint32_t rbrew_read32_checked(uint8_t* mem, uint32_t addr) {
    if (!rbrew_memory_accessible(mem, addr, 4)) {
        RBREW_LOG_INFO("rbrew", "OOB read32 addr=0x%08X\n", addr);
        return 0;
    }
    return rbrew_bswap32(rbrew_load32_raw(mem, addr));
}
static RBREW_NOINLINE uint64_t rbrew_read64_checked(uint8_t* mem, uint32_t addr) {
    if (!rbrew_memory_accessible(mem, addr, 8)) {
        RBREW_LOG_INFO("rbrew", "OOB read64 addr=0x%08X\n", addr);
        return 0;
    }
    return rbrew_bswap64(rbrew_load64_raw(mem, addr));
}

static inline uint8_t rbrew_read8(uint8_t* __restrict mem, uint32_t addr) {
    if (rbrew_arena_access(mem, addr, 1)) return rbrew_load8_raw(mem, addr);
    return rbrew_read8_checked(mem, addr);
}
static inline uint16_t rbrew_read16(uint8_t* __restrict mem, uint32_t addr) {
    if (rbrew_arena_access(mem, addr, 2)) return rbrew_bswap16(rbrew_load16_raw(mem, addr));
    return rbrew_read16_checked(mem, addr);
}
static inline uint32_t rbrew_read32(uint8_t* __restrict mem, uint32_t addr) {
    if (rbrew_arena_access(mem, addr, 4)) return rbrew_bswap32(rbrew_load32_raw(mem, addr));
    return rbrew_read32_checked(mem, addr);
}
static inline uint64_t rbrew_read64(uint8_t* __restrict mem, uint32_t addr) {
    if (rbrew_arena_access(mem, addr, 8)) return rbrew_bswap64(rbrew_load64_raw(mem, addr));
    return rbrew_read64_checked(mem, addr);
}
static inline double rbrew_read_f32(uint8_t* __restrict mem, uint32_t addr) {
    uint32_t bits = rbrew_read32(mem, addr);
    float f;
    memcpy(&f, &bits, 4);
    return (double)f;
}
static inline double rbrew_read_f64(uint8_t* __restrict mem, uint32_t addr) {
    uint64_t bits = rbrew_read64(mem, addr);
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

// Byte-reversed loads (lhbrx/lwbrx): on a little-endian host these are plain
// host-order copies — no bswap on top of the copy.
static inline uint16_t rbrew_read16_rev(uint8_t* __restrict mem, uint32_t addr) {
    return rbrew_load16_raw(mem, addr);
}
static inline uint32_t rbrew_read32_rev(uint8_t* __restrict mem, uint32_t addr) {
    return rbrew_load32_raw(mem, addr);
}

// dcbz: zero one 32-byte cache line.
static inline void rbrew_zero32(uint8_t* __restrict mem, uint32_t addr) {
    if (rbrew_arena_access(mem, addr, 32)) {
        for (uint32_t off = 0; off < 32; off += 4) rbrew_store32_raw(mem, addr + off, 0);
        return;
    }
    uint8_t zero[32] = {};
    rbrew_write_memory(mem, addr, zero, 32);
}

// Stores in guest byte order (bytes already swapped).
static inline void rbrew_store_bytes(uint8_t* __restrict mem, uint32_t addr, const void* bytes, uint32_t size) {
#if defined(RBREW_HYPER_OPTIMIZE)
    // Hot stores copy straight through: write observers and strict
    // reservation tracking are diagnostics and use rbrew_write_memory.
    switch (size) {
    case 1: rbrew_store8_raw(mem, addr, *static_cast<const uint8_t*>(bytes)); break;
    case 2: rbrew_store16_raw(mem, addr, *static_cast<const uint16_t*>(bytes)); break;
    case 4: rbrew_store32_raw(mem, addr, *static_cast<const uint32_t*>(bytes)); break;
    case 8: rbrew_store64_raw(mem, addr, *static_cast<const uint64_t*>(bytes)); break;
    default: {
        const uint8_t* src = static_cast<const uint8_t*>(bytes);
        for (uint32_t i = 0; i < size; ++i) rbrew_store8_raw(mem, addr + i, src[i]);
        break;
    }
    }
#else
    if (rbrew_thread_write_observed || rbrew_strict_reservations_active.load(std::memory_order_relaxed)) {
        rbrew_write_memory(mem, addr, bytes, size);
        return;
    }
    switch (size) {
    case 1: rbrew_store8_raw(mem, addr, *static_cast<const uint8_t*>(bytes)); break;
    case 2: rbrew_store16_raw(mem, addr, *static_cast<const uint16_t*>(bytes)); break;
    case 4: rbrew_store32_raw(mem, addr, *static_cast<const uint32_t*>(bytes)); break;
    case 8: rbrew_store64_raw(mem, addr, *static_cast<const uint64_t*>(bytes)); break;
    default: {
        const uint8_t* src = static_cast<const uint8_t*>(bytes);
        for (uint32_t i = 0; i < size; ++i) rbrew_store8_raw(mem, addr + i, src[i]);
        break;
    }
    }
#endif
}

// Byte-reversed stores (sthbrx/stwbrx): plain host-order copies.
static inline void rbrew_write16_rev(uint8_t* __restrict mem, uint32_t addr, uint16_t v) {
    rbrew_store_bytes(mem, addr, &v, 2);
}
static inline void rbrew_write32_rev(uint8_t* __restrict mem, uint32_t addr, uint32_t v) {
    rbrew_store_bytes(mem, addr, &v, 4);
}
static RBREW_NOINLINE void rbrew_write8_checked(uint8_t* mem, uint32_t addr, uint8_t v) {
    if (!rbrew_memory_accessible(mem, addr, 1)) {
        RBREW_LOG_INFO("rbrew", "OOB write8  addr=0x%08X val=0x%02X\n", addr, v);
        return;
    }
    rbrew_write_memory(mem, addr, &v, 1);
}
static RBREW_NOINLINE void rbrew_write16_checked(uint8_t* mem, uint32_t addr, uint16_t v) {
    if (!rbrew_memory_accessible(mem, addr, 2)) {
        RBREW_LOG_INFO("rbrew", "OOB write16 addr=0x%08X val=0x%04X\n", addr, v);
        return;
    }
    uint16_t be = rbrew_bswap16(v);
    rbrew_write_memory(mem, addr, &be, 2);
}
static RBREW_NOINLINE void rbrew_write32_checked(uint8_t* mem, uint32_t addr, uint32_t v) {
    if (!rbrew_memory_accessible(mem, addr, 4)) {
        RBREW_LOG_INFO("rbrew", "OOB write32 addr=0x%08X val=0x%08X\n", addr, v);
        return;
    }
    uint32_t be = rbrew_bswap32(v);
    rbrew_write_memory(mem, addr, &be, 4);
}
static RBREW_NOINLINE void rbrew_write64_checked(uint8_t* mem, uint32_t addr, uint64_t v) {
    if (!rbrew_memory_accessible(mem, addr, 8)) {
        RBREW_LOG_INFO("rbrew", "OOB write64 addr=0x%08X\n", addr);
        return;
    }
    uint64_t be = rbrew_bswap64(v);
    rbrew_write_memory(mem, addr, &be, 8);
}

static inline void rbrew_write8(uint8_t* __restrict mem, uint32_t addr, uint8_t v) {
    if (rbrew_arena_access(mem, addr, 1)) {
        rbrew_store_bytes(mem, addr, &v, 1);
        return;
    }
    rbrew_write8_checked(mem, addr, v);
}
static inline void rbrew_write16(uint8_t* __restrict mem, uint32_t addr, uint16_t v) {
    if (rbrew_arena_access(mem, addr, 2)) {
        uint16_t be = rbrew_bswap16(v);
        rbrew_store_bytes(mem, addr, &be, 2);
        return;
    }
    rbrew_write16_checked(mem, addr, v);
}
static inline void rbrew_write32(uint8_t* __restrict mem, uint32_t addr, uint32_t v) {
    if (rbrew_arena_access(mem, addr, 4)) {
        uint32_t be = rbrew_bswap32(v);
        rbrew_store_bytes(mem, addr, &be, 4);
        return;
    }
    rbrew_write32_checked(mem, addr, v);
}
static inline void rbrew_write64(uint8_t* __restrict mem, uint32_t addr, uint64_t v) {
    if (rbrew_arena_access(mem, addr, 8)) {
        uint64_t be = rbrew_bswap64(v);
        rbrew_store_bytes(mem, addr, &be, 8);
        return;
    }
    rbrew_write64_checked(mem, addr, v);
}
static inline void rbrew_write_f32(uint8_t* __restrict mem, uint32_t addr, double d) {
    float f = (float)d;
    uint32_t bits;
    memcpy(&bits, &f, 4);
    rbrew_write32(mem, addr, bits);
}
static inline void rbrew_write_f64(uint8_t* __restrict mem, uint32_t addr, double d) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    rbrew_write64(mem, addr, bits);
}

// ---------------------------------------------------------------------------
// Integer helpers
// ---------------------------------------------------------------------------
// Espresso divw/divwu never trap. Undefined-result cases follow the observed
// 750-family values (as Cemu): divw by 0 -> -1 for a negative dividend, else
// 0; INT_MIN / -1 -> 0xFFFFFFFF; divwu by 0 -> 0.
static inline uint32_t rbrew_divw(uint32_t a, uint32_t b) {
    if (b == 0) return int32_t(a) < 0 ? 0xFFFFFFFFu : 0u;
    if (a == 0x80000000u && b == 0xFFFFFFFFu) return 0xFFFFFFFFu;
    return uint32_t(int32_t(a) / int32_t(b));
}
static inline uint32_t rbrew_divwu(uint32_t a, uint32_t b) {
    return b ? a / b : 0u;
}
static inline uint32_t rbrew_abs32(uint32_t v) {
    return (uint32_t)abs((int32_t)v);
}
static inline uint32_t rbrew_rotl32(uint32_t v, uint32_t n) {
    n &= 31u;
    return (v << n) | (v >> ((32u - n) & 31u));
}
static inline uint32_t rbrew_rotr32(uint32_t v, uint32_t n) {
    n &= 31u;
    return (v >> n) | (v << ((32u - n) & 31u));
}
static inline uint32_t rbrew_clz32(uint32_t v) {
    if (v == 0) return 32;
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_clz(v);
#elif defined(_MSC_VER)
    unsigned long index;
    _BitScanReverse(&index, v); // v != 0: highest set bit
    return 31u - (uint32_t)index;
#else
    uint32_t n = 0;
    while (!(v & 0x80000000u)) {
        ++n;
        v <<= 1;
    }
    return n;
#endif
}
static inline uint32_t rbrew_clo32(uint32_t v) {
    return rbrew_clz32(~v);
}
static inline uint32_t rbrew_popcount32(uint32_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_popcount(v);
#else
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    return (((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
#endif
}

// rlwinm: rotate left, then mask bits [mb..me] (PowerPC semantics)
static inline uint32_t rbrew_rlwinm(uint32_t v, uint32_t sh, uint32_t mb, uint32_t me) {
    uint32_t r = rbrew_rotl32(v, sh);
    // PPC bit zero is the MSB. A wrapping interval selects both ends of
    // the word; adjacent MB/ME boundaries select all 32 bits.
    uint32_t from_begin = 0xFFFFFFFFu >> mb;
    uint32_t through_end = 0xFFFFFFFFu << (31u - me);
    uint32_t mask = mb <= me ? from_begin & through_end : from_begin | through_end;
    return r & mask;
}

// Carrying arithmetic (addic/addze/subfic/...): one widened addition gives
// both the sum and the carry-out, instead of compare chains.
static inline uint32_t rbrew_add_ci32(uint32_t a, uint32_t b, uint32_t cin) {
    return a + b + (cin & 1u);
}
static inline uint32_t rbrew_carry_out32(uint32_t a, uint32_t b, uint32_t cin) {
    return uint32_t((uint64_t(a) + b + (cin & 1u)) >> 32);
}

// sraw/srawi: arithmetic right shift (amount is taken modulo 64; amounts of
// 32 or more fill with the sign bit). XER.CA is set when the value is
// negative and any 1-bit is shifted out — at 32 or more every bit of rS
// leaves the shifter, the sign bit included.
static inline uint32_t rbrew_sraw_result(uint32_t v, uint32_t amount) {
    const uint32_t n = amount & 63u;
    return n >= 32 ? uint32_t(int32_t(v) >> 31) : uint32_t(int32_t(v) >> n);
}
static inline uint32_t rbrew_sraw_carry(uint32_t v, uint32_t amount) {
    const uint32_t n = amount & 63u;
    if (n == 0) return 0;
    const uint32_t lost = n >= 32 ? v : (v & ((1u << n) - 1u));
    return (int32_t(v) < 0 && lost != 0) ? 1u : 0u;
}

// ---------------------------------------------------------------------------
// Comparison helpers
// Returns a 4-bit CR field value: bit3=LT, bit2=GT, bit1=EQ, bit0=SO
// Stored in cpu->cr[n] as a uint32_t with those bits.
// ---------------------------------------------------------------------------
static inline uint32_t rbrew_cmp_s32(uint32_t a, uint32_t b) {
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    if (sa < sb) return 8u; // LT
    if (sa > sb) return 4u; // GT
    return 2u;              // EQ
}
static inline uint32_t rbrew_cmp_u32(uint32_t a, uint32_t b) {
    if (a < b) return 8u;
    if (a > b) return 4u;
    return 2u;
}
static inline uint32_t rbrew_cmp_f64(double a, double b) {
    if (a < b) return 8u;
    if (a > b) return 4u;
    if (a == b) return 2u;
    return 1u; // unordered (NaN)
}

// ---------------------------------------------------------------------------
// FP helper
// ---------------------------------------------------------------------------
static inline double rbrew_fround(double v) {
    return (double)(float)v;
}

extern RBREW_HOST_API const bool rbrew_paired_inline_enabled;

// psq quantization: the 2^[-31,32] scale factor is built exactly (no libm),
// matching the ldexp reference for every GQR scale.
static inline int rbrew_psq_signed_scale(uint32_t scale) {
    const int e = int(scale & 0x3F);
    return e - ((e & 32) << 1);
}
static inline double rbrew_psq_factor(uint32_t scale, bool store) {
    const int k = rbrew_psq_signed_scale(scale);
    const uint64_t bits = uint64_t(1023 + (store ? k : -k)) << 52;
    double f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}
// Saturating quantized store of one element (signed-NaN picks the sign's bound).
static inline void rbrew_psq_store_int(uint8_t* __restrict mem, uint32_t ea, double v, double factor, int32_t lo,
                                       int32_t hi, bool wide) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    int32_t q;
    if ((bits & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull) // NaN
        q = (bits >> 63) ? lo : hi;
    else {
        const double scaled = v * factor;
        q = scaled < double(lo) ? lo : scaled > double(hi) ? hi : int32_t(scaled);
    }
    if (wide)
        rbrew_write16(mem, ea, uint16_t(q));
    else
        rbrew_write8(mem, ea, uint8_t(q));
}
// Values below the single exponent range store a signed zero (decaf).
static inline void rbrew_psq_store_float(uint8_t* __restrict mem, uint32_t ea, double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    if (((bits >> 52) & 2047) <= 896)
        rbrew_write32(mem, ea, uint32_t(bits >> 32) & 0x80000000u);
    else
        rbrew_write_f32(mem, ea, v);
}

// Generated instruction words are constant: decode once at compile time.
// The (cpu, mem) form is what generated code calls; the arena base is passed
// explicitly so host compilers can keep guest registers in host registers
// across the inline loads/stores.
template <uint32_t w> static inline void rbrew_ps_constant(CPUState* cpu, uint8_t* __restrict mem) {
    if (!rbrew_paired_inline_enabled) return rbrew_ps_mem(cpu, mem, w);
    constexpr uint32_t op = w >> 26, d = (w >> 21) & 31, a = (w >> 16) & 31;
    if constexpr (op == 56 || op == 57 || op == 60 || op == 61) {
        constexpr bool load = op < 60, update = op == 57 || op == 61, single = (w >> 15) & 1;
        const uint32_t gqr = cpu->gqr[(w >> 12) & 7];
        const uint32_t type = (gqr >> (load ? 16 : 0)) & 7;
        const uint32_t scale = (gqr >> (load ? 24 : 8)) & 0x3F;
        constexpr int32_t disp = int32_t((w & 4095u) << 20) >> 20;
        const uint32_t ea = (update || a ? cpu->r[a] : 0u) + uint32_t(disp);
        if constexpr (load) {
            const double factor = type == 0 ? 1.0 : rbrew_psq_factor(scale, false);
            switch (type) {
            case 0: // float
                cpu->f[d] = rbrew_read_f32(mem, ea);
                cpu->ps1[d] = single ? 1.0 : rbrew_read_f32(mem, ea + 4);
                break;
            case 4: // u8
                cpu->f[d] = double(rbrew_read8(mem, ea)) * factor;
                cpu->ps1[d] = single ? 1.0 : double(rbrew_read8(mem, ea + 1)) * factor;
                break;
            case 5: // u16
                cpu->f[d] = double(rbrew_read16(mem, ea)) * factor;
                cpu->ps1[d] = single ? 1.0 : double(rbrew_read16(mem, ea + 2)) * factor;
                break;
            case 6: // s8
                cpu->f[d] = double(int8_t(rbrew_read8(mem, ea))) * factor;
                cpu->ps1[d] = single ? 1.0 : double(int8_t(rbrew_read8(mem, ea + 1))) * factor;
                break;
            case 7: // s16
                cpu->f[d] = double(int16_t(rbrew_read16(mem, ea))) * factor;
                cpu->ps1[d] = single ? 1.0 : double(int16_t(rbrew_read16(mem, ea + 2))) * factor;
                break;
            default: return rbrew_ps_mem(cpu, mem, w);
            }
        } else {
            if (type != 0 && type < 4) return rbrew_ps_mem(cpu, mem, w); // undefined quantization
            const double factor = type == 0 ? 1.0 : rbrew_psq_factor(scale, true);
            auto store_one = [&](uint32_t address, double value) {
                switch (type) {
                case 0: rbrew_psq_store_float(mem, address, value); break;
                case 4: rbrew_psq_store_int(mem, address, value, factor, 0, 255, false); break;
                case 5: rbrew_psq_store_int(mem, address, value, factor, 0, 65535, true); break;
                case 6: rbrew_psq_store_int(mem, address, value, factor, -128, 127, false); break;
                case 7: rbrew_psq_store_int(mem, address, value, factor, -32768, 32767, true); break;
                default: break;
                }
            };
            store_one(ea, cpu->f[d]);
            if constexpr (!single) {
                if (type == 0)
                    store_one(ea + 4, cpu->ps1[d]);
                else
                    store_one(ea + ((type & 1) ? 2u : 1u), cpu->ps1[d]); // 8-bit types 4/6, 16-bit 5/7
            }
        }
        if constexpr (update) cpu->r[a] = ea;
        return;
    } else if constexpr (op == 4) {
        constexpr uint32_t b = (w >> 11) & 31, c = (w >> 6) & 31, xo = (w >> 1) & 31;
        const double a0 = cpu->f[a], a1 = cpu->ps1[a], b0 = cpu->f[b], b1 = cpu->ps1[b];
        const double c0 = cpu->f[c], c1 = cpu->ps1[c];
        double p0, p1;
        if constexpr (xo == 10) {
            p0 = a0 + b1;
            p1 = c1;
        } else if constexpr (xo == 11) {
            p0 = c0;
            p1 = a0 + b1;
        } else if constexpr (xo == 12) {
            p0 = a0 * c0;
            p1 = a1 * c0;
        } else if constexpr (xo == 13) {
            p0 = a0 * c1;
            p1 = a1 * c1;
        } else if constexpr (xo == 14) {
            p0 = fma(a0, c0, b0);
            p1 = fma(a1, c0, b1);
        } else if constexpr (xo == 15) {
            p0 = fma(a0, c1, b0);
            p1 = fma(a1, c1, b1);
        } else if constexpr (xo == 20) {
            p0 = a0 - b0;
            p1 = a1 - b1;
        } else if constexpr (xo == 21) {
            p0 = a0 + b0;
            p1 = a1 + b1;
        } else if constexpr (xo == 25) {
            p0 = a0 * c0;
            p1 = a1 * c1;
        } else if constexpr (xo == 28) {
            p0 = fma(a0, c0, -b0);
            p1 = fma(a1, c1, -b1);
        } else if constexpr (xo == 29) {
            p0 = fma(a0, c0, b0);
            p1 = fma(a1, c1, b1);
        } else
            return rbrew_ps_mem(cpu, mem, w);
        cpu->f[d] = double(float(p0));
        cpu->ps1[d] = double(float(p1));
        if constexpr (w & 1) cpu->cr[1] = (cpu->fpscr >> 28) & 15;
        return;
    }
    rbrew_ps_mem(cpu, mem, w);
}
template <uint32_t w> static inline void rbrew_ps_constant(CPUState* cpu) {
    rbrew_ps_constant<w>(cpu, cpu->mem);
}

// ---------------------------------------------------------------------------
// Dispatch — called for indirect jumps/calls (bctr/bctrl)
// Looks up the host implementation for the guest address in ctr.
// ---------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

// The _mem forms are what generated code calls: the arena base is passed
// explicitly so host compilers know these may touch guest memory (and only
// that) while keeping guest registers cached in host registers.
RBREW_HOST_API void rbrew_dispatch_mem(CPUState* cpu, uint8_t* mem, uint32_t guest_addr) noexcept(false);
RBREW_HOST_API void rbrew_dispatch(CPUState* cpu, uint32_t guest_addr) noexcept(false);
// Only use at proven linked-call/entry boundaries with an explicit LR return.
// Legacy local/tail dispatch retains its existing behavior.
RBREW_HOST_API void rbrew_dispatch_linked_mem(CPUState* cpu, uint8_t* mem, uint32_t guest_addr) noexcept(false);
RBREW_HOST_API void rbrew_dispatch_linked(CPUState* cpu, uint32_t guest_addr) noexcept(false);
// Invoke the module's declared entry with loader arguments and return its result.
int32_t rbrew_run_entry(CPUState* cpu, uint32_t entry, uint32_t argc, uint32_t argv);
// GHS-style null-terminated constructor table; mark before calling so that
// reentrant initialization and a later guest startup cannot run it twice.
// The caller supplies the program's table and completion-flag addresses.
uint32_t rbrew_run_constructors(CPUState* cpu, uint32_t table, uint32_t done_flag);
RBREW_HOST_API void rbrew_call_indirect_mem(CPUState* cpu, uint8_t* mem, uint32_t guest_addr) noexcept(false);
RBREW_HOST_API void rbrew_call_indirect(CPUState* cpu, uint32_t guest_addr) noexcept(false);
RBREW_HOST_API void rbrew_call_mem(CPUState* cpu, uint8_t* mem, uint32_t guest_addr) noexcept(false);
RBREW_HOST_API void rbrew_call(CPUState* cpu, uint32_t guest_addr) noexcept(false);

// Called by OS stubs to register a host function at a guest address
RBREW_HOST_API void rbrew_register_func(CPUState* cpu, uint32_t guest_addr, HostFunc fn);

// Opt-in host hooks for guest functions the recompiler wrapped (--hooks).
// A hook replaces the call; it runs `original` itself to keep the game's
// behavior. With no hook registered the wrapper calls the original directly.
typedef void (*RbrewGuestHook)(CPUState* cpu, uint32_t guest_addr, HostFunc original);
RBREW_HOST_API RbrewGuestHook rbrew_guest_hook(uint32_t guest_addr);
// Register (or clear with null) before guest code runs; returns false when full.
RBREW_HOST_API bool rbrew_set_guest_hook(uint32_t guest_addr, RbrewGuestHook hook);

#ifdef __cplusplus
}
#endif

#include "rebrewu_coverage.h"

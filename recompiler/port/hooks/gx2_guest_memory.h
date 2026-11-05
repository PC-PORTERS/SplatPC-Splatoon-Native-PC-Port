// gx2_guest_memory.h - little-endian access to the game's guest heap, shared
// by the game hooks and the lighting driver.
#pragma once
#include "../runtime/rebrewu_runtime.h"
#include <cstdint>
#include <cstring>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace gx2env {
constexpr uint32_t kHeapBase = 0x28000000u, kHeapEnd = 0x68000000u;

inline uint32_t r32(const uint8_t* mem, uint32_t address) {
    uint32_t value;
    std::memcpy(&value, mem + address, 4);
    return rbrew_bswap32(value);
}
inline void w32(uint8_t* mem, uint32_t address, uint32_t value) {
    value = rbrew_bswap32(value);
    std::memcpy(mem + address, &value, 4);
}
inline float rf(const uint8_t* mem, uint32_t address) {
    const uint32_t bits = r32(mem, address);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
inline void wf(uint8_t* mem, uint32_t address, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    w32(mem, address, bits);
}

template <class F> inline void for_each_heap_region(const uint8_t* mem, F&& f) {
#if defined(_WIN32)
    const uint8_t* cursor = mem + kHeapBase;
    const uint8_t* end = mem + kHeapEnd;
    MEMORY_BASIC_INFORMATION info;
    while (cursor < end && VirtualQuery(cursor, &info, sizeof info) == sizeof info) {
        const uint8_t* base = static_cast<const uint8_t*>(info.BaseAddress);
        const uint8_t* stop = base + info.RegionSize;
        if (base < cursor) base = cursor;
        if (stop > end) stop = end;
        cursor = stop;
        if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            f(uint32_t(base - mem), uint32_t(stop - mem));
    }
#else
    (void)mem;
    (void)f;
#endif
}
} // namespace gx2env

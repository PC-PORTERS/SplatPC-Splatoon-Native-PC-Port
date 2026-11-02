#pragma once
#include <stdint.h>

// Native port layout: loaded image and main stack below 0x20000000;
// bootstrap allocator in [0x20000000,0x28000000); application provider above it.
// A Cafe application has a 1 GiB MEM2 budget. Keep that capacity separate from
// the port's bootstrap reservation; these are native guest arena addresses,
// not a claim about the console's physical/virtual memory map.
constexpr uint32_t RBREW_APPLICATION_HEAP_BASE = 0x28000000u;
constexpr uint32_t RBREW_APPLICATION_HEAP_SIZE = 0x40000000u;
constexpr uint32_t RBREW_APPLICATION_ARENA_END = RBREW_APPLICATION_HEAP_BASE + RBREW_APPLICATION_HEAP_SIZE;
static_assert(uint64_t(RBREW_APPLICATION_HEAP_BASE) + RBREW_APPLICATION_HEAP_SIZE == RBREW_APPLICATION_ARENA_END);

// Cafe's optional OverlayArena is a separate virtual mapping, not additional
// capacity for the native application allocator. Reserve the aperture without
// committing the intervening hole; OSEnableOverlayArena maps its pages.
constexpr uint32_t RBREW_OVERLAY_BASE = 0xA0000000u;
constexpr uint32_t RBREW_OVERLAY_SIZE = 0x1C000000u;
static_assert(RBREW_APPLICATION_ARENA_END < RBREW_OVERLAY_BASE);

// Cafe base-heap regions at their console virtual addresses (Cemu MMU/FG):
// the foreground-bucket free area backs MEMGetBaseHeapHandle(8) and MEM1
// backs MEMGetBaseHeapHandle(0). Both are always mapped for a foreground app.
constexpr uint32_t RBREW_FG_FREE_BASE = 0xE0000000u;
constexpr uint32_t RBREW_FG_FREE_SIZE = 0x02800000u;
constexpr uint32_t RBREW_MEM1_BASE = 0xF4000000u;
constexpr uint32_t RBREW_MEM1_SIZE = 0x02000000u;
static_assert(RBREW_OVERLAY_BASE + RBREW_OVERLAY_SIZE <= RBREW_FG_FREE_BASE);
static_assert(RBREW_FG_FREE_BASE + RBREW_FG_FREE_SIZE <= RBREW_MEM1_BASE);
// Cafe shared-data area (system fonts; Cemu MMU MEMORY_SHAREDDATA_AREA_ADDR):
// a table of 0x1C-byte entries followed by the font files (OSGetSharedData).
constexpr uint32_t RBREW_SHARED_DATA_BASE = 0xF8000000u;
constexpr uint32_t RBREW_SHARED_DATA_SIZE = 0x02000000u;
static_assert(RBREW_MEM1_BASE + RBREW_MEM1_SIZE <= RBREW_SHARED_DATA_BASE);
constexpr uint32_t RBREW_ARENA_END = RBREW_SHARED_DATA_BASE + RBREW_SHARED_DATA_SIZE;

// Host aperture: the whole 32-bit guest address space plus a small slop, so
// every guest effective address (and a multi-byte access at the very top of
// the space) lands inside one flat mapping. Guest addresses are byte offsets
// from the aperture base; there is no translation stage. This is virtual
// space, not RAM: pages stay demand-zero until the guest touches them, so
// the Wii U's own 2 GiB of physical memory is irrelevant here — its virtual
// map is sparse across 4 GiB just like this aperture.
constexpr uint64_t RBREW_GUEST_APERTURE_SIZE = 0x100000000ull + 0x10000ull;
static_assert(RBREW_ARENA_END <= 0x100000000ull);

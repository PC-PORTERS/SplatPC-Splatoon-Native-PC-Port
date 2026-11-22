#pragma once
#include "gx2_vk.h"
#include <algorithm>
#include <array>

namespace gx2vk::memory_detail {
// Fixed-size ordering: no allocation or memory-budget polling on the draw path.
// DEVICE_LOCAL describes access speed, not dedicated VRAM: UMA RAM is local too.
struct Types {
    std::array<uint32_t, VK_MAX_MEMORY_TYPES> indices{};
    uint32_t count = 0;
};
inline Types memory_types(const VkPhysicalDeviceMemoryProperties& memory, uint32_t bits, MemoryUse use) {
    Types result;
    auto score = [&](uint32_t type) {
        const auto flags = memory.memoryTypes[type].propertyFlags;
        const int local = (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? 1 : 0;
        const int coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? 1 : 0;
        const int cached = (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 1 : 0;
        if (use == MemoryUse::DeviceLocal) return local * 100;
        if (use == MemoryUse::Readback) return cached * 100 + coherent * 10 + local;
        return local * 100 + coherent * 10;
    };
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        const auto flags = memory.memoryTypes[i].propertyFlags;
        if (!(bits & (1u << i))) continue;
        // These require resource/device features that this allocator never enables.
        if (flags & (VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT |
                     VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD))
            continue;
        if (use != MemoryUse::DeviceLocal && !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
        // Stable insertion preserves the driver's order among equally suitable types.
        uint32_t pos = result.count++;
        while (pos && score(i) > score(result.indices[pos - 1])) {
            result.indices[pos] = result.indices[pos - 1];
            --pos;
        }
        result.indices[pos] = i;
    }
    return result;
}

// A refused pool block size is remembered per memory type; after this many pool misses on that
// type it is forgotten, so a refusal that was only transient cannot last.
constexpr uint32_t kBlockRetryMisses = 512;

inline VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

// Allocation-relative ranges become atom-aligned ranges in the backing memory.
// Suballocations and ring spans never share a non-coherent atom with a neighbour.
inline VkMappedMemoryRange mapped_range(const Allocation& a, VkDeviceSize atom, VkDeviceSize offset,
                                        VkDeviceSize bytes) {
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = a.memory;
    const VkDeviceSize begin = a.offset + offset;
    range.offset = begin / atom * atom;
    const VkDeviceSize available = a.size - offset;
    const VkDeviceSize end = begin + std::min(bytes, available);
    range.size = std::min(align_up(end, atom), a.backing_size) - range.offset;
    return range;
}
} // namespace gx2vk::memory_detail

#pragma once
#include "gx2_vk.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <string_view>

namespace gx2vk::device_detail {
inline const char* missing_feature(const VkPhysicalDeviceFeatures& core, const VkPhysicalDeviceVulkan12Features& f12,
                                   const VkPhysicalDeviceVulkan13Features& f13) {
    struct Required {
        const char* name;
        VkBool32 supported;
    };
    const Required required[] = {
        {"dynamicRendering", f13.dynamicRendering},
        {"synchronization2", f13.synchronization2},
        {"timelineSemaphore", f12.timelineSemaphore},
        {"hostQueryReset", f12.hostQueryReset},
        {"geometryShader", core.geometryShader},
        {"independentBlend", core.independentBlend},
        {"textureCompressionBC", core.textureCompressionBC},
        {"imageCubeArray", core.imageCubeArray},
        {"largePoints", core.largePoints},
        // SPIR-V 1.6 discard uses OpDemoteToHelperInvocation.
        {"shaderDemoteToHelperInvocation", f13.shaderDemoteToHelperInvocation},
    };
    for (const auto& feature : required)
        if (!feature.supported) return feature.name;
    return nullptr;
}

inline bool matches(std::string_view request, uint32_t index, std::string_view name) {
    if (request.empty()) return true;
    uint32_t requested_index = 0;
    const auto parsed = std::from_chars(request.data(), request.data() + request.size(), requested_index);
    if (parsed.ec == std::errc{} && parsed.ptr == request.data() + request.size()) return requested_index == index;
    return std::search(name.begin(), name.end(), request.begin(), request.end(), [](unsigned char a, unsigned char b) {
               return std::tolower(a) == std::tolower(b);
           }) != name.end();
}

inline int score(VkPhysicalDeviceType type) {
    if (type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) return 2;
    if (type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) return 1;
    return 0;
}
} // namespace gx2vk::device_detail

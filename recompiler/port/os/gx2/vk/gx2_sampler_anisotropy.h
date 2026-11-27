// gx2_sampler_anisotropy.h - pure helper for effective Vulkan sampler anisotropy.
#pragma once
#include <algorithm>

namespace gx2_anisotropy {

// Computes effective VkSampler maxAnisotropy (1.0f = disabled) given guest
// sampler parameters and an optional forced override (0 = game default, 2/4/8/16).
// Forced AF only applies to eligible mipmapped linear color samplers
// (never integer, shadow/compare, or nearest-filtered UI/LUT samplers).
inline float effective_anisotropy(float guest_anisotropy, bool mipmapped, bool linear_filter, bool shadow, bool integer,
                                  int forced_af, bool device_supports_anisotropy, float device_max_anisotropy) {
    if (!device_supports_anisotropy || integer || device_max_anisotropy <= 1.0f) return 1.0f;
    float level = std::clamp(guest_anisotropy, 1.0f, device_max_anisotropy);
    if (forced_af > 1 && mipmapped && linear_filter && !shadow)
        level = std::max(level, std::min(float(forced_af), device_max_anisotropy));
    return level > 1.0f ? level : 1.0f;
}

} // namespace gx2_anisotropy

// gx2_present_fx.h - present-time post-process anti-aliasing (FXAA, SMAA)
// and spatial upscaling (FSR 1.0 EASU+RCAS, Anime4K) compute passes.
#pragma once
#include "gx2_vk.h"
#include <cstdint>

namespace gx2_present_fx {

struct Output {
    const gx2vk::Image* image = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Applies configured AA / upscaling based on active port settings.
// Returns {nullptr, 0, 0} when no pass needs to run (zero-overhead fast path).
Output apply(const gx2vk::Image& source, uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h);

// Explicit-mode overload for unit tests and deterministic benchmarks.
Output apply_explicit(const gx2vk::Image& source, uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                      int aa_mode, int upscaler, int sharpness);

void shutdown();

} // namespace gx2_present_fx

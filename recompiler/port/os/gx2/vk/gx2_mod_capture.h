#pragma once

#include <cstdint>

namespace gx2vk {
struct Image;
}

namespace gx2_mod_capture {

// Non-blocking async Vulkan readback for active mod frame capture callbacks.
// Returns immediately when no mod has registered a frame capture provider.
void capture_if_wanted(gx2vk::Image* tv_img, uint32_t tv_w, uint32_t tv_h, gx2vk::Image* drc_img, uint32_t drc_w,
                       uint32_t drc_h);

void shutdown();

} // namespace gx2_mod_capture

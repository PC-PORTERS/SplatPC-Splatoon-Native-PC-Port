#pragma once
#include "gx2_vk.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace gx2r {
// Guest dimensions and allocations remain in console coordinates. Only the
// host's screen-sized attachments and their downsample chain change size.
inline VkExtent2D screen_target_extent(uint32_t width, uint32_t height, VkExtent2D display) {
    for (unsigned level = 0; level < 8; ++level)
        if (width == (1280u >> level) && height == (720u >> level))
            return {std::max(1u, display.width >> level), std::max(1u, display.height >> level)};
    if (display.width >= 1280u && display.height >= 720u) {
        constexpr uint32_t kDrcWidths[] = {848u, 854u, 856u, 864u};
        for (unsigned level = 0; level < 4; ++level) {
            if (height != (480u >> level)) continue;
            const uint32_t round = (1u << level) - 1u;
            for (uint32_t base : kDrcWidths)
                if (width == (base >> level) || width == ((base + round) >> level))
                    return {std::max(1u, display.width >> level), std::max(1u, display.height >> level)};
        }
    }
    return {width, height};
}
inline std::array<float, 2> image_coordinate_scale(const gx2vk::Image& image, uint32_t width, uint32_t height) {
    return {width ? float(image.desc.width) / width : 1.0f, height ? float(image.desc.height) / height : 1.0f};
}
// CPU-visible GX2 copies keep the original packing and texel bits. Only these
// uncommon transfers need resampling; ordinary texture uploads take no detour.
inline std::vector<uint8_t> resize_texels(const uint8_t* source, uint32_t width, uint32_t height, uint32_t out_width,
                                          uint32_t out_height, uint32_t bytes) {
    std::vector<uint8_t> out(size_t(out_width) * out_height * bytes);
    for (uint32_t y = 0; y < out_height; ++y) {
        const uint32_t sy = uint32_t((uint64_t(y) * 2 + 1) * height / (uint64_t(out_height) * 2));
        for (uint32_t x = 0; x < out_width; ++x) {
            const uint32_t sx = uint32_t((uint64_t(x) * 2 + 1) * width / (uint64_t(out_width) * 2));
            std::memcpy(out.data() + (size_t(y) * out_width + x) * bytes, source + (size_t(sy) * width + sx) * bytes,
                        bytes);
        }
    }
    return out;
}
inline VkViewport resolution_viewport(const int32_t* viewport, bool upper_left, const double* depth,
                                      const std::array<float, 2>& scale) {
    const float x = float(viewport[0]) * scale[0], y = float(viewport[1]) * scale[1];
    const float width = float(viewport[2]) * scale[0], height = float(viewport[3]) * scale[1];
    return {x, upper_left ? y + height : y, width, upper_left ? -height : height, float(depth[0]), float(depth[1])};
}
inline VkRect2D resolution_scissor(const int32_t* rect, VkExtent2D extent, const std::array<float, 2>& scale) {
    const auto edge = [](int64_t value, float factor, uint32_t limit, bool end) {
        const double position = double(value) * factor;
        return int32_t(std::clamp(end ? std::ceil(position) : std::floor(position), 0.0, double(limit)));
    };
    if (rect[2] <= 0 || rect[3] <= 0) return {};
    const int32_t x0 = edge(rect[0], scale[0], extent.width, false), y0 = edge(rect[1], scale[1], extent.height, false);
    const int32_t x1 = edge(int64_t(rect[0]) + rect[2], scale[0], extent.width, true);
    const int32_t y1 = edge(int64_t(rect[1]) + rect[3], scale[1], extent.height, true);
    return {{x0, y0}, {uint32_t(std::max(0, x1 - x0)), uint32_t(std::max(0, y1 - y0))}};
}
} // namespace gx2r

// Authored texture and uniform workloads; no generated guest code or game assets.
#include "Standalone.h"
#include "os/gx2/gx2_render.h"
#include "os/gx2/gx2_surface.h"
#include "os/gx2/gx2_surface_layout.h"
#include "os/gx2/gx2_detile.h"
#include "os/gx2/vk/gx2_render_vk_state.h"
#include "runtime/rebrewu_runtime.h"
#include "rebrewu/logger.hpp"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
void require(bool value, const char* error) {
    if (!value) throw std::runtime_error(error);
}
double ms(Clock::time_point time) {
    return std::chrono::duration<double, std::milli>(Clock::now() - time).count();
}
uint32_t original(const Gx2DetileDesc& d, uint32_t x, uint32_t y) {
    const auto mode = static_cast<Latte::E_HWTILEMODE>(d.tile_mode);
    if (d.tile_mode <= 1 || d.tile_mode == 16)
        return LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, d.slice, 0, d.bits_per_element, d.pitch,
                                                               d.surface_height, 1);
    if (d.tile_mode <= 3)
        return LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, d.slice, d.bits_per_element, d.pitch,
                                                                   d.surface_height, mode, d.is_depth);
    return LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiled(x, y, d.slice, 0, d.bits_per_element, d.pitch,
                                                               d.surface_height, 1, mode, d.is_depth,
                                                               (d.swizzle >> 8) & 1, (d.swizzle >> 9) & 3);
}
void layouts() {
    std::vector<uint8_t> tiled(2u << 20);
    for (size_t i = 0; i < tiled.size(); ++i) tiled[i] = uint8_t(i * 13 + (i >> 8));
    unsigned cases = 0;
    for (unsigned mode = 4; mode <= 15; ++mode)
        for (unsigned bits : {8u, 16u, 32u, 64u, 128u})
            for (unsigned swizzle : {0u, 0x100u, 0x600u, 0x700u})
                for (unsigned slice : {0u, 3u, 7u})
                    for (bool depth : {false, true}) {
                        Gx2DetileDesc d;
                        d.width = 41;
                        d.height = 37;
                        d.pitch = 96;
                        d.surface_height = 64;
                        d.bits_per_element = bits;
                        d.tile_mode = mode;
                        d.swizzle = swizzle;
                        d.slice = slice;
                        d.is_depth = depth;
                        const auto bytes = bits / 8;
                        std::vector<uint8_t> expected(size_t(d.width) * d.height * bytes), actual(expected.size()),
                            back(tiled.size(), 0xa5);
                        size_t required = 0;
                        for (unsigned y = 0; y < d.height; ++y)
                            for (unsigned x = 0; x < d.width; ++x) {
                                auto offset = original(d, x, y);
                                required = std::max(required, size_t(offset) + bytes);
                                require(required <= tiled.size(), "authored address exceeds input");
                                std::memcpy(expected.data() + (size_t(y) * d.width + x) * bytes, tiled.data() + offset,
                                            bytes);
                            }
                        require(gx2_detile_mip0(d, tiled.data(), required, actual.data()) && actual == expected,
                                "microtile decode differs from independent addresses");
                        require(!gx2_detile_mip0(d, tiled.data(), required - 1, actual.data()),
                                "microtile decode accepted short input");
                        require(gx2_retile_mip0(d, expected.data(), back.data(), required), "microtile encode failed");
                        for (unsigned y = 0; y < d.height; ++y)
                            for (unsigned x = 0; x < d.width; ++x)
                                require(!std::memcmp(back.data() + original(d, x, y),
                                                     expected.data() + (size_t(y) * d.width + x) * bytes, bytes),
                                        "microtile encode differs from independent addresses");
                        const auto before = back;
                        require(!gx2_retile_mip0(d, expected.data(), back.data(), required - 1) && back == before,
                                "failed retile changed memory");
                        ++cases;
                    }
    // Tall lookup textures have no eight-texel row to specialize.
    for (unsigned mode = 4; mode <= 15; ++mode) {
        Gx2DetileDesc d;
        d.width = 4;
        d.height = 129;
        d.pitch = 96;
        d.surface_height = 160;
        d.bits_per_element = 32;
        d.tile_mode = mode;
        d.swizzle = 0x700;
        d.slice = 7;
        std::vector<uint8_t> expected(size_t(d.width) * d.height * 4), actual(expected.size());
        for (unsigned y = 0; y < d.height; ++y)
            for (unsigned x = 0; x < d.width; ++x) {
                const auto offset = original(d, x, y);
                require(uint64_t(offset) + 4 <= tiled.size(), "narrow authored address invalid");
                std::memcpy(expected.data() + (size_t(y) * d.width + x) * 4, tiled.data() + offset, 4);
            }
        require(gx2_detile_mip0(d, tiled.data(), tiled.size(), actual.data()) && actual == expected,
                "narrow decode changed");
        ++cases;
    }
    std::printf("{\"layout_cases\":%u,\"passed\":true}\n", cases);
}
void uniforms() {
    std::vector<uint8_t> mem(8192);
    for (unsigned i = 0; i < mem.size(); ++i) mem[i] = uint8_t(i * 17 + 3);
    for (unsigned count : {1u, 3u, 4u, 7u, 16u, 63u, 256u, 1024u})
        for (unsigned unaligned : {0u, 1u, 2u, 3u}) {
            gx2_render_set_uniform_reg(false, 0, 512 + unaligned, count, mem.data());
            for (unsigned i = 0; i < count; ++i)
                require(gx2_render_uniform_word(false, i) == rbrew_read32(mem.data(), 512 + unaligned + i * 4),
                        "uniform endian conversion changed bytes");
        }
    auto saved = gx2_render_uniform_word(false, 1023);
    gx2_render_set_uniform_reg(false, 1023, 512, 2, mem.data());
    require(gx2_render_uniform_word(false, 1023) == saved, "out-of-range uniform write changed state");
    // Keep IPO from folding repeated stores away in this API microbenchmark.
    void (*volatile upload)(bool, uint32_t, uint32_t, uint32_t, uint8_t*) = gx2_render_set_uniform_reg;
    for (unsigned count : {16u, 64u, 256u, 1024u}) {
        std::vector<double> samples;
        const unsigned operations = 100000;
        for (unsigned repeat = 0; repeat < 5; ++repeat) {
            auto start = Clock::now();
            for (unsigned i = 0; i < operations; ++i) upload(bool(i & 1), 0, 512, count, mem.data());
            samples.push_back(ms(start));
        }
        require(gx2_render_uniform_word(true, count - 1) == rbrew_read32(mem.data(), 512 + (count - 1) * 4),
                "timed uniform upload did not execute");
        std::sort(samples.begin(), samples.end());
        std::printf("{\"metric\":\"uniform_%u\",\"operations\":%u,\"median_ms\":%.6f}\n", count, operations,
                    samples[2]);
    }
}
void edgecases() {
    require(gx2vk::init(nullptr), "compatible GPU unavailable");
    gx2_render_init();
    std::vector<uint8_t> mem(32u << 20);
    constexpr uint32_t descriptor = 0x1000, address = 0x10000, mip_address = 0x1000000;
    auto word = [&](unsigned offset, unsigned value) { rbrew_write32(mem.data(), descriptor + offset, value); };
    struct Case {
        unsigned dim, depth, first, layers, format, tile;
        bool missing = false, short_base = false, zero_size = false;
    };
    unsigned checked = 0;
    for (const auto c : {Case{1, 1, 0, 1, 0x1a, 4}, Case{5, 4, 1, 2, 0x1a, 7}, Case{5, 2, 0, 2, 0x31, 4},
                         Case{2, 4, 0, 4, 0x1a, 7}, Case{3, 6, 0, 6, 0x1a, 4}, Case{1, 1, 0, 1, 0x1a, 4, true},
                         Case{1, 1, 0, 1, 0x1a, 1, false, true}, Case{1, 1, 0, 1, 0x1a, 0, false, false, true}}) {
        std::memset(mem.data() + descriptor, 0, GX2TEX_SIZEOF);
        RbrewGx2SurfaceDesc sd{
            c.dim,  32, c.dim == 3 ? 32u : 24u, c.depth, c.short_base || c.zero_size ? 1u : 4u, c.format, 0, 1,
            c.tile, 0};
        const auto layout = rbrew_gx2_calc_surface_layout(sd);
        require(layout.has_value(), "edge layout invalid");
        const auto format = gx2r::gx2_vk_format(c.format);
        const auto bytes = format.compressed ? format.block : format.bpp;
        word(GX2SURF_DIM, c.dim);
        word(GX2SURF_WIDTH, sd.width);
        word(GX2SURF_HEIGHT, sd.height);
        word(GX2SURF_DEPTH, c.depth);
        word(GX2SURF_MIP_LEVELS, sd.levels);
        word(GX2SURF_FORMAT, c.format);
        word(GX2SURF_IMAGE_SIZE, c.zero_size ? 0 : c.short_base ? 8 : layout->image_size);
        word(GX2SURF_IMAGE, address);
        word(GX2SURF_MIPMAPS, c.missing ? 0 : mip_address);
        word(GX2SURF_MIPMAP_SIZE, c.missing ? 0 : layout->mip_size);
        word(GX2SURF_TILE_MODE, c.tile);
        word(GX2SURF_PITCH, layout->pitch);
        word(GX2TEX_VIEW_FIRST_SLICE, c.first);
        word(GX2TEX_VIEW_NUM_SLICES, c.layers);
        word(GX2TEX_COMP_MAP, 0x06880000);
        for (unsigned i = 0; i < layout->mip_offsets.size(); ++i)
            word(GX2SURF_MIP_OFFSETS + i * 4, layout->mip_offsets[i]);
        std::vector<std::vector<uint8_t>> expected(sd.levels);
        for (unsigned level = 0; level < sd.levels; ++level) {
            const unsigned w = std::max(1u, sd.width >> level), h = std::max(1u, sd.height >> level),
                           layers = c.dim == 2 ? std::max(1u, c.layers >> level) : c.layers;
            Gx2DetileDesc d;
            d.width = format.compressed ? (w + 3) / 4 : w;
            d.height = format.compressed ? (h + 3) / 4 : h;
            d.bits_per_element = bytes * 8;
            d.pitch = layout->level_pitch[level];
            d.surface_height = layout->level_height[level];
            d.tile_mode = level ? layout->level_tile[level] : c.tile;
            const unsigned source = level ? mip_address + (level == 1 ? 0 : layout->mip_offsets[level - 1]) : address;
            auto& wanted = expected[level];
            wanted.resize(size_t(d.width) * d.height * layers * bytes);
            for (unsigned z = 0; z < layers; ++z) {
                d.slice = c.first + z;
                for (unsigned y = 0; y < d.height; ++y)
                    for (unsigned x = 0; x < d.width; ++x) {
                        const auto offset = original(d, x, y);
                        require(uint64_t(source) + offset + bytes <= mem.size(), "authored mip address exceeds memory");
                        for (unsigned b = 0; b < bytes; ++b) {
                            const uint8_t value = uint8_t(level * 37 + z * 11 + y * 5 + x + b);
                            mem[source + offset + b] = value;
                            wanted[((size_t(z) * d.height + y) * d.width + x) * bytes + b] =
                                level && c.missing ? 0 : value;
                        }
                    }
            }
            if (c.short_base) {
                std::fill(wanted.begin(), wanted.end(), 0);
                std::memcpy(wanted.data(), mem.data() + address, 8);
            }
        }
        gx2_render_bind_texture(descriptor, 0, mem.data());
        auto* texture = gx2r::state().units[0][c.dim == 5 ? 1 : c.dim == 2 ? 2 : c.dim == 3 ? 3 : 0];
        require(texture && texture->image, "edge upload missing image");
        for (unsigned level = 0; level < sd.levels; ++level) {
            const unsigned w = std::max(1u, sd.width >> level), h = std::max(1u, sd.height >> level),
                           layers = c.dim == 2 ? std::max(1u, c.layers >> level) : c.layers;
            if (c.dim == 2) {
                gx2r::end_rendering();
                auto readback = gx2vk::create_buffer(expected[level].size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                     gx2vk::MemoryUse::Readback);
                require(readback.memory.mapped, "volume readback allocation failed");
                gx2vk::barrier_if_dirty();
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                copy.imageExtent = {w, h, layers};
                auto cb = gx2vk::cmd();
                vkCmdCopyImageToBuffer(cb, texture->image->image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &copy);
                VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.memoryBarrierCount = 1;
                dependency.pMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(cb, &dependency);
                require(gx2vk::finish() && gx2vk::invalidate_mapped(readback.memory), "volume readback failed");
                require(!std::memcmp(readback.memory.mapped, expected[level].data(), expected[level].size()),
                        "volume mip bytes changed");
                gx2vk::destroy_buffer(readback);
            } else {
                for (unsigned z = 0; z < layers; ++z) {
                    std::vector<uint8_t> actual;
                    require(gx2vk::read_image(*texture->image, VK_IMAGE_ASPECT_COLOR_BIT, level, z, w, h, actual),
                            "mip readback failed");
                    const auto plane = expected[level].size() / layers;
                    require(actual.size() == plane &&
                                !std::memcmp(actual.data(), expected[level].data() + z * plane, plane),
                            "mip/layer/compressed or fallback bytes changed");
                }
            }
            ++checked;
        }
    }
    require(!gx2vk::validation_error_count(), "edge upload validation error");
    gx2_render_shutdown();
    std::printf("{\"checked_subresources\":%u,\"passed\":true}\n", checked);
}
void textures() {
    require(gx2vk::init(nullptr), "compatible GPU unavailable");
    gx2_render_init();
    std::printf("{\"device\":\"%s\",\"synthetic\":true}\n", gx2vk::caps().device_name.c_str());
    std::vector<uint8_t> mem(32u << 20);
    constexpr uint32_t descriptor = 0x1000, address = 0x10000;
    auto word = [&](unsigned offset, unsigned value) { rbrew_write32(mem.data(), descriptor + offset, value); };
    for (unsigned size : {256u, 1024u})
        for (unsigned mode : {1u, 4u, 5u, 6u, 7u}) {
            const unsigned layers = size == 256 ? 2 : 1;
            RbrewGx2SurfaceDesc surface{layers > 1 ? 5u : 1u, size, size, layers, 1, 0x1a, 0, 1, mode, 0};
            const auto layout = rbrew_gx2_calc_surface_layout(surface);
            require(layout.has_value(), "authored surface invalid");
            word(GX2SURF_DIM, surface.dim);
            word(GX2SURF_WIDTH, size);
            word(GX2SURF_HEIGHT, size);
            word(GX2SURF_DEPTH, layers);
            word(GX2SURF_MIP_LEVELS, 1);
            word(GX2SURF_FORMAT, 0x1a);
            word(GX2SURF_IMAGE_SIZE, layout->image_size);
            word(GX2SURF_IMAGE, address);
            word(GX2SURF_TILE_MODE, mode);
            word(GX2SURF_PITCH, layout->pitch);
            word(GX2TEX_VIEW_NUM_SLICES, layers);
            word(GX2TEX_COMP_MAP, 0x06880000);
            Gx2DetileDesc d;
            d.width = d.height = size;
            d.pitch = layout->pitch;
            d.surface_height = layout->padded_height;
            d.bits_per_element = 32;
            d.tile_mode = mode;
            std::vector<std::vector<uint8_t>> expected(layers, std::vector<uint8_t>(size_t(size) * size * 4));
            for (unsigned z = 0; z < layers; ++z) {
                d.slice = z;
                for (unsigned y = 0; y < size; ++y)
                    for (unsigned x = 0; x < size; ++x) {
                        const uint32_t value = (x * 13 + y * 17 + z * 43) | 0xff000000u;
                        auto off = original(d, x, y);
                        require(uint64_t(off) + 4 <= layout->image_size, "authored tiled write out of bounds");
                        std::memcpy(mem.data() + address + off, &value, 4);
                        std::memcpy(expected[z].data() + (size_t(y) * size + x) * 4, &value, 4);
                    }
            }
            gx2_render_bind_texture(descriptor, 0, mem.data());
            require(gx2vk::finish(), "warm upload failed");
            std::vector<double> samples;
            auto start = Clock::now();
            for (unsigned i = 0; i < 16; ++i) {
                const auto t = Clock::now();
                const uint8_t value = uint8_t(i + 71);
                mem[address] = value;
                expected[0][0] = value;
                gx2_render_invalidate_textures(address, layout->image_size);
                gx2_render_bind_texture(descriptor, 0, mem.data());
                gx2vk::submit(false);
                samples.push_back(ms(t));
            }
            require(gx2vk::finish(), "upload completion failed");
            const double total = ms(start);
            auto* texture = gx2r::state().units[0][layers > 1 ? 1 : 0];
            require(texture && texture->image, "texture missing after burst");
            for (unsigned z = 0; z < layers; ++z) {
                std::vector<uint8_t> pixels;
                require(gx2vk::read_image(*texture->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, z, size, size, pixels),
                        "upload readback failed");
                require(pixels == expected[z], "GPU texture bytes differ from authored input");
            }
            std::sort(samples.begin(), samples.end());
            std::printf("{\"metric\":\"texture_%u_mode_%u\",\"operations\":16,\"wall_ms\":%.6f,\"median_ms\":%.6f,"
                        "\"p95_ms\":%.6f,\"p99_ms\":%.6f,\"bytes_per_upload\":%zu}\n",
                        size, mode, total, samples[8], samples[15], samples[15], size_t(size) * size * 4 * layers);
        }
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX m{};
    m.cb = sizeof(m);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof(m));
    std::printf("{\"peak_commit_bytes\":%llu,\"gpu_owned_bytes\":%llu}\n", (unsigned long long)m.PeakPagefileUsage,
                (unsigned long long)gx2vk::allocated_bytes());
#endif
    require(!gx2vk::validation_error_count(), "Vulkan reported errors");
    gx2_render_shutdown();
    std::puts("{\"passed\":true}");
}
} // namespace
int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected layouts, uniforms, textures, or edgecases");
        const std::string_view mode = argv[1];
        if (mode == "layouts")
            layouts();
        else if (mode == "uniforms")
            uniforms();
        else if (mode == "textures")
            textures();
        else if (mode == "edgecases")
            edgecases();
        else
            throw std::runtime_error("unknown synthetic workload");
        return 0;
    } catch (const std::exception& e) {
        RBREW_LOG_ERROR("test", "FAIL: %s", e.what());
        return 1;
    }
}

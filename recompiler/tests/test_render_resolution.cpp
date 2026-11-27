#include "os/gx2/gx2_render.h"
#include "os/gx2/vk/gx2_present_fx.h"
#include "os/gx2/vk/gx2_render_vk_state.h"
#include "os/gx2/vk/gx2_sampler_anisotropy.h"
#include "os/gx2/vk/gx2_vk_shader.h"
#include "runtime/port_settings.h"
#include "rebrewu/logger.hpp"
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
using Descriptor = std::array<uint8_t, GX2DB_SIZEOF>;
void word(Descriptor& d, unsigned offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) d[offset + i] = uint8_t(value >> (24 - i * 8));
}
Descriptor surface(uint32_t address, uint32_t width, uint32_t height, uint32_t format, bool depth = false) {
    Descriptor d{};
    word(d, GX2SURF_DIM, 1);
    word(d, GX2SURF_WIDTH, width);
    word(d, GX2SURF_HEIGHT, height);
    word(d, GX2SURF_DEPTH, 1);
    word(d, GX2SURF_MIP_LEVELS, 1);
    word(d, GX2SURF_FORMAT, format);
    word(d, GX2SURF_IMAGE, address);
    word(d, GX2SURF_IMAGE_SIZE, width * height * 4);
    word(d, GX2SURF_USE, depth ? 4 : 2);
    word(d, GX2SURF_TILE_MODE, 1);
    word(d, GX2SURF_PITCH, width);
    word(d, 0x7C, 1);
    return d;
}
void draw_native_pixels(gx2r::ColorTarget& target) {
    const auto vs = gx2vk::shader_module(
        VK_SHADER_STAGE_VERTEX_BIT, "#version 450\nvoid main(){vec2 "
                                    "p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);gl_Position=vec4(p*2-1,0.5,1);}");
    const auto ps = gx2vk::shader_module(
        VK_SHADER_STAGE_FRAGMENT_BIT, "#version 450\nlayout(location=0)out vec4 c;void "
                                      "main(){c=vec4(float(int(gl_FragCoord.x)&1),float(int(gl_FragCoord.y)&1),0,1);}");
    require(vs && ps, "synthetic native-pixel shader failed");
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    require(vkCreatePipelineLayout(gx2vk::device(), &layout_info, nullptr, &layout) == VK_SUCCESS,
            "test pipeline layout failed");
    VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                              {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState masks[8]{};
    masks[0].colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 8;
    blend.pAttachments = masks;
    const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = states;
    VkFormat formats[8]{};
    formats[0] = target.image.desc.format;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 8;
    rendering.pColorAttachmentFormats = formats;
    rendering.depthAttachmentFormat = gx2r::state().depth_target->format;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &samples;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    require(vkCreateGraphicsPipelines(gx2vk::device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) == VK_SUCCESS,
            "test graphics pipeline failed");
    gx2_render_set_viewport(0, 0, 1280, 720, 0, 1);
    require(gx2r::ensure_rendering(), "native pixel render failed");
    const auto scale = gx2r::framebuffer_coordinate_scale();
    const auto& g = gx2r::state().raster;
    const auto vp = gx2r::resolution_viewport(g.viewport, true, g.depth_range, scale);
    const auto sc = gx2r::render_area();
    const auto cb = gx2vk::cmd();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    gx2r::end_rendering();
    gx2vk::note_gpu_write(target.image);
    std::vector<uint8_t> pixels;
    const auto width = target.image.desc.width, height = target.image.desc.height;
    require(gx2vk::read_image(target.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, width, height, pixels),
            "native pixel readback failed");
    for (uint32_t y = height - 4; y < height; ++y)
        for (uint32_t x = width - 16; x < width; ++x) {
            const auto* p = pixels.data() + (size_t(y) * width + x) * 4;
            require(p[0] == (x % 2 ? 255 : 0) && p[1] == (y % 2 ? 255 : 0) && p[3] == 255,
                    "GPU did not shade independent pixels across the native attachment (cropped or upscaled)");
        }
    vkDestroyPipeline(gx2vk::device(), pipeline, nullptr);
    vkDestroyPipelineLayout(gx2vk::device(), layout, nullptr);
    gx2_render_set_scissor(640, 360, 640, 360);
    gx2_render_clear_color(1, 0, 1, 1);
    require(gx2vk::read_image(target.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, width, height, pixels),
            "scaled scissor readback failed");
    const auto* corner = pixels.data() + (size_t(width) * height - 1) * 4;
    require(corner[0] == 255 && corner[1] == 0 && corner[2] == 255,
            "scissor clear did not reach native bottom-right corner");
    const uint32_t x = width / 2 - 1, y = height - 2;
    const auto* outside = pixels.data() + (size_t(y) * width + x) * 4;
    require(outside[0] == (x % 2 ? 255 : 0) && outside[1] == (y % 2 ? 255 : 0) && outside[2] == 0,
            "scissor clear changed pixels outside the guest rectangle");
}
void check_alias_history() {
    gx2_render_init();
    std::vector<uint8_t> mem(1u << 20);
    auto& state = gx2r::state();
    // Reused guest addresses have many historical shapes. Keep real GPU
    // images so the test exercises production selection and live writes.
    for (unsigned width = 1; width < 80; ++width) {
        const auto old = surface(0x10000, width, 8, 0x1A);
        gx2_render_set_color_buffer(1, mem.data(), old.data(), 0);
    }
    auto texture = surface(0x10000, 80, 8, 0x1A);
    gx2_render_set_color_buffer(1, mem.data(), texture.data(), 0);
    auto* target = state.color_targets[0];
    word(texture, GX2TEX_VIEW_NUM_MIPS, 1);
    word(texture, GX2TEX_VIEW_FIRST_SLICE, 0);
    word(texture, GX2TEX_VIEW_NUM_SLICES, 1);
    word(texture, GX2TEX_COMP_MAP, 0x06880000);
    gx2_render_bind_texture(1, 0, mem.data(), texture.data());
    require(state.units[0][0] && state.units[0][0]->image == &target->image, "historical texture view selected");
    const auto before = gx2_render_texture_alias_probe_count();
    for (unsigned i = 0; i < 2000; ++i) {
        gx2_render_bind_texture(1, i % 2, mem.data(), texture.data());
        require(state.units[i % 2][0] && state.units[i % 2][0]->image == &target->image,
                "view cache changed across texture units");
    }
    const auto probes = gx2_render_texture_alias_probe_count() - before;
    require(probes <= 2000, "binding rescanned incompatible historical render targets");
    // A negative result must be invalidated when a matching target appears.
    const auto added_surface = surface(0x10000, 81, 8, 0x1A);
    auto missing = added_surface;
    word(missing, GX2TEX_VIEW_NUM_MIPS, 1);
    word(missing, GX2TEX_VIEW_FIRST_SLICE, 0);
    word(missing, GX2TEX_VIEW_NUM_SLICES, 1);
    word(missing, GX2TEX_COMP_MAP, 0x06880000);
    gx2_render_bind_texture(1, 0, mem.data(), missing.data());
    gx2_render_set_color_buffer(1, mem.data(), added_surface.data(), 0);
    auto* added = state.color_targets[0];
    gx2_render_bind_texture(1, 0, mem.data(), missing.data());
    require(state.units[0][0]->image == &added->image, "negative view cache survived a new target");
    for (const float red : {0.25f, 0.75f}) {
        VkClearColorValue color{};
        color.float32[0] = red;
        color.float32[3] = 1;
        gx2r::clear_image_color(added->image, color);
        gx2_render_bind_texture(1, 0, mem.data(), missing.data());
        std::vector<uint8_t> pixels;
        require(gx2vk::read_image(*state.units[0][0]->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 81, 8, pixels),
                "alias readback failed");
        require(std::abs(int(pixels[0]) - int(std::lround(red * 255))) <= 1, "cached alias lost live GPU contents");
    }
    require(gx2vk::validation_error_count() == 0, "alias lookup caused Vulkan validation errors");
    gx2_render_shutdown();
    std::printf("Alias history: 2000 binds, %llu compatible probes; insertion and live pixels preserved\n",
                (unsigned long long)probes);
}
void check_resolution(VkExtent2D display) {
    gx2_render_init();
    require(gx2_render_set_display_resolution(display.width, display.height), "display selection failed");
    std::vector<uint8_t> mem(16u << 20);
    const auto main = surface(0x10000, 1280, 720, 0x1A);
    gx2_render_set_color_buffer(1, mem.data(), main.data(), 0);
    auto& s = gx2r::state();
    auto* target = s.color_targets[0];
    require(target && target->image.image, "main color target missing");
    require(target->image.desc.width == display.width && target->image.desc.height == display.height,
            "GPU color attachment remains at console resolution instead of display resolution");
    require(target->width == 1280 && target->height == 720, "guest dimensions were changed");
    const auto depth = surface(0x400000, 1280, 720, 0x80E, true);
    gx2_render_set_depth_buffer(1, mem.data(), depth.data());
    require(s.depth_target && s.depth_target->image.desc.width == display.width &&
                s.depth_target->image.desc.height == display.height,
            "GPU depth attachment differs from display resolution");
    require(gx2r::ensure_rendering(), "native-sized rendering could not begin");
    require(gx2r::render_area().extent.width == display.width && gx2r::render_area().extent.height == display.height,
            "GPU render area still clips to console dimensions");
    gx2r::end_rendering();
    draw_native_pixels(*target);
    auto texture = main;
    word(texture, GX2TEX_VIEW_NUM_MIPS, 1);
    word(texture, GX2TEX_VIEW_FIRST_SLICE, 0);
    word(texture, GX2TEX_VIEW_NUM_SLICES, 1);
    word(texture, GX2TEX_COMP_MAP, 0x06880000);
    gx2_render_bind_texture(1, 0, mem.data(), texture.data());
    const auto* sampled = s.units[0][0];
    require(sampled && sampled->image == &target->image, "sampling lost the native render target");
    require(sampled->coordinate_scale[0] == float(display.width) / 1280 &&
                sampled->coordinate_scale[1] == float(display.height) / 720,
            "texel-fetch coordinates were not adjusted for native render targets");
    auto array = texture;
    word(array, GX2SURF_DIM, 5);
    word(array, GX2SURF_DEPTH, 2);
    word(array, GX2SURF_IMAGE_SIZE, 1280 * 720 * 8);
    word(array, GX2TEX_VIEW_NUM_SLICES, 2);
    gx2_render_bind_texture(1, 1, mem.data(), array.data());
    auto* array_texture = s.units[1][1];
    require(array_texture && array_texture->image, "color array alias missing");
    std::vector<uint8_t> array_pixels;
    require(gx2vk::read_image(*array_texture->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1280, 720, array_pixels),
            "color array alias readback failed");
    const auto* array_corner = array_pixels.data() + (1280 * 720 - 1) * 4;
    require(array_corner[0] == 255 && array_corner[2] == 255, "array alias cropped a scaled color target");
    const auto copied = surface(0x500000, 1280, 720, 0x1A);
    gx2_render_copy_surface(main.data(), 0, 0, copied.data(), 0, 0, mem.data());
    for (auto& [key, view] : s.cb_map)
        if (key[0] == 0x500000) {
            std::vector<uint8_t> pixels;
            require(
                gx2vk::read_image(view.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, display.width, display.height, pixels),
                "scaled surface copy failed");
            const auto* p = pixels.data() + (size_t(display.width) * display.height - 1) * 4;
            require(p[0] == 255 && p[2] == 255, "surface copy cropped the native-resolution image");
        }
    auto cpu = surface(0xC00000, 1280, 720, 0x1A);
    word(cpu, GX2SURF_TILE_MODE, 16);
    gx2_render_copy_surface(main.data(), 0, 0, cpu.data(), 0, 0, mem.data());
    const auto* cpu_corner = mem.data() + 0xC00000 + (1280 * 720 - 1) * 4;
    require(cpu_corner[0] == 255 && cpu_corner[2] == 255 && cpu_corner[3] == 255,
            "CPU surface copy broke guest dimensions or packing");
    gx2_render_set_scan_format(false, 0x41A);
    gx2_render_copy_to_scan(1, 1, mem.data(), main.data());
    require(s.scan_w[0] == display.width && s.scan_h[0] == display.height,
            "scan-out crops the native-resolution scene");
    gx2r::clear_image_depth(s.depth_target->image, 0.375f, 0);
    const auto numeric = surface(0x600000, 1280, 720, 0x80E);
    gx2_render_set_color_buffer(1, mem.data(), numeric.data(), 0);
    auto* converted = s.color_targets[0];
    require(gx2r::depth_to_color_pass(*s.depth_target, *converted, 1280, 720), "scaled depth-to-color pass failed");
    std::vector<uint8_t> depth_pixels;
    require(gx2vk::read_image(converted->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, display.width, display.height,
                              depth_pixels),
            "scaled depth-to-color readback failed");
    float last_depth = 0;
    std::memcpy(&last_depth, depth_pixels.data() + depth_pixels.size() - 4, 4);
    require(std::abs(last_depth - 0.375f) < 0.00001f, "depth-to-color pass left the native-resolution edge unrendered");
    auto depth_array = depth;
    word(depth_array, GX2SURF_DIM, 5);
    word(depth_array, GX2SURF_DEPTH, 2);
    word(depth_array, GX2TEX_VIEW_NUM_MIPS, 1);
    word(depth_array, GX2TEX_VIEW_FIRST_SLICE, 0);
    word(depth_array, GX2TEX_VIEW_NUM_SLICES, 2);
    word(depth_array, GX2TEX_COMP_MAP, 0x06880000);
    gx2_render_bind_texture(1, 2, mem.data(), depth_array.data());
    auto* depth_sample = s.units[2][1];
    require(depth_sample && depth_sample->image && depth_sample->depth, "depth array alias missing");
    require(gx2vk::read_image(*depth_sample->image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1280, 720, depth_pixels),
            "depth array alias readback failed");
    std::memcpy(&last_depth, depth_pixels.data() + depth_pixels.size() - 4, 4);
    require(std::abs(last_depth - 0.375f) < 0.00001f, "array alias cropped a scaled depth target");
    require(gx2r::depth_to_depth_pass(*s.depth_target, *depth_sample->image, 0, 1), "shader depth resampling failed");
    require(gx2vk::read_image(*depth_sample->image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 1280, 720, depth_pixels),
            "resampled depth slice readback failed");
    std::memcpy(&last_depth, depth_pixels.data() + depth_pixels.size() - 4, 4);
    require(std::abs(last_depth - 0.375f) < 0.00001f, "depth resampling missed the selected array slice");
    const auto half = surface(0x800000, 640, 360, 0x1A);
    gx2_render_set_color_buffer(1, mem.data(), half.data(), 0);
    require(s.color_targets[0]->image.desc.width == display.width / 2 &&
                s.color_targets[0]->image.desc.height == display.height / 2,
            "post-processing target did not follow display resolution");
    const auto shadow = surface(0x900000, 512, 512, 0x80E, true);
    gx2_render_set_depth_buffer(1, mem.data(), shadow.data());
    require(s.depth_target->image.desc.width == 512 && s.depth_target->image.desc.height == 512,
            "shadow/data allocation was resized");
    const auto blocks = surface(0xA00000, 320, 180, 0x122);
    gx2_render_set_color_buffer(1, mem.data(), blocks.data(), 0);
    require(s.color_targets[0]->image.desc.width == 320 && s.color_targets[0]->image.desc.height == 180,
            "integer/block data allocation was resized");
    if (display.width >= 1280 && display.height >= 720) {
        const auto drc_scan = surface(0xB00000, 854, 480, 0x1A);
        gx2_render_set_color_buffer(1, mem.data(), drc_scan.data(), 0);
        require(s.color_targets[0]->image.desc.width == display.width &&
                    s.color_targets[0]->image.desc.height == display.height && s.color_targets[0]->width == 854 &&
                    s.color_targets[0]->height == 480,
                "GamePad output target did not scale to display resolution");
        gx2_render_set_scan_format(true, 0x41A);
        gx2_render_copy_to_scan(0xB00000, 4, mem.data(), drc_scan.data());
        require(s.scan_w[1] == display.width && s.scan_h[1] == display.height,
                "GamePad scan-out stays at console resolution instead of display resolution");
        const auto drc_color = surface(0xB10000, 848, 480, 0x1A);
        gx2_render_set_color_buffer(1, mem.data(), drc_color.data(), 0);
        require(s.color_targets[0]->image.desc.width == display.width &&
                    s.color_targets[0]->image.desc.height == display.height,
                "GamePad 3D color target did not scale to display resolution");
        const auto drc_depth = surface(0xB20000, 848, 480, 0x80E, true);
        gx2_render_set_depth_buffer(1, mem.data(), drc_depth.data());
        require(s.depth_target->image.desc.width == display.width &&
                    s.depth_target->image.desc.height == display.height,
                "GamePad 3D depth target did not scale to display resolution");
        for (const uint32_t hw : {427u, 424u}) {
            const auto drc_half = surface(0xB30000 + hw, hw, 240, 0x1A);
            gx2_render_set_color_buffer(1, mem.data(), drc_half.data(), 0);
            require(s.color_targets[0]->image.desc.width == std::max(1u, display.width >> 1) &&
                        s.color_targets[0]->image.desc.height == std::max(1u, display.height >> 1),
                    "GamePad post-processing target did not follow display resolution");
        }
    }
    require(!gx2_render_set_display_resolution(800, 600), "startup-only resolution changed live cached targets");
    require(gx2vk::finish(), "native-resolution GPU commands failed");
    gx2_render_shutdown();
    std::printf("GPU attachments, depth, post-processing and scan-out: %ux%u\n", display.width, display.height);
}
void check_anisotropy_and_8k_scaling() {
    gx2_render_init();
    const VkExtent2D ext8k{7680, 4320};
    require(gx2r::screen_target_extent(1280, 720, ext8k).width == 7680 &&
                gx2r::screen_target_extent(1280, 720, ext8k).height == 4320,
            "8K scaling of main 720p target failed");
    require(gx2r::screen_target_extent(640, 360, ext8k).width == 3840 &&
                gx2r::screen_target_extent(640, 360, ext8k).height == 2160,
            "8K scaling of half-res target failed");
    require(gx2r::screen_target_extent(320, 180, ext8k).width == 1920 &&
                gx2r::screen_target_extent(320, 180, ext8k).height == 1080,
            "8K scaling of quarter-res target failed");
    require(gx2r::screen_target_extent(854, 480, ext8k).width == 7680 &&
                gx2r::screen_target_extent(854, 480, ext8k).height == 4320,
            "8K scaling of GamePad output target failed");
    const auto lim = gx2vk::caps().limits;
    const bool ok8k = gx2_render_set_display_resolution(7680, 4320);
    const bool supported8k = 7680 <= lim.maxImageDimension2D && 4320 <= lim.maxImageDimension2D &&
                             7680 <= lim.maxFramebufferWidth && 4320 <= lim.maxFramebufferHeight;
    require(ok8k == supported8k, "8K display resolution acceptance disagreed with Vulkan device limits");
    require(!gx2_render_set_display_resolution(lim.maxImageDimension2D + 1u, 720),
            "resolution exceeding maxImageDimension2D was accepted");
    gx2_render_shutdown();

    using gx2_anisotropy::effective_anisotropy;
    require(effective_anisotropy(4.0f, true, true, false, false, 0, true, 16.0f) == 4.0f,
            "AF=0 must preserve guest anisotropy");
    require(effective_anisotropy(1.0f, true, true, false, false, 16, true, 16.0f) == 16.0f,
            "AF=16 did not upgrade eligible mipmapped linear color sampler");
    require(effective_anisotropy(1.0f, true, true, false, false, 16, true, 8.0f) == 8.0f,
            "forced AF must clamp to device maxAnisotropy");
    require(effective_anisotropy(1.0f, true, true, true, false, 16, true, 16.0f) == 1.0f,
            "forced AF must not apply to shadow samplers");
    require(effective_anisotropy(1.0f, true, true, false, true, 16, true, 16.0f) == 1.0f,
            "forced AF must not apply to integer textures");
    require(effective_anisotropy(1.0f, false, true, false, false, 16, true, 16.0f) == 1.0f,
            "forced AF must not apply to non-mipmapped samplers");
    require(effective_anisotropy(1.0f, true, false, false, false, 16, true, 16.0f) == 1.0f,
            "forced AF must not apply to nearest-filtered samplers");
    require(effective_anisotropy(16.0f, true, true, false, false, 16, false, 16.0f) == 1.0f,
            "AF must stay off when device anisotropy is unsupported");
}
void check_present_fx_gpu() {
    gx2_render_init();
    constexpr uint32_t kW = 128, kH = 72;
    gx2vk::Image src;
    gx2vk::ImageDesc desc{};
    desc.format = VK_FORMAT_R8G8B8A8_UNORM;
    desc.width = kW;
    desc.height = kH;
    desc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    require(gx2vk::create_image(src, desc), "present FX test source image creation failed");

    auto span = gx2vk::upload(size_t(kW) * kH * 4, 16);
    require(span.data != nullptr, "present FX upload allocation failed");
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            const bool hi = (int(x) - int(y) * 2) > 0;
            uint8_t* p = span.data + (size_t(y) * kW + x) * 4;
            p[0] = hi ? 240 : 20;
            p[1] = hi ? 240 : 20;
            p[2] = hi ? 240 : 20;
            p[3] = 255;
        }
    }
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kW, kH, 1};
    gx2vk::barrier_if_dirty();
    vkCmdCopyBufferToImage(gx2vk::cmd(), span.buffer, src.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    gx2vk::note_gpu_write(src);

    // 1. Off + Off fast path.
    const auto off =
        gx2_present_fx::apply_explicit(src, kW, kH, 256, 144, rbrew_settings::kAAOff, rbrew_settings::kUpscalerOff, 70);
    require(off.image == nullptr, "present FX Off+Off did not take fast path");

    // 2. FXAA + Off (same resolution).
    const auto fxaa =
        gx2_present_fx::apply_explicit(src, kW, kH, kW, kH, rbrew_settings::kAAFXAA, rbrew_settings::kUpscalerOff, 70);
    require(fxaa.image && fxaa.width == kW && fxaa.height == kH, "FXAA output invalid");
    std::vector<uint8_t> pixels;
    require(gx2vk::read_image(const_cast<gx2vk::Image&>(*fxaa.image), VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, kW, kH, pixels),
            "FXAA readback failed");
    require(pixels[(2 * kW + 100) * 4] >= 230 && pixels[(50 * kW + 4) * 4] <= 30,
            "FXAA altered flat light/dark regions");

    // 3. SMAA + Off (same resolution, 3-pass).
    const auto smaa =
        gx2_present_fx::apply_explicit(src, kW, kH, kW, kH, rbrew_settings::kAASMAA, rbrew_settings::kUpscalerOff, 70);
    require(smaa.image && smaa.width == kW && smaa.height == kH, "SMAA output invalid");
    require(gx2vk::read_image(const_cast<gx2vk::Image&>(*smaa.image), VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, kW, kH, pixels),
            "SMAA readback failed");
    require(pixels[(2 * kW + 100) * 4] >= 230 && pixels[(50 * kW + 4) * 4] <= 30,
            "SMAA altered flat light/dark regions");

    // 4. FSR 1.0 (EASU + RCAS 2x upscale, and RCAS-only 1x).
    const auto fsr2x = gx2_present_fx::apply_explicit(src, kW, kH, kW * 2, kH * 2, rbrew_settings::kAAOff,
                                                      rbrew_settings::kUpscalerFSR, 80);
    require(fsr2x.image && fsr2x.width == kW * 2 && fsr2x.height == kH * 2, "FSR 2x output invalid");
    require(gx2vk::read_image(const_cast<gx2vk::Image&>(*fsr2x.image), VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, kW * 2, kH * 2,
                              pixels),
            "FSR 2x readback failed");
    require(pixels[(4 * (kW * 2) + 200) * 4] >= 230 && pixels[(100 * (kW * 2) + 8) * 4] <= 30,
            "FSR 2x distorted flat regions");
    const auto fsr1x =
        gx2_present_fx::apply_explicit(src, kW, kH, kW, kH, rbrew_settings::kAAOff, rbrew_settings::kUpscalerFSR, 80);
    require(fsr1x.image && fsr1x.width == kW && fsr1x.height == kH, "FSR 1x RCAS output invalid");

    // 5. SMAA + Anime4K 2x upscale.
    const auto a4k = gx2_present_fx::apply_explicit(src, kW, kH, kW * 2, kH * 2, rbrew_settings::kAASMAA,
                                                    rbrew_settings::kUpscalerAnime4K, 75);
    require(a4k.image && a4k.width == kW * 2 && a4k.height == kH * 2, "Anime4K 2x output invalid");
    require(gx2vk::read_image(const_cast<gx2vk::Image&>(*a4k.image), VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, kW * 2, kH * 2,
                              pixels),
            "Anime4K 2x readback failed");
    require(pixels[(4 * (kW * 2) + 200) * 4] >= 230 && pixels[(100 * (kW * 2) + 8) * 4] <= 30,
            "Anime4K 2x distorted flat regions");

    require(gx2vk::validation_error_count() == 0, "present FX passes caused Vulkan validation errors");
    gx2_present_fx::shutdown();
    gx2vk::destroy_image(src);
    gx2_render_shutdown();
    std::printf("Present FX GPU: FXAA, SMAA, FSR 1.0 (EASU+RCAS), Anime4K passed with 0 validation errors\n");
}
} // namespace
int main(int argc, char**) {
    try {
        if (argc > 1) {
            SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
            require(SDL_Init(SDL_INIT_VIDEO) == 0, "SDL display enumeration failed");
            const int count = SDL_GetNumVideoDisplays();
            require(count > 0, "no desktop display found");
            for (int display = 0; display < count; ++display) {
                SDL_DisplayMode mode{};
                require(SDL_GetDesktopDisplayMode(display, &mode) == 0, "desktop mode query failed");
                std::printf("Display %d: %dx%d physical pixels, %d Hz\n", display, mode.w, mode.h, mode.refresh_rate);
            }
            SDL_Quit();
            return 0;
        }
        if (!gx2vk::init(nullptr)) return 77;
        check_anisotropy_and_8k_scaling();
        check_present_fx_gpu();
        check_alias_history();
        for (const auto display :
             {VkExtent2D{1920, 1080}, {1366, 768}, {2560, 1440}, {3840, 2160}, {1024, 768}, {960, 540}, {1280, 720}})
            check_resolution(display);
        return 0;
    } catch (const std::exception& error) {
        RBREW_LOG_ERROR("test", "FAIL: %s", error.what());
        return 1;
    }
}

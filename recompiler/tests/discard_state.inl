// Authored transform-feedback draw followed by a raster draw with unchanged
// depth/stencil registers. Both use the production draw and pipeline paths.
void gx2_render_vk_test_discard_state() {
    using render_stress::require;
    Program program;
    program.id = UINT32_MAX - 7;
    for (auto& info : program.info) {
        info.attribute_locations.fill(-1);
        info.buffer_bindings.fill(-1);
        info.vk.uniform_buffer_bindings.fill(-1);
        info.vk.texture_bindings.fill(-1);
        info.vk.offset_tex_scale.fill(-1);
    }
    program.info[0].streamout_mask = 1;
    program.info[0].streamout_strides[0] = 16;
    program.info[1].vk.pixel_outputs = 1;
    program.vs = gx2vk::shader_module(
        VK_SHADER_STAGE_VERTEX_BIT,
        "#version 450\nlayout(location=0,xfb_buffer=0,xfb_offset=0,xfb_stride=16)out vec4 exported;"
        "void main(){vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);"
        "gl_Position=vec4(p*2.-1.,0.5,1.);exported=gl_Position;}\n");
    program.ps_source = "#version 450\nlayout(location=0)out vec4 c;void main(){c=vec4(1.);}\n";
    require(program.vs && create_program_layouts(program, true), "discard regression shader setup failed");
    ColorTarget color;
    color.width = color.height = 32;
    gx2vk::ImageDesc desc;
    desc.width = desc.height = 32;
    desc.usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    require(gx2vk::create_image(color.image, desc), "discard regression color target failed");
    VkClearColorValue black{};
    clear_image_color(color.image, black);
    DepthTarget depth;
    depth.width = depth.height = 32;
    depth.format = desc.format = VK_FORMAT_D32_SFLOAT;
    desc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    require(gx2vk::create_image(depth.image, desc), "discard regression depth target failed");
    clear_image_depth(depth.image, 1.f, 0);
    S.raster = RasterState{};
    S.raster.viewport[2] = S.raster.viewport[3] = 32;
    S.raster.cull = false;
    S.framebuffer = {};
    S.framebuffer.colors[0] = &color;
    S.framebuffer.depth = &depth;
    S.framebuffer.valid = true;
    S.target_masks = 1;
    S.draw_instances = 1;
    S.draw_first_instance = 0;
    std::vector<uint8_t> memory(8192);
    S.regs[0xA2AC] = 1;
    S.regs[0xA204] = 1u << 22;
    S.regs[0xA2C8] = 1;
    S.regs[0xA2B4] = 128 / 4;
    S.regs[0xA2B6] = 4096 / 256;
    S.regs[0xA2B7] = 0;
    reset_command_tracking();
    IndexStream indices;
    require(issue_draw_program(&program, memory.data(), 4, 3, 0, indices, false, nullptr), "discard draw failed");
    S.regs[0xA2AC] = S.regs[0xA204] = 0;
    require(issue_draw_program(&program, memory.data(), 4, 3, 0, indices, false, nullptr), "draw after discard failed");
    end_rendering();
    flush_streamouts(memory.data());
    std::vector<uint8_t> pixels;
    require(gx2vk::read_image(color.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 32, 32, pixels),
            "discard regression readback failed");
    const auto center = (16 * 32 + 16) * 4;
    require(pixels[center] == 255 && pixels[center + 1] == 255 && pixels[center + 2] == 255,
            "unchanged depth/stencil registers lost the draw after discard");
    S.framebuffer = {};
    gx2vk::destroy_image(color.image);
    gx2vk::destroy_image(depth.image);
    require(gx2vk::finish(), "discard regression cleanup failed");
    release_program_layouts(program);
    RBREW_LOG_INFO("test", "Depth/stencil state survives a discarded particle update");
}

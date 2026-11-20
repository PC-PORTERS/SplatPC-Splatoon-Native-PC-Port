void verify_discard_state_transitions() {
    using render_stress::require;
    Program program;
    program.id = UINT32_MAX - 5;
    for (auto& info : program.info) {
        info.attribute_locations.fill(-1);
        info.buffer_bindings.fill(-1);
        info.vk.uniform_buffer_bindings.fill(-1);
        info.vk.texture_bindings.fill(-1);
    }
    program.info[1].vk.pixel_outputs = 1;
    program.vs = gx2vk::shader_module(VK_SHADER_STAGE_VERTEX_BIT,
                                      "#version 450\nvoid main(){vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);"
                                      "gl_Position=vec4(p*2.-1.,0.25,1.);}");
    program.ps_source = "#version 450\nlayout(location=0)out vec4 c;void main(){c=vec4(1.);}";
    require(program.vs && create_program_layouts(program), "discard transition shaders failed");
    ColorTarget color;
    DepthTarget depth;
    color.width = color.height = depth.width = depth.height = 8;
    depth.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    depth.stencil = true;
    gx2vk::ImageDesc desc;
    desc.width = desc.height = 8;
    desc.usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    require(gx2vk::create_image(color.image, desc), "discard transition color allocation failed");
    desc.format = depth.format;
    desc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    require(gx2vk::create_image(depth.image, desc), "discard transition depth allocation failed");
    VkClearColorValue zero{};
    clear_image_color(color.image, zero);
    clear_image_depth(depth.image, 0.75f, 0);
    S.framebuffer = {};
    S.framebuffer.valid = true;
    S.framebuffer.colors[0] = &color;
    S.framebuffer.depth = &depth;
    S.raster = RasterState{};
    S.raster.viewport[2] = S.raster.viewport[3] = 8;
    S.raster.depth_test = true;
    S.raster.depth_write = false;
    S.raster.depth_func = wire::LESS;
    S.raster.cull = false;
    S.target_masks = 15;
    S.draw_instances = 1;
    S.draw_first_instance = 0;
    S.regs[0xA204] = 1u << 19;
    S.regs[0xA2AC] = 0;
    IndexStream indices;
    indices.indexed = true;
    indices.count = 3;
    indices.type = VK_INDEX_TYPE_UINT16;
    indices.span = gx2vk::upload(6, 2);
    require(indices.span.data, "discard transition indices failed");
    const uint16_t values[3] = {0, 1, 2};
    std::memcpy(indices.span.data, values, sizeof(values));
    require(issue_draw_program(&program, nullptr, 4, 3, 0, indices, false, nullptr), "pre-discard draw failed");

    PipelineRequest request{};
    request.program = &program;
    request.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    const auto pipeline = create_pipeline(request, {true, true, true});
    require(pipeline && ensure_rendering(true), "discard pipeline failed");
    const auto cb = gx2vk::cmd();
    require(apply_dynamic_state(cb, request.topology, false, false, true), "discard state failed");
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdDraw(cb, 3, 1, 0, 0);
    const auto raster = S.raster;
    S.raster.depth_test = false;
    S.raster.depth_write = true;
    S.raster.depth_func = wire::GREATER;
    S.raster.stencil_test = true;
    require(apply_dynamic_state(cb, request.topology, false, false, true), "repeated discard state failed");
    vkCmdDraw(cb, 3, 1, 0, 0);
    S.raster = raster;
    require(issue_draw_program(&program, nullptr, 4, 3, 0, indices, false, nullptr), "post-discard draw failed");
    end_rendering();
    std::vector<uint8_t> pixels;
    require(gx2vk::read_image(color.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 8, 8, pixels),
            "discard transition readback failed");
    require(pixels.size() == 256 && std::all_of(pixels.begin(), pixels.end(), [](uint8_t v) { return v == 255; }),
            "post-discard raster state was not restored");
    require(gx2vk::validation_error_count() == 0, "discard transition validation failed");
    S.framebuffer = {};
    vkDestroyPipeline(gx2vk::device(), pipeline, nullptr);
    gx2vk::destroy_image(color.image);
    gx2vk::destroy_image(depth.image);
    release_program_layouts(program);
    reset_command_tracking();
}

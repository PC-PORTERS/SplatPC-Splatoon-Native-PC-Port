// Included only in test-enabled runtime builds, beside the actual draw helpers.
// This bypasses guest shader lookup only: every draw uses the production vertex,
// pipeline, uniform, texture, descriptor, dynamic-state and submission paths.
namespace render_stress {
struct Counts {
    uint64_t sets = 0, updates = 0, pools = 0, resets = 0, binds = 0, indices = 0, polls = 0, pipelines = 0;
} counts;
bool lifetime = false, gate_submitted = false;
VkSemaphore gate = VK_NULL_HANDLE;
VkResult fail_allocate = VK_SUCCESS;
std::vector<uint32_t> last_dynamic_offsets;
PFN_vkQueueSubmit2 submit;
PFN_vkAllocateDescriptorSets allocate;
PFN_vkUpdateDescriptorSets update;
PFN_vkCreateDescriptorPool pool;
PFN_vkResetDescriptorPool reset;
PFN_vkCmdBindDescriptorSets bind;
PFN_vkCmdBindIndexBuffer index;
PFN_vkGetSemaphoreCounterValue poll;
PFN_vkCmdBindPipeline pipeline;
VKAPI_ATTR VkResult VKAPI_CALL count_allocate(VkDevice d, const VkDescriptorSetAllocateInfo* i, VkDescriptorSet* s) {
    ++counts.sets;
    return allocate(d, i, s);
}
VKAPI_ATTR VkResult VKAPI_CALL inspect_allocate(VkDevice d, const VkDescriptorSetAllocateInfo* i, VkDescriptorSet* s) {
    if (fail_allocate != VK_SUCCESS) {
        ++counts.sets;
        auto r = fail_allocate;
        fail_allocate = VK_SUCCESS;
        return r;
    }
    return count_allocate(d, i, s);
}
VKAPI_ATTR void VKAPI_CALL count_update(VkDevice d, uint32_t n, const VkWriteDescriptorSet* w, uint32_t c,
                                        const VkCopyDescriptorSet* x) {
    ++counts.updates;
    update(d, n, w, c, x);
}
VKAPI_ATTR VkResult VKAPI_CALL count_pool(VkDevice d, const VkDescriptorPoolCreateInfo* i,
                                          const VkAllocationCallbacks* a, VkDescriptorPool* p) {
    ++counts.pools;
    return pool(d, i, a, p);
}
VKAPI_ATTR VkResult VKAPI_CALL count_reset(VkDevice d, VkDescriptorPool p, VkDescriptorPoolResetFlags f) {
    ++counts.resets;
    return reset(d, p, f);
}
VKAPI_ATTR void VKAPI_CALL count_bind(VkCommandBuffer c, VkPipelineBindPoint p, VkPipelineLayout l, uint32_t f,
                                      uint32_t n, const VkDescriptorSet* s, uint32_t a, const uint32_t* o) {
    ++counts.binds;
    bind(c, p, l, f, n, s, a, o);
}
VKAPI_ATTR void VKAPI_CALL inspect_bind(VkCommandBuffer c, VkPipelineBindPoint p, VkPipelineLayout l, uint32_t f,
                                        uint32_t n, const VkDescriptorSet* s, uint32_t a, const uint32_t* o) {
    last_dynamic_offsets.assign(o, o + a);
    count_bind(c, p, l, f, n, s, a, o);
}
VKAPI_ATTR void VKAPI_CALL count_index(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkIndexType t) {
    ++counts.indices;
    index(c, b, o, t);
}
VKAPI_ATTR VkResult VKAPI_CALL count_poll(VkDevice d, VkSemaphore s, uint64_t* v) {
    ++counts.polls;
    return poll(d, s, v);
}
VKAPI_ATTR void VKAPI_CALL count_pipeline(VkCommandBuffer c, VkPipelineBindPoint p, VkPipeline s) {
    ++counts.pipelines;
    pipeline(c, p, s);
}
VKAPI_ATTR VkResult VKAPI_CALL count_submit(VkQueue q, uint32_t n, const VkSubmitInfo2* info, VkFence f) {
    if (!gate || gate_submitted || n != 1) return submit(q, n, info, f);
    gate_submitted = true;
    VkSubmitInfo2 copy = *info;
    std::vector<VkSemaphoreSubmitInfo> waits;
    for (unsigned i = 0; i < copy.waitSemaphoreInfoCount; ++i) waits.push_back(copy.pWaitSemaphoreInfos[i]);
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = gate;
    wait.value = 1;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    waits.push_back(wait);
    copy.pWaitSemaphoreInfos = waits.data();
    copy.waitSemaphoreInfoCount = uint32_t(waits.size());
    return submit(q, 1, &copy, f);
}
struct Hooks {
    Hooks() {
        counts = {};
        allocate = vkAllocateDescriptorSets;
        update = vkUpdateDescriptorSets;
        pool = vkCreateDescriptorPool;
        reset = vkResetDescriptorPool;
        bind = vkCmdBindDescriptorSets;
        index = vkCmdBindIndexBuffer;
        poll = vkGetSemaphoreCounterValue;
        pipeline = vkCmdBindPipeline;
        submit = vkQueueSubmit2;
        vkAllocateDescriptorSets = count_allocate;
        vkUpdateDescriptorSets = count_update;
        vkCreateDescriptorPool = count_pool;
        vkResetDescriptorPool = count_reset;
        vkCmdBindDescriptorSets = count_bind;
        vkCmdBindIndexBuffer = count_index;
        vkGetSemaphoreCounterValue = count_poll;
        vkCmdBindPipeline = count_pipeline;
        vkQueueSubmit2 = lifetime ? count_submit : submit;
    }
    ~Hooks() {
        vkAllocateDescriptorSets = allocate;
        vkUpdateDescriptorSets = update;
        vkCreateDescriptorPool = pool;
        vkResetDescriptorPool = reset;
        vkCmdBindDescriptorSets = bind;
        vkCmdBindIndexBuffer = index;
        vkGetSemaphoreCounterValue = poll;
        vkCmdBindPipeline = pipeline;
        vkQueueSubmit2 = submit;
    }
};
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}
} // namespace render_stress

void gx2_render_vk_test_stress(bool push, bool churn) {
    using namespace render_stress;
    Program program;
    program.id = UINT32_MAX - 1;
    for (auto& info : program.info) {
        info.attribute_locations.fill(-1);
        info.buffer_bindings.fill(-1);
        info.vk.uniform_buffer_bindings.fill(-1);
        info.vk.texture_bindings.fill(-1);
        info.vk.offset_tex_scale.fill(-1);
    }
    program.info[0].attribute_locations[200] = 0;
    auto& ps = program.info[1];
    ps.uniform_mode = 2;
    ps.vk.uniform_vars_binding = 0;
    ps.vk.uniform_block_bytes = 16;
    ps.vk.offset_registers = 0;
    ps.vk.register_vec4s = 1;
    ps.vk.pixel_outputs = 1;
    ps.vk.texture_bindings[0] = 1;
    ps.vk.texture_dim[0] = 2;
    program.vs = gx2vk::shader_module(
        VK_SHADER_STAGE_VERTEX_BIT,
        "#version 450\nlayout(location=0)in uvec4 a;void main(){vec2 "
        "p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);gl_Position=vec4(p*2.-1.+float(a.x)*1e-10,0.,1.);}\n");
    program.ps_source = "#version 450\nlayout(set=0,binding=0)uniform U{vec4 tint;};layout(set=0,binding=1)uniform "
                        "sampler2D t;layout(location=0)out vec4 c;void main(){c=tint*texelFetch(t,ivec2(0),0);}\n";
    require(program.vs && create_program_layouts(program, push), "authored draw layout failed");
    std::array<ColorTarget, 2> targets;
    std::array<gx2vk::Image, 32> images;
    std::array<TexObject, 32> textures;
    for (auto& t : targets) {
        t.width = 64;
        t.height = 32;
        gx2vk::ImageDesc d;
        d.width = 64;
        d.height = 32;
        d.usage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        require(gx2vk::create_image(t.image, d), "target allocation failed");
        VkClearColorValue zero{};
        clear_image_color(t.image, zero);
    }
    for (unsigned i = 0; i < images.size(); ++i) {
        gx2vk::ImageDesc d;
        d.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        require(gx2vk::create_image(images[i], d), "sampled texture allocation failed");
        VkClearColorValue color{};
        color.float32[0] = float((i & 1) != 0);
        color.float32[1] = float((i & 2) != 0);
        color.float32[2] = float((i & 4) != 0);
        color.float32[3] = 1;
        clear_image_color(images[i], color);
        textures[i].image = &images[i];
        textures[i].params.min_filter = textures[i].params.mag_filter = wire::NEAREST;
    }
    IndexStream indices;
    indices.indexed = true;
    indices.type = VK_INDEX_TYPE_UINT16;
    indices.count = 3;
    indices.span = gx2vk::upload(6, 2);
    const uint16_t data[3] = {0, 1, 2};
    require(indices.span.data, "index upload failed");
    std::memcpy(indices.span.data, data, 6);
    const float white[4] = {1, 1, 1, 1};
    std::memcpy(S.uniform_words[1].data(), white, 16);
    S.raster = RasterState{};
    S.raster.viewport[2] = 64;
    S.raster.viewport[3] = 32;
    S.raster.scissor_test = true;
    S.raster.scissor[2] = S.raster.scissor[3] = 1;
    S.draw_instances = 1;
    S.draw_first_instance = 0;
    S.target_masks = 1;
    S.framebuffer = {};
    S.framebuffer.colors[0] = &targets[0];
    S.framebuffer.valid = true;
    S.units[0][0] = &textures[0];
    // All four immutable pipelines are created before measuring draw overhead.
    for (unsigned i = 0; i < 4; ++i) {
        S.raster.blend = (i & 1) != 0;
        require(issue_draw_program(&program, nullptr, i & 2 ? 6 : 4, 3, 0, indices, false, nullptr),
                "warm draw failed");
    }
    end_rendering();
    require(gx2vk::finish(), "warm draw wait failed");
    std::array<std::array<std::array<uint8_t, 4>, 2048>, 2> expected{};
    std::vector<double> samples;
    const auto begin = Clock::now();
    const auto submissions = gx2vk::submission_count(), barriers = gx2vk::barrier_count();
    {
        Hooks hooks;
        uint64_t first_resets = 0;
        for (unsigned batch = 0; batch < 16; ++batch) {
            if (lifetime) {
                // Replace a texture while its earlier descriptor may still be
                // in flight, then upload through the real mapped ring.
                gx2vk::destroy_image(images[batch]);
                gx2vk::ImageDesc d;
                d.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                require(gx2vk::create_image(images[batch], d), "in-flight replacement allocation failed");
                const uint8_t color[4] = {uint8_t(batch & 1 ? 255 : 0), uint8_t(batch & 2 ? 255 : 0),
                                          uint8_t(batch & 4 ? 255 : 0), 255};
                auto span = gx2vk::upload(4, 4);
                require(span.data, "texture ring upload failed");
                std::memcpy(span.data, color, 4);
                gx2vk::barrier_if_dirty();
                VkBufferImageCopy copy{};
                copy.bufferOffset = span.offset;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {1, 1, 1};
                vkCmdCopyBufferToImage(gx2vk::cmd(), span.buffer, images[batch].image, VK_IMAGE_LAYOUT_GENERAL, 1,
                                       &copy);
                gx2vk::note_gpu_write(images[batch]);
            }
            require(gx2vk::span_alive(indices.span), "index ring span recycled prematurely");
            S.framebuffer.colors[0] = &targets[batch & 1];
            const auto start = Clock::now();
            for (unsigned j = 0; j < 1024; ++j) {
                const unsigned i = batch * 1024 + j, tex = i % 32, x = j % 64, y = (j / 64) + (batch / 2 % 2) * 16;
                S.units[0][0] = &textures[tex];
                S.raster.scissor[0] = int32_t(x);
                S.raster.scissor[1] = int32_t(y);
                // Switch pipelines and descriptors while retaining exact draw order.
                S.raster.blend = ((j / 16) & 1) != 0;
                if (churn) {
                    const float tint[4] = {j & 1 ? .5f : 1, 1, 1, 1};
                    std::memcpy(S.uniform_words[1].data(), tint, 16);
                }
                require(issue_draw_program(&program, nullptr, j & 32 ? 6 : 4, 3, 0, indices, false, nullptr),
                        "authored stress draw failed");
                auto& e = expected[batch & 1][y * 64 + x];
                e = {uint8_t((tex & 1) ? (churn && (j & 1) ? 128 : 255) : 0), uint8_t(tex & 2 ? 255 : 0),
                     uint8_t(tex & 4 ? 255 : 0), 255};
            }
            // Asynchronous submissions exercise several live pool generations.
            gx2vk::submit(false);
            samples.push_back(ms(start));
            if (batch == 0) first_resets = counts.resets;
        }
        if (lifetime) {
            require(gate_submitted && counts.resets == first_resets && counts.pools >= 15,
                    "descriptor pool reset before in-flight work completed");
            VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
            signal.semaphore = gate;
            signal.value = 1;
            require(vkSignalSemaphore(gx2vk::device(), &signal) == VK_SUCCESS, "lifetime gate release failed");
        }
        require(gx2vk::finish(), "stress completion failed");
        if (lifetime) {
            require(gx2vk::span_alive(indices.span), "index lifetime lost after wait");
            require(issue_draw_program(&program, nullptr, 4, 3, 0, indices, false, nullptr),
                    "post-completion reuse draw failed");
            gx2vk::submit(true);
            require(counts.resets > first_resets, "completed descriptor pools were not reused");
        }
        // No pending work: this must not make repeated driver queries.
        for (unsigned i = 0; i < 1000; ++i) gx2vk::completed_serial();
        const auto total = ms(begin);
        std::sort(samples.begin(), samples.end());
        const unsigned draws = lifetime ? 16385 : 16384;
        std::printf("{\"metric\":\"draws\",\"operations\":%u,\"wall_ms\":%.6f,\"record_batch_median_ms\":%.6f,\"record_"
                    "batch_p95_ms\":%.6f,\"ops_per_sec\":%.3f,\"sets\":%llu,\"updates\":%llu,\"pools\":%llu,\"pool_"
                    "resets\":%llu,\"descriptor_binds\":%llu,\"index_binds\":%llu,\"completion_queries\":%llu,"
                    "\"pipeline_binds\":%llu,\"submissions\":%llu,\"barriers\":%llu}\n",
                    draws, total, samples[8], samples[15], draws * 1000. / total, (unsigned long long)counts.sets,
                    (unsigned long long)counts.updates, (unsigned long long)counts.pools,
                    (unsigned long long)counts.resets, (unsigned long long)counts.binds,
                    (unsigned long long)counts.indices, (unsigned long long)counts.polls,
                    (unsigned long long)counts.pipelines, (unsigned long long)(gx2vk::submission_count() - submissions),
                    (unsigned long long)(gx2vk::barrier_count() - barriers));
    }
    S.framebuffer = {};
    S.units[0][0] = nullptr;
    end_rendering();
    for (unsigned i = 0; i < targets.size(); ++i) {
        std::vector<uint8_t> bytes;
        require(gx2vk::read_image(targets[i].image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 64, 32, bytes),
                "stress readback failed");
        for (unsigned p = 0; p < 2048; ++p)
            for (unsigned c = 0; c < 4; ++c)
                require(std::abs(int(bytes[p * 4 + c]) - int(expected[i][p][c])) <= 1,
                        "draw order, uniform, texture or render-target output mismatch");
        gx2vk::destroy_image(targets[i].image);
    }
    for (auto& image : images) gx2vk::destroy_image(image);
    require(gx2vk::finish(), "cleanup wait failed");
    s_last_pipeline_lookup.reset();
    for (auto it = s_pipelines.begin(); it != s_pipelines.end();) {
        uint32_t id = 0;
        std::memcpy(&id, it->first.data(), 4);
        if (id != program.id) {
            ++it;
            continue;
        }
        if (auto p = await_compile(it->second)) vkDestroyPipeline(gx2vk::device(), p, nullptr);
        it = s_pipelines.erase(it);
    }
    release_program_layouts(program);
    reset_command_tracking();
    std::printf("{\"correct_pixels\":4096,\"passed\":true}\n");
}

void gx2_render_vk_test_lifetime() {
    using namespace render_stress;
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    info.pNext = &type;
    require(vkCreateSemaphore(gx2vk::device(), &info, nullptr, &gate) == VK_SUCCESS, "lifetime gate creation failed");
    lifetime = true;
    gate_submitted = false;
    try {
        gx2_render_vk_test_stress(false, true);
    } catch (...) {
        VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        signal.semaphore = gate;
        signal.value = 1;
        vkSignalSemaphore(gx2vk::device(), &signal);
        gx2vk::finish();
        vkDestroySemaphore(gx2vk::device(), gate, nullptr);
        gate = VK_NULL_HANDLE;
        lifetime = false;
        throw;
    }
    vkDestroySemaphore(gx2vk::device(), gate, nullptr);
    gate = VK_NULL_HANDLE;
    lifetime = false;
}

#include "render_discard_state.inl"

void gx2_render_vk_test_binding_contracts() {
    using namespace render_stress;
    WarmPipelineRecord warm{};
    warm.colors[0] = VK_FORMAT_R8G8B8A8_UNORM;
    warm.depth = VK_FORMAT_D32_SFLOAT;
    require(warm_formats_supported(warm), "portable warm-up formats rejected");
    if (!gx2vk::format_supports(VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        warm.depth = VK_FORMAT_D24_UNORM_S8_UINT;
        require(!warm_formats_supported(warm), "foreign GPU's unsupported depth format accepted");
    }
    Program program;
    for (auto& i : program.info) {
        i.attribute_locations.fill(-1);
        i.vk.uniform_buffer_bindings.fill(-1);
        i.vk.texture_bindings.fill(-1);
    }
    program.info[1].vk.uniform_vars_binding = 7;
    program.info[1].vk.uniform_buffer_bindings[0] = 3;
    require(create_program_layouts(program, false), "contract layout failed");
    {
        Program equivalent;
        equivalent.info = program.info;
        require(create_program_layouts(equivalent, false), "shared contract layout failed");
        require(equivalent.layout == program.layout && equivalent.set_layout == program.set_layout,
                "identical shader interfaces did not share layouts");
        Program stage;
        stage.info = program.info;
        std::swap(stage.info[0].vk, stage.info[1].vk);
        require(create_program_layouts(stage, false), "stage-visibility layout failed");
        require(stage.layout != program.layout, "layout key omitted shader-stage visibility");
        Program pushed;
        pushed.info = program.info;
        require(create_program_layouts(pushed, true), "push contract layout failed");
        if (pushed.push)
            require(pushed.layout != program.layout, "push and ordinary descriptor layouts were conflated");
        std::array<Program, 8> concurrent;
        std::vector<std::future<bool>> results;
        for (auto& p : concurrent) {
            p.info = program.info;
            p.info[1].vk.uniform_vars_binding = 11;
            auto* target = &p;
            results.push_back(
                std::async(std::launch::async, [target] { return create_program_layouts(*target, false); }));
        }
        for (auto& result : results) require(result.get(), "concurrent layout creation failed");
        const auto shared = concurrent[0].layout;
        for (auto& p : concurrent) {
            require(p.layout == shared, "compiler workers created duplicate layouts");
            release_program_layouts(p);
        }
        release_program_layouts(equivalent);
        release_program_layouts(stage);
        release_program_layouts(pushed);
    }
    require(program.dynamic_uniform_bindings.size() == 2, "device dynamic UBO limit not respected");
    auto buffer = gx2vk::create_buffer(8192, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                       gx2vk::MemoryUse::Upload);
    require(buffer.buffer, "contract buffer failed");
    VkDescriptorBufferInfo buffers[2] = {{buffer.buffer, 512, 64}, {buffer.buffer, 256, 64}};
    VkWriteDescriptorSet writes[2]{};
    for (unsigned i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[i].descriptorCount = 1;
        writes[i].dstBinding = i ? 3 : 7;
        writes[i].pBufferInfo = &buffers[i];
    }
    {
        Hooks hooks;
        vkAllocateDescriptorSets = inspect_allocate;
        vkCmdBindDescriptorSets = inspect_bind;
        auto cb = gx2vk::cmd();
        reset_command_tracking();
        require(bind_descriptors(cb, program, writes, 2), "first contract bind failed");
        require(last_dynamic_offsets == std::vector<uint32_t>{256, 512}, "dynamic offsets are not in binding order");
        buffers[0].offset = 768;
        buffers[1].offset = 1024;
        require(bind_descriptors(cb, program, writes, 2) && counts.sets == 1,
                "changing uniform offsets allocated duplicate sets");
        require(last_dynamic_offsets == std::vector<uint32_t>{1024, 768}, "updated dynamic offsets were lost");
        require(bind_descriptors(cb, program, writes, 2) && counts.binds == 2, "unchanged descriptors rebound");
        buffers[0].range = 32;
        require(bind_descriptors(cb, program, writes, 2) && counts.sets == 2, "descriptor range missing from key");
        fail_allocate = VK_ERROR_OUT_OF_POOL_MEMORY;
        buffers[0].range = 48;
        const auto before = counts.sets;
        require(bind_descriptors(cb, program, writes, 2) && counts.sets == before + 2,
                "pool exhaustion was not recovered once");
        fail_allocate = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        buffers[0].range = 16;
        const auto failed = counts.sets;
        require(!bind_descriptors(cb, program, writes, 2) && counts.sets == failed + 1,
                "device OOM retried or recursively allocated");
        require(bind_descriptors(cb, program, writes, 2), "binding did not recover after injected failure");
        bind_indices(cb, buffer.buffer, 0, VK_INDEX_TYPE_UINT16);
        bind_indices(cb, buffer.buffer, 0, VK_INDEX_TYPE_UINT16);
        require(counts.indices == 1, "identical indices rebound");
        bind_indices(cb, buffer.buffer, 4, VK_INDEX_TYPE_UINT16);
        bind_indices(cb, buffer.buffer, 4, VK_INDEX_TYPE_UINT32);
        reset_command_tracking();
        bind_indices(cb, buffer.buffer, 4, VK_INDEX_TYPE_UINT32);
        require(counts.indices == 4, "index offset/type or invalidation ignored");
        gx2vk::submit(true);
        const auto polls = counts.polls;
        for (unsigned i = 0; i < 1000; ++i) gx2vk::completed_serial();
        require(counts.polls == polls, "idle completion queries reached driver");
    }
    // Cached sampler results must match full normalization for every field.
    for (unsigned i = 0; i < 24; ++i) {
        TexParams p;
        p.min_filter = i & 1 ? wire::NEAREST : wire::LINEAR;
        p.wrap_s = i & 2 ? wire::CLAMP_TO_BORDER : wire::REPEAT;
        p.min_lod = float(i) / 8;
        p.max_lod = 8;
        p.lod_bias = float(i) / 16;
        p.border = {float(i & 1), 0, 0, 1};
        const bool shadow = i & 4, integer = i & 8;
        VkSamplerCreateInfo info;
        gx2r_sampler_info(p, shadow, integer, info);
        VkClearColorValue border{};
        std::memcpy(border.float32, p.border.data(), 16);
        const auto expected = gx2vk::sampler(info, info.borderColor == VK_BORDER_COLOR_INT_CUSTOM_EXT ||
                                                           info.borderColor == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT
                                                       ? &border
                                                       : nullptr);
        require(tex_object_sampler(p, shadow, integer) == expected &&
                    tex_object_sampler(p, shadow, integer) == expected,
                "sampler cache changed parameters");
    }
    gx2vk::destroy_buffer(buffer);
    require(gx2vk::finish(), "contract cleanup failed");
    release_program_layouts(program);
    reset_command_tracking();
    verify_discard_state_transitions();
    std::printf("{\"binding_contracts\":true,\"passed\":true}\n");
}

#include "heavy_render_stress.inl"

// Authored populated-scene proxy, with no game code, assets or scene claims.
// Kept beside production draw helpers to exercise vertex translation and residency.
#include "../port/runtime/rebrewu_memory_observer.h"
#include "rebrewu/logger.hpp"
void gx2_render_vk_test_heavy(bool overdraw, bool push) {
    using namespace render_stress;
    using Clock = std::chrono::steady_clock;
    auto require = [](bool ok, const char* text) {
        if (!ok) throw std::runtime_error(text);
    };
    auto elapsed = [](Clock::time_point t) {
        return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    };
    constexpr unsigned objects = 1024, meshes = 16, streams = 4, stream_bytes = 65536, extent = 512;
    // RBREW_BENCH_FRAMES lengthens profiling runs; comparisons use 12.
    const unsigned frames = [] {
        const char* v = std::getenv("RBREW_BENCH_FRAMES");
        const int n = v ? std::atoi(v) : 0;
        return n > 0 ? unsigned(n) : 12u;
    }();
    constexpr uint32_t base = 65536, fetch = 0x123400;
    const size_t arena_bytes = base + meshes * streams * stream_bytes;
#ifdef _WIN32
    auto* mem = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, arena_bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE));
    require(mem != nullptr, "authored write-watch arena failed");
#else
    std::vector<uint8_t> arena(arena_bytes);
    auto* mem = arena.data();
#endif
    std::vector<R700FetchAttribute> attributes;
    // Native fetch decoder index 19 imports four 32-bit words.
    for (unsigned s = 0; s < streams; ++s) attributes.push_back({s, s, 0, 0x813, 0, GX2_ENDIAN_SWAP_NONE, 0, false});
    gx2_render_store_fetch_attributes(fetch, attributes);
    gx2_render_set_active_fetch_shader(fetch);
    for (unsigned s = 0; s < 32; ++s) S.regs[0xA0E0 + s] = s < streams ? s : 255;
    for (unsigned m = 0; m < meshes; ++m)
        for (unsigned s = 0; s < streams; ++s)
            for (unsigned v = 0; v < stream_bytes / 16; ++v) {
                uint32_t data[4] = {v & 3, m, s, 1};
                if (s == 0) {
                    const float p[4] = {v % 3 == 1 ? 3.f : -1.f, v % 3 == 2 ? 3.f : -1.f, 0.5f, 1.f};
                    std::memcpy(data, p, 16);
                }
                std::memcpy(mem + base + (m * streams + s) * stream_bytes + v * 16, data, 16);
            }
    std::array<Program, 9> programs;
    auto vertex = gx2vk::shader_module(
        VK_SHADER_STAGE_VERTEX_BIT,
        "#version 450\nlayout(location=0)in uvec4 a;layout(location=1)in uvec4 b;layout(location=2)in uvec4 "
        "c;layout(location=3)in uvec4 d;"
        "layout(set=0,binding=0)uniform Pose{vec4 bones[16];};void main(){vec4 p=uintBitsToFloat(a);"
        "vec4 "
        "skin=bones[b.x&15u]+bones[c.y&15u]+bones[d.z&15u];p.xy+=skin.xy+vec2(float((b.y^c.z^d.w)&1u)*0.001);gl_"
        "Position=p;}\n");
    require(vertex != VK_NULL_HANDLE, "heavy vertex shader failed");
    for (unsigned i = 0; i < programs.size(); ++i) {
        auto& p = programs[i];
        p.id = UINT32_MAX - 32 - i;
        p.vs = vertex;
        for (auto& info : p.info) {
            info.attribute_locations.fill(-1);
            info.buffer_bindings.fill(-1);
            info.vk.uniform_buffer_bindings.fill(-1);
            info.vk.texture_bindings.fill(-1);
            info.vk.offset_tex_scale.fill(-1);
        }
        auto& vs = p.info[0];
        for (unsigned s = 0; s < streams; ++s) vs.attribute_locations[s] = s;
        vs.uniform_mode = 2;
        vs.vk.uniform_vars_binding = 0;
        vs.vk.offset_registers = 0;
        vs.vk.register_vec4s = 16;
        vs.vk.uniform_block_bytes = 256;
        auto& ps = p.info[1];
        ps.vk.pixel_outputs = i == 8 ? 0 : 1;
        if (i == 8)
            p.ps_source = "#version 450\nvoid main(){}\n";
        else {
            ps.uniform_mode = 2;
            ps.vk.uniform_vars_binding = 1;
            ps.vk.offset_registers = 0;
            ps.vk.register_vec4s = 1;
            ps.vk.uniform_block_bytes = 16;
            for (unsigned t = 0; t < 4; ++t) {
                ps.vk.texture_bindings[t] = int8_t(2 + t);
                ps.vk.texture_dim[t] = 2;
            }
            p.ps_source =
                "#version 450\nlayout(set=0,binding=1)uniform U{vec4 tint;};layout(set=0,binding=2)uniform sampler2D "
                "t0;layout(set=0,binding=3)uniform sampler2D t1;layout(set=0,binding=4)uniform sampler2D "
                "t2;layout(set=0,binding=5)uniform sampler2D shadowMap;layout(location=0)out vec4 color;void main(){"
                "uint "
                "h=uint(round(texelFetch(t0,ivec2(0),0).r*255.))*3u+uint(round(texelFetch(t1,ivec2(0),0).g*255.))*5u+"
                "uint(round(texelFetch(t2,ivec2(0),0).b*255.))*7u+" +
                std::to_string(i) + "u;for(uint j=0u;j<" + std::to_string(overdraw ? 48 : 8) +
                "u;++j){h=(h*1664525u+1013904223u)^(h>>13u);}"
                "vec3 rgb=vec3(h&255u,(h>>8u)&255u,(h>>16u)&255u)/255.;float "
                "lit=texelFetch(shadowMap,ivec2(gl_FragCoord.xy),0).r<0.75?1.:0.;color=vec4(rgb*lit*tint.rgb,tint.a);}"
                "\n";
        }
        require(create_program_layouts(p, push), "heavy program layout failed");
    }
    std::array<ColorTarget, 2> targets;
    DepthTarget shadow;
    for (auto& t : targets) {
        t.width = t.height = extent;
        gx2vk::ImageDesc d;
        d.width = d.height = extent;
        d.usage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        require(gx2vk::create_image(t.image, d), "heavy target allocation failed");
        VkClearColorValue zero{};
        clear_image_color(t.image, zero);
    }
    {
        gx2vk::ImageDesc d;
        d.width = d.height = extent;
        d.format = VK_FORMAT_D32_SFLOAT;
        d.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        require(gx2vk::create_image(shadow.image, d), "heavy shadow allocation failed");
        shadow.width = shadow.height = extent;
        shadow.tex.image = &shadow.image;
        shadow.tex.depth = true;
        shadow.tex.params.min_filter = shadow.tex.params.mag_filter = wire::NEAREST;
    }
    std::array<gx2vk::Image, 64> images;
    std::array<TexObject, 64> textures;
    for (unsigned i = 0; i < images.size(); ++i) {
        gx2vk::ImageDesc d;
        d.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        require(gx2vk::create_image(images[i], d), "heavy texture allocation failed");
        VkClearColorValue v{};
        v.float32[0] = float(i * 3) / 255;
        v.float32[1] = float(i * 2) / 255;
        v.float32[2] = float(i) / 255;
        v.float32[3] = 1;
        clear_image_color(images[i], v);
        textures[i].image = &images[i];
        textures[i].params.min_filter = textures[i].params.mag_filter = wire::NEAREST;
    }
    IndexStream indices;
    indices.indexed = true;
    indices.type = VK_INDEX_TYPE_UINT16;
    indices.count = overdraw ? 768 : 384;
    indices.span = gx2vk::upload(indices.count * 2, 2);
    require(indices.span.data != nullptr, "heavy indices failed");
    for (unsigned i = 0; i < indices.count; ++i) {
        const uint16_t v = uint16_t(i);
        std::memcpy(indices.span.data + i * 2, &v, 2);
    }
    VkQueryPool timestamps = VK_NULL_HANDLE;
    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gx2vk::physical_device(), &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(gx2vk::physical_device(), &queue_count, queues.data());
    const unsigned valid_bits = queues[gx2vk::queue_family()].timestampValidBits;
    if (valid_bits) {
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = frames * 2;
        require(vkCreateQueryPool(gx2vk::device(), &info, nullptr, &timestamps) == VK_SUCCESS,
                "heavy timestamp pool failed");
        vkCmdResetQueryPool(gx2vk::cmd(), timestamps, 0, frames * 2);
    }
    auto bind_geometry = [&](unsigned object) {
        const unsigned m = object % meshes;
        for (unsigned s = 0; s < streams; ++s)
            gx2_render_set_attrib_buffer(s, base + (m * streams + s) * stream_bytes, stream_bytes, 16);
    };
    S.raster = RasterState{};
    S.raster.viewport[2] = S.raster.viewport[3] = 16;
    S.draw_instances = 1;
    S.draw_first_instance = 0;
    S.single_instance_fetch = false;
    S.conditional_draw = true;
    S.regs[0xA204] = 1u << 19; // authored shaders emit Vulkan's [0,1] depth range
    S.framebuffer = {};
    S.framebuffer.valid = true;
    S.framebuffer.depth = &shadow;
    S.target_masks = 0;
    S.raster.depth_test = true;
    std::memset(S.uniform_words[0].data(), 0, 256);
    bind_geometry(0);
    {
        VertexBindings probe;
        setup_vertex_attribs(mem, programs[0], probe);
        require(probe.input.count == streams && probe.buffers[0] && probe.input.attrs[0].stride == 16 &&
                    probe.input.attrs[0].format == VK_FORMAT_R32G32B32A32_UINT,
                "heavy vertex bindings missing");
    }
    require(issue_draw_program(&programs[8], mem, 4, indices.count, 0, indices, false, nullptr),
            "heavy shadow warm-up failed");
    S.framebuffer.depth = nullptr;
    S.framebuffer.colors[0] = &targets[0];
    S.raster.depth_test = false;
    S.target_masks = 1;
    for (unsigned t = 0; t < 3; ++t) S.units[t][0] = &textures[0];
    S.units[3][0] = &shadow.tex;
    for (unsigned i = 0; i < 8; ++i)
        for (bool blend : {false, true}) {
            S.raster.blend = blend;
            require(issue_draw_program(&programs[i], mem, 4, indices.count, 0, indices, false, nullptr),
                    "heavy pipeline warm-up failed");
        }
    end_rendering();
    require(gx2vk::finish(), "heavy warm-up completion failed");
    auto history = gx2vk::create_buffer(frames * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gx2vk::MemoryUse::Readback);
    require(history.memory.mapped != nullptr, "heavy history allocation failed");
    counts = {};
    Hooks hooks;
    const auto uploads = C.vertex_uploads, submissions = gx2vk::submission_count(), barriers = gx2vk::barrier_count();
    const auto begin = Clock::now();
    std::vector<double> cpu_samples;
    for (unsigned f = 0; f < frames; ++f) {
        const auto start = Clock::now();
        gx2_write_watch_begin_batch();
        // Native shared-writer stores exercise geometry invalidation while earlier frames are in flight.
        for (unsigned m = 0; m < meshes; ++m) rbrew_write32(mem, base + (m * streams + 2) * stream_bytes, f);
        require(gx2vk::span_alive(indices.span), "heavy index span expired");
        if (timestamps) {
            if (f & 1)
                vkCmdWriteTimestamp2(gx2vk::cmd(), VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestamps, f * 2);
            else
                vkCmdWriteTimestamp(gx2vk::cmd(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, f * 2);
        }
        clear_image_depth(shadow.image, 1, 0);
        for (auto& t : targets) {
            VkClearColorValue zero{};
            clear_image_color(t.image, zero);
        }
        for (unsigned pass = 0; pass < 3; ++pass) {
            const unsigned n = pass == 2 ? 256 : objects;
            S.raster.blend = pass == 2;
            S.raster.blend_src_rgb = wire::SRC_ALPHA;
            S.raster.blend_dst_rgb = wire::ONE_MINUS_SRC_ALPHA;
            S.raster.depth_test = pass == 0;
            S.target_masks = pass == 0 ? 0 : 1;
            S.framebuffer.depth = pass == 0 ? &shadow : nullptr;
            S.framebuffer.colors[0] = pass == 0 ? nullptr : &targets[0];
            for (unsigned o = 0; o < n; ++o) {
                bind_geometry(o);
                const unsigned x = o % 32, y = o / 32;
                S.raster.viewport[0] = int32_t(x * 16);
                S.raster.viewport[1] = int32_t(y * 16);
                if (pass) S.framebuffer.colors[0] = &targets[(o / 128) & 1];
                const float pose[4] = {float((o + f) % 4) * .0001f, 0, 0, 0};
                std::memcpy(S.uniform_words[0].data(), pose, 16);
                const float tint[4] = {1, 1, 1, pass == 2 ? .5f : 1.f};
                std::memcpy(S.uniform_words[1].data(), tint, 16);
                for (unsigned t = 0; t < 3; ++t) S.units[t][0] = &textures[(o + t * 7 + f) % 64];
                auto& p = programs[pass == 0 ? 8 : (o + f + pass) % 8];
                require(issue_draw_program(&p, mem, 4, indices.count, 0, indices, false, nullptr),
                        "heavy measured draw failed");
            }
        }
        end_rendering();
        if (timestamps) {
            if (f & 1)
                vkCmdWriteTimestamp2(gx2vk::cmd(), VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestamps, f * 2 + 1);
            else
                vkCmdWriteTimestamp(gx2vk::cmd(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, f * 2 + 1);
        }
        gx2vk::barrier_if_dirty();
        VkBufferImageCopy snapshot{};
        snapshot.bufferOffset = f * 4;
        snapshot.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        snapshot.imageOffset = {8, 8, 0};
        snapshot.imageExtent = {1, 1, 1};
        vkCmdCopyImageToBuffer(gx2vk::cmd(), targets[0].image.image, VK_IMAGE_LAYOUT_GENERAL, history.buffer, 1,
                               &snapshot);
        gx2vk::note_gpu_write();
        gx2vk::submit(false);
        cpu_samples.push_back(elapsed(start));
    }
    {
        VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = history.buffer;
        b.size = VK_WHOLE_SIZE;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.bufferMemoryBarrierCount = 1;
        d.pBufferMemoryBarriers = &b;
        vkCmdPipelineBarrier2(gx2vk::cmd(), &d);
    }
    require(gx2vk::finish(), "heavy final GPU completion failed");
    const double wall = elapsed(begin);
    std::vector<double> gpu_samples;
    if (timestamps) {
        std::vector<uint64_t> ticks(frames * 2);
        const auto result = vkGetQueryPoolResults(gx2vk::device(), timestamps, 0, frames * 2, ticks.size() * 8,
                                                  ticks.data(), 8, VK_QUERY_RESULT_64_BIT);
        if (result != VK_SUCCESS)
            RBREW_LOG_WARN("render", "timestamp result=%d valid_bits=%u submissions=%llu", int(result), valid_bits,
                           (unsigned long long)(gx2vk::submission_count() - submissions));
        require(result == VK_SUCCESS, "heavy timestamps unavailable");
        const uint64_t mask = valid_bits == 64 ? ~0ull : (1ull << valid_bits) - 1;
        for (unsigned f = 0; f < frames; ++f)
            gpu_samples.push_back(double((ticks[f * 2 + 1] - ticks[f * 2]) & mask) *
                                  gx2vk::caps().limits.timestampPeriod / 1e6);
        vkDestroyQueryPool(gx2vk::device(), timestamps, nullptr);
    }
    auto percentile = [](std::vector<double> values, double p) {
        if (values.empty()) return -1.;
        std::sort(values.begin(), values.end());
        return values[std::min(values.size() - 1, size_t(std::ceil(p * values.size()) - 1))];
    };
    unsigned checked = 0;
    auto rgb = [&](unsigned o, unsigned pass, unsigned f) {
        uint32_t h = ((o + f) % 64) * 9 + ((o + 7 + f) % 64) * 10 + ((o + 14 + f) % 64) * 7 + (o + f + pass) % 8;
        for (unsigned j = 0; j < (overdraw ? 48u : 8u); ++j) h = (h * 1664525u + 1013904223u) ^ (h >> 13);
        return std::array<unsigned, 3>{h & 255u, (h >> 8) & 255u, (h >> 16) & 255u};
    };
    for (unsigned target = 0; target < targets.size(); ++target) {
        std::vector<uint8_t> pixels;
        require(gx2vk::read_image(targets[target].image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, extent, extent, pixels),
                "heavy output readback failed");
        for (unsigned o = 0; o < objects; ++o)
            if (((o / 128) & 1) == target) {
                auto expected = rgb(o, o < 256 ? 2 : 1, frames - 1);
                const size_t p = ((o / 32 * 16 + 8) * extent + o % 32 * 16 + 8) * 4;
                for (unsigned c = 0; c < 3; ++c) {
                    if (std::abs(int(pixels[p + c]) - int(expected[c])) > 2)
                        RBREW_LOG_WARN("render", "pixel target=%u object=%u component=%u actual=%u expected=%u", target,
                                       o, c, pixels[p + c], expected[c]);
                    require(std::abs(int(pixels[p + c]) - int(expected[c])) <= 2,
                            "heavy texture/shadow/blend/order output mismatch");
                }
                ++checked;
            }
    }
    std::vector<uint8_t> depth;
    require(gx2vk::read_image(shadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, extent, extent, depth),
            "heavy shadow readback failed");
    for (unsigned o = 0; o < objects; ++o) {
        float z;
        std::memcpy(&z, depth.data() + ((o / 32 * 16 + 8) * extent + o % 32 * 16 + 8) * 4, 4);
        require(std::abs(z - .5f) < 1e-6, "heavy shadow depth mismatch");
        ++checked;
    }
    require(gx2vk::invalidate_mapped(history.memory), "heavy history invalidation failed");
    for (unsigned f = 0; f < frames; ++f) {
        const auto expected = rgb(0, 2, f);
        for (unsigned c = 0; c < 3; ++c)
            require(std::abs(int(static_cast<uint8_t*>(history.memory.mapped)[f * 4 + c]) - int(expected[c])) <= 2,
                    "in-flight heavy frame lost original resource contents");
        ++checked;
    }
    std::printf("{\"metric\":\"heavy_frames\",\"frames\":%u,\"objects\":%u,\"draws_per_frame\":2304,\"indices_per_"
                "draw\":%u,\"wall_ms\":%.6f,\"cpu_median_ms\":%.6f,\"cpu_p95_ms\":%.6f,\"cpu_p99_ms\":%.6f,\"gpu_"
                "median_ms\":%.6f,\"gpu_p95_ms\":%.6f,\"gpu_p99_ms\":%.6f,\"draws_per_sec\":%.3f,\"vertex_uploads\":%"
                "llu,\"submissions\":%llu,\"barriers\":%llu,\"checked_pixels\":%u,\"passed\":true}\n",
                frames, objects, indices.count, wall, percentile(cpu_samples, .5), percentile(cpu_samples, .95),
                percentile(cpu_samples, .99), percentile(gpu_samples, .5), percentile(gpu_samples, .95),
                percentile(gpu_samples, .99), frames * 2304 * 1000. / wall,
                (unsigned long long)(C.vertex_uploads - uploads),
                (unsigned long long)(gx2vk::submission_count() - submissions),
                (unsigned long long)(gx2vk::barrier_count() - barriers), checked);
    std::printf("{\"metric\":\"heavy_bindings\",\"pipeline_binds\":%llu,\"descriptor_binds\":%llu,\"descriptor_"
                "updates\":%llu,\"descriptor_sets\":%llu}\n",
                (unsigned long long)counts.pipelines, (unsigned long long)counts.binds,
                (unsigned long long)counts.updates, (unsigned long long)counts.sets);
    S.framebuffer = {};
    S.units = {};
    for (auto& a : S.attrib) a = {};
    end_rendering();
    for (auto& t : targets) gx2vk::destroy_image(t.image);
    gx2vk::destroy_image(shadow.image);
    for (auto& i : images) gx2vk::destroy_image(i);
    gx2vk::destroy_buffer(history);
    require(gx2vk::finish(), "heavy cleanup wait failed");
    s_last_pipeline_lookup.reset();
    for (auto it = s_pipelines.begin(); it != s_pipelines.end();) {
        uint32_t id = 0;
        std::memcpy(&id, it->first.data(), 4);
        bool own = false;
        for (auto& p : programs) own |= id == p.id;
        if (!own) {
            ++it;
            continue;
        }
        if (auto p = await_compile(it->second)) vkDestroyPipeline(gx2vk::device(), p, nullptr);
        it = s_pipelines.erase(it);
    }
    for (auto& p : programs) release_program_layouts(p);
    reset_command_tracking();
#ifdef _WIN32
    rbrew_set_write_observer({});
    VirtualFree(mem, 0, MEM_RELEASE);
#endif
}

void gx2_render_vk_test_watch_ranges() {
#ifdef _WIN32
    auto require = [](bool ok, const char* text) {
        if (!ok) throw std::runtime_error(text);
    };
    constexpr uint32_t bytes = 4u << 20, base = 65536, range = 1u << 20;
    auto* mem =
        static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE));
    require(mem != nullptr, "watch test allocation failed");
    std::memset(mem, 1, bytes);
    gx2_write_watch_begin_batch();
    const auto first = gx2_write_generation(mem, base, range);
    require(first != kGx2WriteUntracked, "authored arena is not tracked");
    for (unsigned n = 0; n < 256; ++n)
        require(gx2_write_generation(mem, base, range) == first, "unchanged range generation changed");
    rbrew_write32(mem, base + range - 4, 0x12345678);
    const auto changed = gx2_write_generation(mem, base, range);
    require(changed > first, "observed partial-page write was missed");
    Gx2WriteRange checks[4] = {
        {base, range}, {base + 4096, range}, {base + range - 4096, 8192}, {base + 2 * range, range}};
    gx2_write_generations(mem, checks, 4);
    require(checks[0].generation >= changed && checks[1].generation >= changed && checks[2].generation >= changed,
            "overlapping batched range lost write");
    // A producer's ordinary store remains dirty until the next batch.
    mem[base + 8192] ^= 1;
    gx2_write_watch_begin_batch();
    require(gx2_write_generation(mem, base, range) > changed, "new-batch OS write was missed");
    std::thread other([&] {
        mem[base + 16384] ^= 1;
        gx2_write_generation(mem, base, range);
    });
    other.join();
    const auto outside = gx2_write_generation(mem, base, range);
    require(outside > changed, "non-batch caller lost the write history");
    gx2_write_watch_begin_batch();
    require(gx2_write_generation(mem, base, range) == outside, "new batch changed collected history");
    const auto begin = std::chrono::steady_clock::now();
    uint64_t sum = 0;
    for (unsigned n = 0; n < 10000; ++n) {
        gx2_write_generations(mem, checks, 4);
        for (const auto& check : checks) sum += check.generation;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    std::printf(
        "{\"metric\":\"watch_ranges\",\"range_queries\":40000,\"wall_ms\":%.6f,\"checksum\":%llu,\"passed\":true}\n",
        ms, (unsigned long long)sum);
    rbrew_set_write_observer({});
    VirtualFree(mem, 0, MEM_RELEASE);
#endif
}

void gx2_render_vk_test_vertex_plans() {
    auto require = [](bool ok, const char* text) {
        if (!ok) throw std::runtime_error(text);
    };
#ifdef _WIN32
    auto* mem = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 1u << 20, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE));
    require(mem != nullptr, "geometry arena allocation failed");
#else
    std::vector<uint8_t> arena(1u << 20, 0);
    auto* mem = arena.data();
#endif
    gx2_write_watch_begin_batch();
    Program p;
    p.info[0].attribute_locations.fill(-1);
    p.info[0].attribute_locations[5] = 0;
    p.info[0].attribute_locations[6] = 1;
    p.attribute_semantics = {5, 6};
    for (unsigned i = 0; i < 32; ++i) S.regs[0xA0E0 + i] = 255;
    S.regs[0xA0E0] = 5;
    S.regs[0xA0E1] = 6;
    std::vector<R700FetchAttribute> attrs = {{5, 0, 16, 0x813, 3, 3, 0, true}, {6, 1, 8, 4, 0, 3, 0, false}};
    constexpr uint32_t fetch = 0x7770;
    gx2_render_store_fetch_attributes(fetch, attrs);
    gx2_render_set_active_fetch_shader(fetch);
    gx2_render_set_attrib_buffer(0, 65536, 256, 32);
    gx2_render_set_attrib_buffer(1, 81920, 256, 16);
    S.single_instance_fetch = false;
    auto get = [&] {
        VertexBindings v;
        setup_vertex_attribs(mem, p, v);
        require(v.input.count == 2, "vertex plan omitted a live location");
        return v;
    };
    auto v = get();
    require(v.input.attrs[0].format == VK_FORMAT_R32G32B32A32_UINT && v.input.attrs[1].format == VK_FORMAT_R8G8_UINT,
            "vertex plan changed native formats");
    require(v.input.attrs[0].stride == 32 && v.input.attrs[0].divisor == 3, "vertex plan changed stride/divisor");
    gx2_render_set_attrib_buffer(0, 65536, 256, 24);
    S.single_instance_fetch = true;
    S.instance_uniform = 8;
    v = get();
    require(v.input.attrs[0].stride == 24 && v.input.attrs[0].divisor == 1 && v.offsets[0] % 16 == 0,
            "vertex plan cached instance or stride state");
    // Untracked test memory creates a fresh ring span each time; compare the
    // relative offset against the base span recorded by this draw's slot.
    require(v.offsets[0] >= 64, "instance divisor offset was lost");
    gx2_render_set_attrib_buffer(1, 0, 0, 16);
    v = get();
    require(v.input.attrs[1].stride == 0, "unbound stream was not generic");
    gx2_render_set_attrib_buffer(1, 81920, 256, 16);
    S.regs[0xA0E1] = 255;
    v = get();
    require(v.input.attrs[1].stride == 0, "semantic-import change was ignored");
    S.regs[0xA0E1] = 6;
    attrs[0].per_instance = false;
    attrs[0].format = 4;
    gx2_render_store_fetch_attributes(fetch, attrs);
    v = get();
    require(v.input.attrs[0].format == VK_FORMAT_R8G8_UINT, "same-address fetch replacement was ignored");
    attrs[0].format = 0x3f;
    gx2_render_store_fetch_attributes(fetch, attrs);
    v = get();
    require(v.input.attrs[0].stride == 0 && v.input.attrs[1].stride == 0,
            "invalid format did not abort remaining live attributes");
    gx2_render_set_attrib_buffer(0, 0, 0, 24);
    v = get();
    require(v.input.attrs[1].stride == 16, "invalid format on an absent stream aborted a later stream");
    S.single_instance_fetch = false;
    S.instance_uniform = 0;
    attrs[0].format = 0x813;
    gx2_render_store_fetch_attributes(fetch, attrs);
    gx2_render_set_attrib_buffer(0, 65536, 256, 24);
    const auto start = std::chrono::steady_clock::now();
    uint64_t sum = 0;
    for (unsigned n = 0; n < 20000; ++n) {
        auto value = get();
        sum += value.input.attrs[0].stride + value.input.attrs[1].format + value.offsets[0];
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    for (auto& a : S.attrib) a = {};
    require(gx2vk::finish(), "vertex plan cleanup failed");
    std::printf(
        "{\"metric\":\"vertex_plans\",\"operations\":20000,\"wall_ms\":%.6f,\"checksum\":%llu,\"passed\":true}\n", ms,
        (unsigned long long)sum);
#ifdef _WIN32
    rbrew_set_write_observer({});
    VirtualFree(mem, 0, MEM_RELEASE);
#endif
}

void gx2_render_vk_test_packet_batches() {
    auto require = [](bool ok, const char* text) {
        if (!ok) throw std::runtime_error(text);
    };
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = 4;
    VkQueryPool pool = VK_NULL_HANDLE;
    require(vkCreateQueryPool(gx2vk::device(), &info, nullptr, &pool) == VK_SUCCESS, "packet query pool failed");
    const auto start = std::chrono::steady_clock::now();
    {
        gx2vk::CommandBatch outer;
        vkCmdResetQueryPool(gx2vk::cmd(), pool, 0, 4);
        for (unsigned pass = 0; pass < 2; ++pass) {
            gx2vk::CommandBatch inner;
            const auto cb = gx2vk::cmd();
            vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, pool, pass * 2);
            // Over 32 MiB of deep-copied packets forces publication while a
            // nested batch is still open, plus wrap/reuse of the queue ring.
            for (unsigned i = 0; i < (pass ? 1u : 500000u); ++i) {
                VkViewport viewport{float(i & 7), 0, 16, 16, 0, 1};
                vkCmdSetViewport(cb, 0, 1, &viewport);
            }
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, pass * 2 + 1);
            gx2vk::submit(false); // must publish/drain without closing either scope
        }
    }
    require(gx2vk::finish(), "batched packet completion failed");
    uint64_t ticks[4]{};
    require(vkGetQueryPoolResults(gx2vk::device(), pool, 0, 4, sizeof(ticks), ticks, 8, VK_QUERY_RESULT_64_BIT) ==
                VK_SUCCESS,
            "batched query order/reset failed");
    for (auto tick : ticks) require(tick != 0, "batched timestamp was not recorded");
    vkDestroyQueryPool(gx2vk::device(), pool, nullptr);
    reset_command_tracking();
    std::printf("{\"packet_commands\":500001,\"wall_ms\":%.6f,\"passed\":true}\n",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
}

#include "plaza_render_stress.inl"
#include "npc_render_stress.inl"

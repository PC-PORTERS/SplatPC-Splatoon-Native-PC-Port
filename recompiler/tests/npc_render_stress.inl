// Authored skinned-character proxy (no game code, assets or scene claims).
// Characters bind their resources the way nn::g3d models do: a per-character
// matrix palette, per-shape blocks, shared materials and a scene block, all as
// guest uniform blocks (GX2Set*UniformBlock). The CPU rewrites every palette
// and shape block each frame (animation); the vertex shader performs real
// four-bone skinning from the palette. Shadow casters draw character by
// character, the colour pass draws material by material (characters
// interleave). Every shape owns a 16x16 tile whose colour encodes the exact
// palette, shape, material and scene words the GPU read in the final frame.
#include "rebrewu/logger.hpp"
void gx2_render_vk_test_npc(unsigned characters, bool push) {
    using namespace render_stress;
    using Clock = std::chrono::steady_clock;
    auto require = [](bool ok, const char* text) {
        if (!ok) throw std::runtime_error(text);
    };
    auto elapsed = [](Clock::time_point t) {
        return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    };
    constexpr unsigned shapes = 8, bones = 64, extent = 512, vertices = 384;
    require(characters >= 1 && characters * shapes <= (extent / 16) * (extent / 16), "npc count exceeds tiles");
    const unsigned frames = [] {
        const char* v = std::getenv("RBREW_BENCH_FRAMES");
        const int n = v ? std::atoi(v) : 0;
        return n > 0 ? unsigned(n) : 12u;
    }();
    constexpr uint32_t palette_bytes = bones * 48, shape_bytes = 64, material_bytes = 256, scene_bytes = 256,
                       stream_bytes = vertices * 16;
    constexpr uint32_t base = 65536, fetch = 0x7A5400;
    const uint32_t scene_base = base, material_base = scene_base + 2 * scene_bytes,
                   shape_base = material_base + shapes * material_bytes,
                   palette_base = (shape_base + characters * shapes * shape_bytes + 4095) & ~4095u,
                   mesh_base = (palette_base + characters * palette_bytes + 4095) & ~4095u;
    const size_t arena_bytes = mesh_base + size_t(shapes) * 2 * stream_bytes;
#ifdef _WIN32
    auto* mem = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, arena_bytes, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE));
    require(mem != nullptr, "npc write-watch arena failed");
#else
    std::vector<uint8_t> arena(arena_bytes);
    auto* mem = arena.data();
#endif
    // Meshes are shared by every character (one resource file per model).
    for (unsigned s = 0; s < shapes; ++s)
        for (unsigned v = 0; v < vertices; ++v) {
            // One covering triangle; the rest are skinned but zero-area, so the
            // GPU runs every vertex without hiding CPU cost behind overdraw.
            const float p[4] = {v < 3 && v % 3 == 1 ? 3.f : -1.f, v < 3 && v % 3 == 2 ? 3.f : -1.f, .5f, 1.f};
            const float w[3] = {.4f, .3f, .2f};
            uint32_t skin[4] = {((v * 5 + s) % bones) | ((v * 11 + 3) % bones) << 8 | ((v + s * 9) % bones) << 16 |
                                    ((v * 3 + 7) % bones) << 24,
                                0, 0, 0};
            std::memcpy(skin + 1, w, 12);
            std::memcpy(mem + mesh_base + (s * 2) * stream_bytes + v * 16, p, 16);
            std::memcpy(mem + mesh_base + (s * 2 + 1) * stream_bytes + v * 16, skin, 16);
        }
    for (unsigned m = 0; m < shapes; ++m) {
        uint32_t words[material_bytes / 4]{};
        words[0] = 0xABC00u + m * 17;
        for (unsigned i = 1; i < material_bytes / 4; ++i) words[i] = m * 1000 + i;
        std::memcpy(mem + material_base + m * material_bytes, words, material_bytes);
    }
    auto write_scene = [&](unsigned f) {
        for (unsigned k = 0; k < 2; ++k) {
            uint32_t words[scene_bytes / 4];
            for (unsigned i = 0; i < scene_bytes / 4; ++i) words[i] = (k ? 0x3000u : 0x1000u) + i * 0x100u + f;
            std::memcpy(mem + scene_base + k * scene_bytes, words, scene_bytes);
        }
    };
    auto bone_value = [](unsigned n, unsigned b, unsigned f, float& a, float& tx) {
        a = 1e-3f * float((n * 7 + b + f) % 31);
        tx = 1e-4f * float((n * 3 + b) % 97 + 1) + 1e-6f * float(f);
    };
    // CPU animation: every palette and shape block is rewritten each frame.
    auto animate = [&](unsigned f) {
        for (unsigned n = 0; n < characters; ++n) {
            float rows[bones * 12];
            for (unsigned b = 0; b < bones; ++b) {
                float a, tx;
                bone_value(n, b, f, a, tx);
                const float c = std::cos(a), s = std::sin(a);
                const float m[12] = {c, -s, 0, tx, s, c, 0, -tx, 0, 0, 1, 0};
                std::memcpy(rows + b * 12, m, 48);
            }
            std::memcpy(mem + palette_base + n * palette_bytes, rows, palette_bytes);
            for (unsigned s = 0; s < shapes; ++s) {
                const float m[16] = {1, 0, 0, .001f * float(s),      0,        1, 0, 0, 0,
                                     0, 1, 0, float(n * shapes + s), float(f), 0, 0};
                std::memcpy(mem + shape_base + (n * shapes + s) * shape_bytes, m, shape_bytes);
            }
        }
    };
    std::vector<R700FetchAttribute> attributes;
    for (unsigned s = 0; s < 2; ++s) attributes.push_back({s, s, 0, 0x813, 0, GX2_ENDIAN_SWAP_NONE, 0, false});
    gx2_render_store_fetch_attributes(fetch, attributes);
    gx2_render_set_active_fetch_shader(fetch);
    for (unsigned s = 0; s < 32; ++s) S.regs[0xA0E0 + s] = s < 2 ? s : 255;
    auto vertex = gx2vk::shader_module(
        VK_SHADER_STAGE_VERTEX_BIT,
        "#version 450\nlayout(location=0)in uvec4 a;layout(location=1)in uvec4 b;"
        "layout(set=0,binding=0)uniform Scene{uvec4 scene[16];};layout(set=0,binding=1)uniform Palette{vec4 "
        "bones[192];};"
        "layout(set=0,binding=2)uniform Shape{vec4 shape[4];};layout(location=0)flat out uint tag;void main(){"
        "vec4 p=vec4(uintBitsToFloat(a).xyz,1.);vec3 w=uintBitsToFloat(b.yzw);float "
        "weight[4]=float[4](w.x,w.y,w.z,1.-w.x-w.y-w.z);vec3 skinned=vec3(0.);"
        "for(int k=0;k<4;++k){uint "
        "j=(b.x>>(8*k))&63u;skinned+=weight[k]*vec3(dot(bones[j*3u],p),dot(bones[j*3u+1u],p),dot(bones[j*3u+2u],p));}"
        "vec4 q=vec4(skinned,1.);gl_Position=vec4(dot(shape[0],q),dot(shape[1],q),dot(shape[2],q),1.);"
        "tag=floatBitsToUint(bones[0].w)*0x9E3779B1u^floatBitsToUint(bones[189].w)*0x85EBCA77u^floatBitsToUint(shape[3]"
        ".x)*0xC2B2AE3Du^floatBitsToUint(shape[3].y)*0x165667B1u^scene[0].x*0x27D4EB2Fu;}\n");
    require(vertex != VK_NULL_HANDLE, "npc vertex shader failed");
    std::array<Program, shapes + 1> programs;
    for (unsigned i = 0; i < programs.size(); ++i) {
        auto& p = programs[i];
        p.id = UINT32_MAX - 96 - i;
        p.vs = vertex;
        for (auto& info : p.info) {
            info.attribute_locations.fill(-1);
            info.buffer_bindings.fill(-1);
            info.vk.uniform_buffer_bindings.fill(-1);
            info.vk.texture_bindings.fill(-1);
            info.vk.offset_tex_scale.fill(-1);
        }
        auto& vs = p.info[0];
        vs.attribute_locations[0] = 0;
        vs.attribute_locations[1] = 1;
        // VS banks: 0 scene, 1 palette, 2 shape (guest binding points 0-2).
        for (unsigned bank = 0; bank < 3; ++bank) {
            vs.buffer_bindings[bank] = int8_t(bank);
            vs.vk.uniform_buffer_bindings[bank] = int8_t(bank);
        }
        vs.vk.uniform_buffer_vec4s[0] = 16;
        vs.vk.uniform_buffer_vec4s[1] = 192;
        vs.vk.uniform_buffer_vec4s[2] = 4;
        auto& ps = p.info[1];
        if (i == shapes) {
            ps.vk.pixel_outputs = 0;
            p.ps_source = "#version 450\nvoid main(){}\n";
        } else {
            ps.vk.pixel_outputs = 1;
            // PS banks: 0 scene, 3 material (guest binding points 16, 19).
            ps.buffer_bindings[0] = 16;
            ps.vk.uniform_buffer_bindings[0] = 3;
            ps.vk.uniform_buffer_vec4s[0] = 16;
            ps.buffer_bindings[3] = 19;
            ps.vk.uniform_buffer_bindings[3] = 4;
            ps.vk.uniform_buffer_vec4s[3] = 16;
            p.ps_source = "#version 450\nlayout(set=0,binding=3)uniform SceneP{uvec4 "
                          "sp[16];};layout(set=0,binding=4)uniform Material{uvec4 mat[16];};"
                          "layout(location=0)flat in uint tag;layout(location=0)out vec4 color;void main(){uint "
                          "h=tag^(mat[0].x*0x165667B1u)^(sp[0].y*0x9E3779B9u)^" +
                          std::to_string(i * 0x01000193u) +
                          "u;"
                          "h^=h>>15u;h*=0x2C1B3C6Du;h^=h>>12u;color=vec4(float(h&255u),float((h>>8u)&255u),float((h>>"
                          "16u)&255u),255.)/255.;}\n";
        }
        require(create_program_layouts(p, push), "npc program layout failed");
    }
    auto bits = [](float v) {
        uint32_t u;
        std::memcpy(&u, &v, 4);
        return u;
    };
    auto expected_rgb = [&](unsigned n, unsigned s, unsigned f, std::optional<uint32_t> bone0_bits = {},
                            std::optional<uint32_t> bone63_bits = {}) {
        float a, tx0, tx63;
        bone_value(n, 0, f, a, tx0);
        bone_value(n, 63, f, a, tx63);
        const uint32_t scene0 = 0x1000u + f, scene_p1 = 0x1000u + 0x100u + f, material = 0xABC00u + s * 17;
        uint32_t tag = bone0_bits.value_or(bits(tx0)) * 0x9E3779B1u ^ bone63_bits.value_or(bits(tx63)) * 0x85EBCA77u ^
                       bits(float(n * shapes + s)) * 0xC2B2AE3Du ^ bits(float(f)) * 0x165667B1u ^ scene0 * 0x27D4EB2Fu;
        uint32_t h = tag ^ (material * 0x165667B1u) ^ (scene_p1 * 0x9E3779B9u) ^ (s * 0x01000193u);
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        return std::array<unsigned, 3>{h & 255u, (h >> 8) & 255u, (h >> 16) & 255u};
    };
    ColorTarget target;
    DepthTarget shadow;
    {
        target.width = target.height = extent;
        gx2vk::ImageDesc d;
        d.width = d.height = extent;
        d.usage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        require(gx2vk::create_image(target.image, d), "npc target allocation failed");
    }
    {
        gx2vk::ImageDesc d;
        d.width = d.height = extent;
        d.format = VK_FORMAT_D32_SFLOAT;
        d.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        require(gx2vk::create_image(shadow.image, d), "npc shadow allocation failed");
        shadow.width = shadow.height = extent;
    }
    IndexStream indices;
    indices.indexed = true;
    indices.type = VK_INDEX_TYPE_UINT16;
    indices.count = vertices;
    indices.span = gx2vk::upload(vertices * 2, 2);
    require(indices.span.data != nullptr, "npc indices failed");
    for (unsigned i = 0; i < vertices; ++i) {
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
                "npc timestamp pool failed");
        vkCmdResetQueryPool(gx2vk::cmd(), timestamps, 0, frames * 2);
    }
    S.raster = RasterState{};
    S.raster.viewport[2] = S.raster.viewport[3] = 16;
    S.draw_instances = 1;
    S.draw_first_instance = 0;
    S.single_instance_fetch = false;
    S.conditional_draw = true;
    S.regs[0xA204] = 1u << 19; // authored shaders emit Vulkan's [0,1] depth range
    S.framebuffer = {};
    S.framebuffer.valid = true;
    // Guest draw sequence: GX2Set*UniformBlock then the draw.
    auto draw = [&](unsigned pass, unsigned n, unsigned s) {
        const unsigned tile = n * shapes + s;
        S.raster.viewport[0] = int32_t(tile % 32 * 16);
        S.raster.viewport[1] = int32_t(tile / 32 * 16);
        gx2_render_set_attrib_buffer(0, mesh_base + (s * 2) * stream_bytes, stream_bytes, 16);
        gx2_render_set_attrib_buffer(1, mesh_base + (s * 2 + 1) * stream_bytes, stream_bytes, 16);
        gx2_render_set_uniform_block(false, 0, scene_base + (pass == 0 ? scene_bytes : 0), scene_bytes, mem);
        gx2_render_set_uniform_block(false, 1, palette_base + n * palette_bytes, palette_bytes, mem);
        gx2_render_set_uniform_block(false, 2, shape_base + tile * shape_bytes, shape_bytes, mem);
        if (pass) {
            gx2_render_set_uniform_block(true, 0, scene_base, scene_bytes, mem);
            gx2_render_set_uniform_block(true, 3, material_base + s * material_bytes, material_bytes, mem);
        }
        require(
            issue_draw_program(&programs[pass == 0 ? shapes : s], mem, 4, indices.count, 0, indices, false, nullptr),
            "npc draw failed");
    };
    auto frame = [&](unsigned f) {
        S.framebuffer.depth = &shadow;
        S.framebuffer.colors[0] = nullptr;
        S.target_masks = 0;
        S.raster.depth_test = S.raster.depth_write = true;
        for (unsigned n = 0; n < characters; ++n)
            for (unsigned s = 0; s < shapes; ++s) draw(0, n, s);
        S.framebuffer.depth = nullptr;
        S.framebuffer.colors[0] = &target;
        S.target_masks = 1;
        S.raster.depth_test = S.raster.depth_write = false;
        for (unsigned s = 0; s < shapes; ++s)
            for (unsigned n = 0; n < characters; ++n) draw(1, n, s);
    };
    auto begin_frame = [&](unsigned f) {
        gx2_write_watch_begin_batch();
        write_scene(f);
        animate(f);
        require(gx2vk::span_alive(indices.span), "npc index span expired");
        clear_image_depth(shadow.image, 1, 0);
        VkClearColorValue zero{};
        clear_image_color(target.image, zero);
    };
    // Warm every program and pipeline the measured frames use, and let the
    // upload ring reach its steady-state chunk count before timing.
    for (unsigned f = 0; f < 8; ++f) {
        begin_frame(f);
        frame(f);
        end_rendering();
        gx2vk::submit(false);
    }
    require(gx2vk::finish(), "npc warm-up completion failed");
    counts = {};
    Hooks hooks;
    const auto draws = C.draws, linked = C.programs_linked, pipelines = C.pipelines,
               ubo_uploads = C.uniform_buffer_uploads, submissions = gx2vk::submission_count();
    const auto watch_queries = gx2_write_watch_stats().queries;
    std::vector<double> cpu_samples, draw_samples;
    const auto begin = Clock::now();
    for (unsigned f = 0; f < frames; ++f) {
        const auto start = Clock::now();
        begin_frame(f);
        if (timestamps) vkCmdWriteTimestamp(gx2vk::cmd(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, f * 2);
        const auto draw_start = Clock::now();
        frame(f);
        draw_samples.push_back(elapsed(draw_start));
        end_rendering();
        if (timestamps) vkCmdWriteTimestamp(gx2vk::cmd(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, f * 2 + 1);
        gx2vk::submit(false);
        cpu_samples.push_back(elapsed(start));
    }
    require(gx2vk::finish(), "npc GPU completion failed");
    const double wall = elapsed(begin);
    const unsigned draws_per_frame = characters * shapes * 2;
    require(C.draws - draws == uint64_t(frames) * draws_per_frame, "npc draws were skipped");
    require(C.programs_linked == linked && C.pipelines == pipelines, "npc measured frames compiled new objects");
    std::vector<double> gpu_samples;
    if (timestamps) {
        std::vector<uint64_t> ticks(frames * 2);
        require(vkGetQueryPoolResults(gx2vk::device(), timestamps, 0, frames * 2, ticks.size() * 8, ticks.data(), 8,
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS,
                "npc timestamps unavailable");
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
    // Every shape's tile must show the final frame's exact palette, shape,
    // material and scene words; the shadow pass must have written depth.
    unsigned checked = 0;
    uint64_t checksum = 0;
    {
        std::vector<uint8_t> pixels;
        require(gx2vk::read_image(target.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, extent, extent, pixels),
                "npc colour readback failed");
        std::vector<uint8_t> depth;
        require(gx2vk::read_image(shadow.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, extent, extent, depth),
                "npc depth readback failed");
        for (unsigned n = 0; n < characters; ++n)
            for (unsigned s = 0; s < shapes; ++s) {
                const unsigned tile = n * shapes + s;
                for (unsigned y : {2u, 8u, 13u})
                    for (unsigned x : {2u, 8u, 13u}) {
                        const size_t px = size_t(tile / 32 * 16 + y) * extent + tile % 32 * 16 + x;
                        const auto expected = expected_rgb(n, s, frames - 1);
                        for (unsigned c = 0; c < 3; ++c) {
                            if (pixels[px * 4 + c] != expected[c])
                                RBREW_LOG_WARN("npc", "pixel character=%u shape=%u component=%u actual=%u expected=%u",
                                               n, s, c, pixels[px * 4 + c], expected[c]);
                            require(pixels[px * 4 + c] == expected[c], "npc skinned uniform output mismatch");
                            checksum = checksum * 1099511628211ull + pixels[px * 4 + c];
                        }
                        float z;
                        std::memcpy(&z, depth.data() + px * 4, 4);
                        require(std::abs(z - .5f) < 1e-5f, "npc shadow depth mismatch");
                        ++checked;
                    }
            }
    }
    // Contracts on the final frame's data, one colour draw each in a new batch:
    // a shorter block at the same address is a distinct upload whose missing
    // last bone reads the declared zero padding; a rewrite in place is seen.
    {
        const unsigned f = frames - 1;
        auto redraw = [&](uint32_t palette_size) {
            gx2_write_watch_begin_batch();
            S.framebuffer.depth = nullptr;
            S.framebuffer.colors[0] = &target;
            S.target_masks = 1;
            S.raster.depth_test = S.raster.depth_write = false;
            draw(1, 0, 0);
            gx2_render_set_uniform_block(false, 1, palette_base, palette_size, mem);
            require(issue_draw_program(&programs[0], mem, 4, indices.count, 0, indices, false, nullptr),
                    "npc contract draw failed");
            end_rendering();
            std::vector<uint8_t> pixels;
            require(gx2vk::read_image(target.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, extent, extent, pixels),
                    "npc contract readback failed");
            return std::array<unsigned, 3>{pixels[(8 * extent + 8) * 4], pixels[(8 * extent + 8) * 4 + 1],
                                           pixels[(8 * extent + 8) * 4 + 2]};
        };
        float a, tx0;
        bone_value(0, 0, f, a, tx0);
        require(redraw(palette_bytes - 48) == expected_rgb(0, 0, f, {}, 0u),
                "shorter block at a cached address reused the longer upload");
        require(redraw(palette_bytes) == expected_rgb(0, 0, f), "full block lost after a shorter binding");
        const float moved = tx0 + 1e-3f;
        std::memcpy(mem + palette_base + 12, &moved, 4);
        require(redraw(palette_bytes) == expected_rgb(0, 0, f, bits(moved)), "in-place palette write was not uploaded");
        std::memcpy(mem + palette_base + 12, &tx0, 4);
        require(redraw(palette_bytes) == expected_rgb(0, 0, f), "restored palette bytes were not uploaded");
        checked += 4;
    }
    std::printf(
        "{\"metric\":\"npc_frames\",\"characters\":%u,\"push\":%s,\"frames\":%u,\"draws_per_frame\":%u,\"bones\":%u,"
        "\"wall_ms\":%.6f,\"cpu_median_ms\":%.6f,\"cpu_p95_ms\":%.6f,\"cpu_p99_ms\":%.6f,"
        "\"draw_median_ms\":%.6f,\"draw_p95_ms\":%.6f,\"draw_p99_ms\":%.6f,\"gpu_median_ms\":%.6f,\"gpu_p95_ms\":%.6f,"
        "\"gpu_p99_ms\":%.6f,\"draws_per_sec\":%.3f,"
        "\"uniform_uploads_per_frame\":%.3f,\"write_watch_queries_per_frame\":%.3f,\"pipeline_binds\":%llu,"
        "\"descriptor_binds\":%llu,\"submissions\":%llu,\"checked_pixels\":%u,\"checksum\":%llu,\"passed\":true}\n",
        characters, push ? "true" : "false", frames, draws_per_frame, bones, wall, percentile(cpu_samples, .5),
        percentile(cpu_samples, .95), percentile(cpu_samples, .99), percentile(draw_samples, .5),
        percentile(draw_samples, .95), percentile(draw_samples, .99), percentile(gpu_samples, .5),
        percentile(gpu_samples, .95), percentile(gpu_samples, .99), frames * draws_per_frame * 1000. / wall,
        double(C.uniform_buffer_uploads - ubo_uploads) / frames,
        double(gx2_write_watch_stats().queries - watch_queries) / frames, (unsigned long long)counts.pipelines,
        (unsigned long long)counts.binds, (unsigned long long)(gx2vk::submission_count() - submissions), checked,
        (unsigned long long)checksum);
    for (auto& stage : S.uniform_blocks)
        for (auto& block : stage) block.address = block.size = 0;
    S.framebuffer = {};
    for (auto& a : S.attrib) a = {};
    S.regs[0xA204] = 0;
    end_rendering();
    gx2vk::destroy_image(target.image);
    gx2vk::destroy_image(shadow.image);
    require(gx2vk::finish(), "npc cleanup wait failed");
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

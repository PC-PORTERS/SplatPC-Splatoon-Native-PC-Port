// gx2_weather.cpp - the plaza's sky, clouds, light shafts and rain
// (gx2_weather.h, shaders in gx2_weather_shaders.h).
//
// Per frame, outside rendering, at the game's final full-screen pass:
//   sky-view LUT (sun and moon), one of 16 interleaved phases of the cloud
//   dome, light shafts, wet ground, rain. The atmosphere's transmittance and
//   multiple-scattering LUTs and the cloud noise volumes are made once.
// In the scene pass, the sky dome's colour draw (VR_Sky into the linear HDR
// target) is replaced by a full-screen sky draw at the far plane, so the
// game's own fog, bloom, lens flare and colour grading apply to it and
// transparent effects drawn later stay on top.
#include "gx2_weather.h"
#include "../../runtime/rebrewu_runtime.h"
#include "gx2_weather_math.h"
#include "gx2_weather_collision.h"
#include "gx2_weather_shaders.h"
#include "gx2_env_light.h"
#include "vk/gx2_render_vk_state.h"
#include "vk/gx2_vk_shader.h"
#include "../../runtime/port_settings.h"
#include "../../runtime/native_control_defaults.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>

namespace gx2weather {
namespace {
using Params = WeatherParams;

enum Binding : uint32_t {
    kUniform = 0,
    kTransmittance,
    kMultiScatter,
    kSkyView,
    kShape,
    kDetail,
    kDome,
    kDepth,
    kColor,
    kCover,
    kOut2D,
    kOut3D,
    kDomeImage,
    kRaysTexture,
    kAo,
    kBloom0,
    kBloom1,
    kBloom2,
    kBloom3,
    kBloom4,
    kToneMap,
    kBindingCount
};
bool storage(uint32_t b) {
    return b == kOut2D || b == kOut3D || b == kDomeImage;
}

VkDescriptorSetLayout s_set_layout = VK_NULL_HANDLE;
VkPipelineLayout s_layout = VK_NULL_HANDLE;
VkPipeline s_transmittance = VK_NULL_HANDLE, s_multiscatter = VK_NULL_HANDLE, s_skyview = VK_NULL_HANDLE,
           s_noise = VK_NULL_HANDLE, s_dome_pass = VK_NULL_HANDLE, s_rays_pass = VK_NULL_HANDLE,
           s_composite = VK_NULL_HANDLE, s_ao_pass = VK_NULL_HANDLE, s_bloom_pass = VK_NULL_HANDLE,
           s_taa_pass = VK_NULL_HANDLE, s_hist_pass = VK_NULL_HANDLE, s_curve_pass = VK_NULL_HANDLE;
std::map<std::array<uint32_t, 10>, VkPipeline> s_sky_pipelines;
std::map<VkFormat, VkPipeline> s_rain_pipelines;
VkDescriptorPool s_pool = VK_NULL_HANDLE;
unsigned s_pool_sets = 0;
gx2vk::Image s_t_lut, s_ms_lut, s_sv_lut, s_shape, s_detail, s_dome, s_dummy2d, s_dummy3d, s_dummy_storage2d,
    s_dummy_storage3d, s_scene_copy, s_result, s_rays, s_cover, s_ao, s_bloom[5], s_bloom_up[4], s_history,
    s_next_history, s_taa_out, s_hist, s_curve;
bool s_curve_valid = false;
Camera s_prev_camera;
bool s_history_valid = false;
// The HDR scene target of this frame (the sky's colour draw), for bloom.
gx2vk::Image* s_hdr = nullptr;
uint64_t s_hdr_frame = ~0ull;
std::array<float, 4> s_cover_mapping{};
std::future<RainCover> s_cover_loading;
bool s_cover_started = false;
bool s_failed = false, s_static_ready = false, s_skyview_ready = false;
uint64_t s_dome_frames = 0;
int s_dome_quality = 0;

// Calibration of the physical sky to the game's HDR scene colour: the sky at
// the game's sun elevation matches the brightness of its painted dome.
constexpr float kDayScale = 7.5f;
constexpr float kAdaptReference = 0.07f;

bool fail(const char* what) {
    s_failed = true;
    RBREW_LOG_WARN("weather", "%s unavailable; keeping the original sky", what);
    return false;
}

VkPipeline compute_pipeline(const char* source) {
    VkShaderModule module = gx2vk::shader_module(VK_SHADER_STAGE_COMPUTE_BIT, std::string(kCommon) + source);
    if (!module) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = s_layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    vkCreateComputePipelines(gx2vk::device(), gx2vk::pipeline_cache(), 1, &info, nullptr, &pipeline);
    return pipeline;
}

bool make_image(gx2vk::Image& image, VkImageType type, VkFormat format, uint32_t w, uint32_t h, uint32_t d,
                VkImageUsageFlags usage) {
    if (image && image.desc.width == w && image.desc.height == h && image.desc.depth == d &&
        image.desc.format == format)
        return true;
    gx2vk::destroy_image(image);
    gx2vk::ImageDesc desc;
    desc.type = type;
    desc.format = format;
    desc.width = w;
    desc.height = h;
    desc.depth = d;
    desc.usage = usage;
    return gx2vk::create_image(image, desc);
}
constexpr VkImageUsageFlags kStorageSampled = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;

bool prepare() {
    if (s_failed) return false;
    if (s_composite) return true;
    if (!gx2vk::format_supports(VK_FORMAT_R16G16B16A16_SFLOAT,
                                VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                    VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                    VK_FORMAT_FEATURE_BLIT_DST_BIT))
        return fail("Atmosphere (RGBA16F storage)");
    const auto device = gx2vk::device();
    VkDescriptorSetLayoutBinding bindings[kBindingCount]{};
    for (uint32_t i = 0; i < kBindingCount; ++i)
        bindings[i] = {
            i,
            i == kUniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
            : storage(i)  ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                          : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            1,
            VkShaderStageFlags(VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT),
            nullptr};
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set.bindingCount = kBindingCount;
    set.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &set, nullptr, &s_set_layout) != VK_SUCCESS)
        return fail("Atmosphere layout");
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &s_set_layout;
    if (vkCreatePipelineLayout(device, &layout, nullptr, &s_layout) != VK_SUCCESS) return fail("Atmosphere layout");
    s_transmittance = compute_pipeline(kTransmittanceShader);
    s_multiscatter = compute_pipeline(kMultiScatterShader);
    s_skyview = compute_pipeline(kSkyViewShader);
    s_noise = compute_pipeline(kNoise);
    s_dome_pass = compute_pipeline((std::string(kCloudDensity) + kCloudDome).c_str());
    s_rays_pass = compute_pipeline((std::string(kCloudDensity) + kRays).c_str());
    s_composite = compute_pipeline(kComposite);
    s_ao_pass = compute_pipeline(kAmbientOcclusion);
    s_bloom_pass = compute_pipeline(kBloom);
    s_taa_pass = compute_pipeline(kTemporal);
    s_hist_pass = compute_pipeline(kHistogram);
    s_curve_pass = compute_pipeline(kToneCurve);
    if (!s_transmittance || !s_multiscatter || !s_skyview || !s_noise || !s_dome_pass || !s_rays_pass || !s_composite ||
        !s_ao_pass || !s_bloom_pass || !s_taa_pass || !s_hist_pass || !s_curve_pass)
        return fail("Atmosphere shaders");
    const auto rgba16 = VK_FORMAT_R16G16B16A16_SFLOAT;
    if (!make_image(s_t_lut, VK_IMAGE_TYPE_2D, rgba16, 256, 64, 1, kStorageSampled) ||
        !make_image(s_ms_lut, VK_IMAGE_TYPE_2D, rgba16, 32, 32, 1, kStorageSampled) ||
        !make_image(s_sv_lut, VK_IMAGE_TYPE_2D, rgba16, 192, 216, 1, kStorageSampled) ||
        !make_image(s_shape, VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, 128, 128, 128, kStorageSampled) ||
        !make_image(s_detail, VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 32, kStorageSampled) ||
        !make_image(s_dummy2d, VK_IMAGE_TYPE_2D, rgba16, 1, 1, 1, kStorageSampled) ||
        !make_image(s_dummy3d, VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, kStorageSampled) ||
        !make_image(s_dummy_storage2d, VK_IMAGE_TYPE_2D, rgba16, 1, 1, 1, kStorageSampled) ||
        !make_image(s_dummy_storage3d, VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, kStorageSampled))
        return fail("Atmosphere images");
    return true;
}

VkDescriptorSet allocate_set() {
    const auto device = gx2vk::device();
    if (s_pool_sets == 64) {
        const auto retired = s_pool;
        gx2vk::defer([device, retired] { vkDestroyDescriptorPool(device, retired, nullptr); });
        s_pool = VK_NULL_HANDLE;
        s_pool_sets = 0;
    }
    if (!s_pool) {
        const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 * 17},
                                              {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64 * 3},
                                              {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64}};
        VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.maxSets = 64;
        info.poolSizeCount = 3;
        info.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device, &info, nullptr, &s_pool) != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool = s_pool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &s_set_layout;
    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(device, &info, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    ++s_pool_sets;
    return set;
}

VkImageView view(gx2vk::Image& image, bool depth = false) {
    gx2vk::ViewDesc v;
    v.aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (image.desc.type == VK_IMAGE_TYPE_3D) v.type = VK_IMAGE_VIEW_TYPE_3D;
    return gx2vk::image_view(image, v);
}
VkSampler make_sampler(VkFilter filter, VkSamplerAddressMode u, VkSamplerAddressMode v) {
    VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    s.magFilter = s.minFilter = filter;
    s.addressModeU = u;
    s.addressModeV = v;
    s.addressModeW = u;
    return gx2vk::sampler(s);
}

// A pass's bindings; unset ones get dummies.
struct Bind {
    gx2vk::Image* images[kBindingCount] = {};
    bool depth = false;
};
VkDescriptorSet make_set(const Bind& b, const gx2vk::Span& uniform) {
    VkDescriptorSet set = allocate_set();
    if (!set) return VK_NULL_HANDLE;
    const VkSampler linear =
        make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    const VkSampler repeat =
        make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    const VkSampler nearest =
        make_sampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    const VkSampler dome =
        make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    VkDescriptorImageInfo images[kBindingCount]{};
    VkDescriptorBufferInfo buffer{uniform.buffer, uniform.offset, sizeof(Params)};
    VkWriteDescriptorSet writes[kBindingCount]{};
    for (uint32_t i = 0; i < kBindingCount; ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        if (i == kUniform) {
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[i].pBufferInfo = &buffer;
            continue;
        }
        gx2vk::Image* image = b.images[i];
        const bool volume = i == kShape || i == kDetail || i == kOut3D;
        if (!image)
            image =
                storage(i) ? (volume ? &s_dummy_storage3d : &s_dummy_storage2d) : (volume ? &s_dummy3d : &s_dummy2d);
        VkSampler sampler = i == kShape || i == kDetail  ? repeat
                            : i == kDome                 ? dome
                            : i == kDepth || i == kCover ? nearest
                                                         : linear;
        images[i] = {storage(i) ? VK_NULL_HANDLE : sampler, view(*image, i == kDepth && b.depth),
                     VK_IMAGE_LAYOUT_GENERAL};
        writes[i].descriptorType =
            storage(i) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(gx2vk::device(), kBindingCount, writes, 0, nullptr);
    return set;
}
gx2vk::Span upload(const Params& params) {
    auto span =
        gx2vk::upload(sizeof(Params), std::max<VkDeviceSize>(16, gx2vk::caps().limits.minUniformBufferOffsetAlignment));
    if (span.data) std::memcpy(span.data, &params, sizeof params);
    return span;
}
bool dispatch(VkPipeline pipeline, const Bind& b, const Params& params, uint32_t x, uint32_t y, uint32_t z,
              gx2vk::Image* written) {
    const auto uniform = upload(params);
    if (!uniform.data) return false;
    VkDescriptorSet set = make_set(b, uniform);
    if (!set) return false;
    const auto cb = gx2vk::cmd();
    gx2vk::barrier_if_dirty();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(cb, x, y, z);
    if (written) gx2vk::note_gpu_write(*written);
    return true;
}

struct Frame {
    Camera camera;
    gx2env::SkyState sky;
    rbrew_settings::Values settings;
};
Frame s_frame;
uint64_t s_frame_index = 0;

void fill(Params& p, const Frame& f, uint32_t w, uint32_t h) {
    const Camera& c = f.camera;
    for (int i = 0; i < 4; ++i) {
        p.cam0[i] = c.inverse_view[i];
        p.cam1[i] = c.inverse_view[4 + i];
        p.cam2[i] = c.inverse_view[8 + i];
    }
    std::copy(c.projection.begin(), c.projection.end(), p.proj);
    p.depth_range[0] = c.depth_range[0];
    p.depth_range[1] = c.depth_range[1];
    static const bool bloom_off = [] {
        const char* v = std::getenv("RBREW_BLOOM");
        return v && v[0] == '0';
    }();
    p.depth_range[2] = bloom_off ? 0.f : f.settings.bloom / 100.f * 0.9f; // bloom (RBREW_BLOOM=0: off)
    p.depth_range[3] =
        f.settings.shader_quality >= 2 ? 0.45f : 0.f; // supplemental AO, on top of the game's baked occlusion
    const auto& s = f.sky;
    p.sun[0] = s.sun[0];
    p.sun[1] = s.sun[1];
    p.sun[2] = s.sun[2];
    p.sun[3] = float(std::fmod(s.seconds, 100000.0));
    // Moonlight: far brighter than life so the night stays readable.
    p.moon[0] = s.moon[0];
    p.moon[1] = s.moon[1];
    p.moon[2] = s.moon[2];
    p.moon[3] = 0.0011f;
    p.pole[0] = s.pole[0];
    p.pole[1] = s.pole[1];
    p.pole[2] = s.pole[2];
    p.pole[3] = s.star_angle;
    const int quality = std::clamp(f.settings.shader_quality, 1, 3);
    p.exposure[0] = kDayScale;
    p.exposure[1] = kAdaptReference;
    p.exposure[2] = 0.006f * s.lamps;
    p.exposure[3] = float(quality);
    p.clouds[0] = f.settings.clouds ? s.cloud_cover : 0.f;
    p.clouds[1] = s.rain;
    p.clouds[2] = float(s_frame_index % 100000);
    // Interleaved refresh order (Bayer): grid x grid phases.
    const int grid = quality >= 3 ? 8 : 4;
    auto bayer = [](int i, int n) {
        int x = 0, y = 0;
        for (int bit = 0, m = n; m > 1; m >>= 1, ++bit) {
            const int a = (i >> (2 * bit)) & 1, b = (i >> (2 * bit + 1)) & 1;
            x = (x << 1) | (a ^ b);
            y = (y << 1) | a;
        }
        return y * n + x;
    };
    p.clouds[3] = float(bayer(int(s_dome_frames % uint64_t(grid * grid)), grid));
    // Cloud clock: the weather clock scaled by the cloud speed setting (a
    // jump - new weather, time set in the menu - does not move the clouds).
    static CloudClock cloud_clock;
    const double clouds_t = cloud_clock.advance(s.seconds, f.settings.cloud_speed);
    p.cloud_control[0] = float(std::fmod(clouds_t, 100000.0));
    p.cloud_control[1] = s.post_only ? 1.f : 0.f;
    p.cloud_control[2] = f.settings.clarity / 100.f;
    p.grade_tools[0] = f.settings.gamma / 100.f;
    p.grade_tools[1] = f.settings.shadow_lift / 100.f;
    p.grade_tools[2] = f.settings.highlight_softness / 100.f;
    p.grade_tools[3] = f.settings.sharpness / 100.f;
    { // menu: wind direction turns the drift
        const double a = f.settings.wind_direction * 3.14159265358979 / 180.0, wx = clouds_t * 0.011,
                     wz = clouds_t * 0.004;
        p.wind[0] = float(wx * std::cos(a) - wz * std::sin(a));
        p.wind[1] = float(wx * std::sin(a) + wz * std::cos(a));
    }
    p.wind[2] = f.settings.clouds ? std::clamp(0.7f - s.cloud_cover * 0.7f, 0.f, 0.6f) : 0.f;
    // Sun visibility through the cloud layer as a whole; single clouds
    // already hide the sun in the shaders.
    p.wind[3] = std::clamp((0.95f - s.cloud_cover) * 4.f, 0.f, 1.f);
    p.target[0] = float(w);
    p.target[1] = float(h);
    p.target[2] = 1.f / float(w);
    p.target[3] = 1.f / float(h);
    if (s_dome) {
        p.dome[0] = float(s_dome.desc.width);
        p.dome[1] = float(s_dome.desc.height);
        p.dome[2] = 1.f / p.dome[0];
    }
    p.dome[3] = float(grid);
    p.effects[0] = f.settings.sun_rays ? 0.5f : 0.f; // x(1 + 1.6 warm) at sunrise and sunset
    p.effects[1] = s.wetness;
    p.effects[2] = quality == 1 ? 32.f : quality == 2 ? 64.f : 80.f;
    p.effects[3] = s.lamps;
    if (s_cover) std::copy(s_cover_mapping.begin(), s_cover_mapping.end(), p.cover);
    // The learned grade only nudges the picture: applied fully, a global
    // colour change reads as a filter (RBREW_LOOK_STRENGTH, 0..1).
    static const float look_strength = [] {
        const char* v = std::getenv("RBREW_LOOK_STRENGTH");
        return v ? std::clamp(float(std::atof(v)), 0.f, 1.f) : 0.3f;
    }();
    p.look[0] = f.settings.realistic_look ? std::max(look_strength, 0.001f) : 0.f;
    static const bool dof_off = [] {
        const char* v = std::getenv("RBREW_DOF");
        return v && v[0] == '0';
    }();
    p.look[3] = dof_off || !s.plaza ? 0.f : 2.5f * float(h) / 1440.f;
    static const bool haze_off = [] {
        const char* v = std::getenv("RBREW_HAZE");
        return v && v[0] == '0';
    }();
    p.taa[2] = haze_off ? 1.f : 0.f; // RBREW_HAZE=0: no aerial perspective
    p.post[0] = f.settings.antialiasing ? 1.f : 0.f;
    p.post[1] = f.settings.film_look ? 0.015f : 0.f;
    p.post[2] = f.settings.film_look ? 2.f / float(std::max(w, 1u)) : 0.f; // subpixel fringes at every resolution
    p.post[3] = float(s_frame_index % 997);
    // The wiggle moves the real camera now (hooks/gx2_camera_hooks.cpp);
    // RBREW_IMAGE_SWAY=1: the old shift of the picture in cinema mode.
    static const bool image_sway = [] {
        const char* v = std::getenv("RBREW_IMAGE_SWAY");
        return v && v[0] == '1';
    }();
    if (image_sway && f.settings.camera_motion && f.settings.cinema_mode) {
        // Several slow sines: a breathing, hand-held drift (no shake).
        const double t = s.seconds;
        const double k = f.settings.sway_strength / 100.0; // menu: sway strength
        p.sway[0] = float(k * (0.0016 * std::sin(t * 0.61) + 0.0009 * std::sin(t * 1.37 + 1.3)));
        p.sway[1] = float(k * (0.0013 * std::sin(t * 0.47 + 0.7) + 0.0008 * std::sin(t * 1.19 + 2.1)));
        p.sway[2] = float(k * 0.0018 * std::sin(t * 0.33 + 0.4));
        p.sway[3] = float(1.0 + 0.012 * std::max(k, 1.0));
    }
    // Dense cloud banks: coverage changes their size, not their opacity.
    {
        static const bool hero_off = [] {
            const char* v = std::getenv("RBREW_HERO_CLOUD");
            return v && v[0] == '0';
        }();
        const float coverage =
            cumulus_coverage(f.settings.cumulus_amount, f.settings.weather == rbrew_settings::kAfterRain,
                             !hero_off && f.settings.clouds && f.settings.big_cloud);
        const float clear = coverage > 0 ? 1.f : 0.f;
        p.hero[0] = clear;
        // Fixed in the world: behind the tower as seen from the plaza's main
        // view (-z), in the gap right of the tower. Keep the cloud beyond the
        // skyline so it has an atmospheric scale rather than filling the view.
        const float dx = 0.17f, dz = -0.985f;
        // It drifts with the wind (6 m/s across the view), dissolves at the
        // end of its path and a new one builds up where it started.
        const double cycle = 16.0, km = std::fmod(clouds_t * 0.006 + 6.0, cycle); // starts in view
        const float along = float(km - cycle / 2);
        const float life =
            std::min(std::clamp(float(km) / 2.5f, 0.f, 1.f), std::clamp(float(cycle - km) / 2.5f, 0.f, 1.f));
        p.hero[0] *= life;
        p.hero[1] = dx * 16.f + along * 0.96f;
        p.hero[2] = dz * 16.f + along * 0.28f;
        p.hero[3] = .55f + .65f * coverage;
        // A second one across the plaza (behind the camera in the main view),
        // drifting the other way on its own cycle.
        const double km2 = std::fmod(clouds_t * 0.005 + 3.0, cycle);
        const float along2 = float(km2 - cycle / 2);
        const float life2 =
            std::min(std::clamp(float(km2) / 2.5f, 0.f, 1.f), std::clamp(float(cycle - km2) / 2.5f, 0.f, 1.f));
        p.extra[1] = -dx * 18.f - along2 * 0.9f;
        p.extra[2] = -dz * 18.f + along2 * 0.35f;
        p.extra[3] = coverage > .3f ? clear * life2 : 0.f;
    }
    p.cinema[0] = f.settings.focus_blur / 100.f * 9.f * float(h) / 1440.f; // up to 9 px at 1440p
    p.cinema[1] = f.settings.vignette / 100.f;
    p.cinema[2] = f.settings.white_balance / 50.f;
    p.picture[0] = f.settings.saturation / 100.f;
    p.picture[1] = f.settings.contrast / 100.f;
    p.picture[2] = f.settings.exposure / 4.f;
    p.picture[3] = f.settings.fog / 100.f;
    // Aerial perspective is tuned for the plaza's distances: Octo Valley's far
    // islands turned white with it. RBREW_HAZE_ANYWHERE=1 keeps it.
    static const bool haze_anywhere = [] {
        const char* v = std::getenv("RBREW_HAZE_ANYWHERE");
        return v && v[0] == '1';
    }();
    if (!s.plaza && !haze_anywhere) p.taa[2] = 1.f;
    // Other scenes need their own collision cover before using physical rain/puddles.
    if (!s.plaza) {
        p.clouds[1] = 0;
        p.effects[1] = 0;
    }
    p.extra[0] = 1.5f * std::clamp((s.cloud_cover - 0.25f) / 0.5f, 0.f, 1.f) * (1.f - 0.6f * s.rain);
    p.storm[0] = s.lightning;
    p.storm[1] = s.bolt_seed;
    p.storm[2] = std::cos(s.bolt_azimuth);
    p.storm[3] = std::sin(s.bolt_azimuth);
    if (s.post_only) {
        // Story level: picture effects only. No clouds, shafts, haze,
        // fog, weather or sky; the sun is the level's own light.
        p.clouds[0] = 0.f;
        p.clouds[1] = 0.f;
        p.effects[0] = 0.f;
        p.effects[1] = 0.f;
        p.hero[0] = 0.f;
        p.extra[0] = 0.f;
        p.extra[3] = 0.f;
        p.taa[2] = 1.f;
        p.picture[3] = 0.f;
        p.storm[0] = 0.f;
        p.wind[2] = 0.f;
        p.wind[3] = 0.f;
        p.exposure[2] = 0.f;
    }
}

RainCover load_cover() {
    const char* configured = std::getenv("RBREW_CONTENT_DIR");
    const auto path =
        std::filesystem::path(configured && *configured ? configured : "content") / "Model" / "Fld_Plaza00.szs";
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto size = file.tellg();
    if (size < 16 || size > (128 << 20)) return {};
    std::vector<uint8_t> packed(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(packed.data()), size)) return {};
    std::vector<uint8_t> archive;
    if (!unpack_yaz(packed, archive)) return {};
    std::vector<RoofTriangle> triangles;
    if (!collision_triangles(archive_member(archive, "Fld_Plaza00.kcl"), triangles)) return {};
    return rain_cover(triangles);
}

// Once: atmosphere LUTs and noise volumes. Per frame: sky-view LUT and one
// dome phase. Outside rendering.
void update(const Frame& f, uint32_t w, uint32_t h) {
    Params p{};
    fill(p, f, w, h);
    if (!s_static_ready) {
        Bind b;
        b.images[kOut2D] = &s_t_lut;
        dispatch(s_transmittance, b, p, 256 / 8, 64 / 8, 1, &s_t_lut);
        Bind m;
        m.images[kTransmittance] = &s_t_lut;
        m.images[kOut2D] = &s_ms_lut;
        dispatch(s_multiscatter, m, p, 32 / 8, 32 / 8, 1, &s_ms_lut);
        Params n = p;
        n.effects[0] = 0;
        Bind shape;
        shape.images[kOut3D] = &s_shape;
        dispatch(s_noise, shape, n, 128 / 4, 128 / 4, 128 / 4, &s_shape);
        n.effects[0] = 1;
        Bind detail;
        detail.images[kOut3D] = &s_detail;
        dispatch(s_noise, detail, n, 32 / 4, 32 / 4, 32 / 4, &s_detail);
        s_static_ready = true;
    }
    Bind sv;
    sv.images[kTransmittance] = &s_t_lut;
    sv.images[kMultiScatter] = &s_ms_lut;
    sv.images[kOut2D] = &s_sv_lut;
    dispatch(s_skyview, sv, p, 192 / 8, 216 / 8, 1, &s_sv_lut);
    s_skyview_ready = true;
    const int quality = std::clamp(f.settings.shader_quality, 1, 3);
    const uint32_t dw = quality == 1 ? 1024 : quality == 2 ? 2048 : 4096;
    const uint32_t grid = quality >= 3 ? 8 : 4;
    if (quality != s_dome_quality || !s_dome) {
        if (!make_image(s_dome, VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, dw, dw / 4, 1, kStorageSampled))
            return;
        s_dome_quality = quality;
        s_dome_frames = 0;
        fill(p, f, w, h);
    }
    p.clouds[2] = float(s_dome_frames);
    Bind d;
    d.images[kTransmittance] = &s_t_lut;
    d.images[kSkyView] = &s_sv_lut;
    d.images[kShape] = &s_shape;
    d.images[kDetail] = &s_detail;
    d.images[kDomeImage] = &s_dome;
    dispatch(s_dome_pass, d, p, (dw / grid + 7) / 8, (dw / 4 / grid + 7) / 8, 1, &s_dome);
    ++s_dome_frames;
}

VkPipeline sky_pipeline(const VkFormat* colors, VkFormat depth, bool stencil) {
    std::array<uint32_t, 10> key{};
    for (int i = 0; i < 8; ++i) key[i] = uint32_t(colors[i]);
    key[8] = uint32_t(depth);
    key[9] = stencil;
    auto [entry, inserted] = s_sky_pipelines.emplace(key, VK_NULL_HANDLE);
    if (!inserted) return entry->second;
    const auto vertex = gx2vk::shader_module(VK_SHADER_STAGE_VERTEX_BIT, std::string(kCommon) + kSkyVertex);
    const auto fragment = gx2vk::shader_module(VK_SHADER_STAGE_FRAGMENT_BIT, std::string(kCommon) + kSkyFragment);
    if (!vertex || !fragment) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto& stage : stages) {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth_state{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_state.depthTestEnable = depth != VK_FORMAT_UNDEFINED;
    depth_state.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState blend[8]{};
    blend[0].colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 8;
    blending.pAttachments = blend;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 8;
    rendering.pColorAttachmentFormats = colors;
    rendering.depthAttachmentFormat = depth;
    rendering.stencilAttachmentFormat = stencil ? depth : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &samples;
    info.pDepthStencilState = &depth_state;
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamic;
    info.layout = s_layout;
    if (vkCreateGraphicsPipelines(gx2vk::device(), gx2vk::pipeline_cache(), 1, &info, nullptr, &entry->second) !=
        VK_SUCCESS)
        RBREW_LOG_ERROR("weather", "Sky pipeline unavailable");
    return entry->second;
}

VkPipeline rain_pipeline(VkFormat format) {
    auto [entry, inserted] = s_rain_pipelines.emplace(format, VK_NULL_HANDLE);
    if (!inserted) return entry->second;
    const auto vertex = gx2vk::shader_module(VK_SHADER_STAGE_VERTEX_BIT, std::string(kCommon) + kRainVertex);
    const auto fragment = gx2vk::shader_module(VK_SHADER_STAGE_FRAGMENT_BIT, std::string(kCommon) + kRainFragment);
    if (!vertex || !fragment) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto& stage : stages) {
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &samples;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamic;
    info.layout = s_layout;
    if (vkCreateGraphicsPipelines(gx2vk::device(), gx2vk::pipeline_cache(), 1, &info, nullptr, &entry->second) !=
        VK_SUCCESS)
        RBREW_LOG_ERROR("weather", "Rain pipeline unavailable");
    return entry->second;
}

bool atmosphere_off() {
    static const bool off = [] {
        const char* v = std::getenv("RBREW_ATMOSPHERE");
        return v && v[0] == '0';
    }();
    return off;
}
} // namespace

bool set_rain_cover(const RainCover& cover) {
    if (cover.size < 4 || cover.size > 2048 || cover.heights.size() != size_t(cover.size) * cover.size ||
        !std::all_of(cover.mapping.begin(), cover.mapping.end(), [](float v) { return std::isfinite(v); }) ||
        cover.mapping[2] <= 0 || cover.mapping[3] <= 0 ||
        !std::all_of(cover.heights.begin(), cover.heights.end(), [](float v) { return std::isfinite(v); }))
        return false;
    gx2r::end_rendering();
    if (!make_image(s_cover, VK_IMAGE_TYPE_2D, VK_FORMAT_R32_SFLOAT, cover.size, cover.size, 1,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return false;
    const auto bytes = cover.heights.size() * sizeof(float);
    const auto span = gx2vk::upload(bytes, 16);
    if (!span.data) return false;
    std::memcpy(span.data, cover.heights.data(), bytes);
    VkBufferImageCopy copy{};
    copy.bufferOffset = span.offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {cover.size, cover.size, 1};
    gx2vk::barrier_if_dirty();
    vkCmdCopyBufferToImage(gx2vk::cmd(), span.buffer, s_cover.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    gx2vk::note_gpu_write(s_cover);
    s_cover_mapping = cover.mapping;
    gx2r::note_framebuffer_changed();
    return true;
}

bool apply(gx2vk::Image& color, gx2vk::Image& depth, const Camera& camera, const rbrew_settings::Values& settings,
           double seconds, double cycle_seconds) {
    (void)seconds;
    (void)cycle_seconds;
    if (!settings.shader_quality || !color || !depth ||
        !matching_scene_aspect(color.desc.width, color.desc.height, depth.desc.width, depth.desc.height) || !prepare())
        return false;
    if (!std::isfinite(camera.depth_range[0]) || !std::isfinite(camera.depth_range[1]) ||
        std::abs(camera.depth_range[1] - camera.depth_range[0]) < 1e-6f)
        return false;
    if (!(depth.desc.usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
        !gx2vk::format_supports(color.desc.format, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT))
        return false;
    gx2r::end_rendering();
    if (s_cover_loading.valid() && s_cover_loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            if (!set_rain_cover(s_cover_loading.get()))
                RBREW_LOG_WARN("weather", "Collision cover unavailable; rain stays off");
        } catch (const std::exception&) {
            RBREW_LOG_WARN("weather", "Could not load the collision cover; rain stays off");
        }
    }
    static bool format_logged = false;
    if (!format_logged && std::getenv("RBREW_WEATHER_LOG")) {
        format_logged = true;
        RBREW_LOG_DEBUG("weather", "final image format %d", int(color.desc.format));
    }
    Frame f = s_frame;
    f.camera = camera;
    f.settings = settings;
    const uint32_t w = color.desc.width, h = color.desc.height;
    update(f, w, h);
    ++s_frame_index;
    Params p{};
    fill(p, f, w, h);
    const bool rays = settings.sun_rays && f.sky.sun_elevation > -1.5f && f.sky.rain < 0.6f;
    {
        if (!make_image(s_scene_copy, VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, 1,
                        kStorageSampled | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
            !make_image(s_result, VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, 1,
                        kStorageSampled | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
            !make_image(s_rays, VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, (w + 1) / 2, (h + 1) / 2, 1,
                        kStorageSampled) ||
            !make_image(s_ao, VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, (w + 1) / 2, (h + 1) / 2, 1,
                        kStorageSampled))
            return false;
        const auto cb = gx2vk::cmd();
        gx2vk::barrier_if_dirty();
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = blit.dstOffsets[1] = {int32_t(w), int32_t(h), 1};
        vkCmdBlitImage(cb, color.image, VK_IMAGE_LAYOUT_GENERAL, s_scene_copy.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                       VK_FILTER_NEAREST);
        gx2vk::note_gpu_write(s_scene_copy);
        if (rays) {
            Bind r;
            r.images[kDepth] = &depth;
            r.depth = true;
            r.images[kDome] = &s_dome;
            r.images[kOut2D] = &s_rays;
            r.images[kShape] = &s_shape;
            r.images[kDetail] = &s_detail; // volumetric shafts sample the clouds
            dispatch(s_rays_pass, r, p, (s_rays.desc.width + 7) / 8, (s_rays.desc.height + 7) / 8, 1, &s_rays);
        }
        if (p.depth_range[3] > 0) {
            Bind a;
            a.images[kDepth] = &depth;
            a.depth = true;
            a.images[kOut2D] = &s_ao;
            dispatch(s_ao_pass, a, p, (s_ao.desc.width + 7) / 8, (s_ao.desc.height + 7) / 8, 1, &s_ao);
        }
        // Bloom chain from the HDR scene target (compute, kBloom).
        bool bloom = p.depth_range[2] > 0 && s_hdr && s_hdr_frame == gx2vk::present_count() && *s_hdr;
        constexpr VkImageUsageFlags kBloomUsage = kStorageSampled;
        for (int i = 0; bloom && i < 5; ++i) {
            const uint32_t bw = std::max(1u, w >> (i + 1)), bh = std::max(1u, h >> (i + 1));
            bloom = make_image(s_bloom[i], VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, bw, bh, 1, kBloomUsage) &&
                    (i == 4 || make_image(s_bloom_up[i], VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, bw, bh, 1,
                                          kBloomUsage));
        }
        if (bloom) {
            Params bp = p;
            gx2vk::Image* from = s_hdr;
            for (int i = 0; i < 5; ++i) {
                bp.effects[0] = i == 0 ? 0.f : 1.f;
                Bind b;
                b.images[kColor] = from;
                b.images[kOut2D] = &s_bloom[i];
                dispatch(s_bloom_pass, b, bp, (s_bloom[i].desc.width + 7) / 8, (s_bloom[i].desc.height + 7) / 8, 1,
                         &s_bloom[i]);
                from = &s_bloom[i];
            }
            bp.effects[0] = 2.f;
            for (int i = 3; i >= 0; --i) {
                Bind b;
                b.images[kColor] = from;
                b.images[kRaysTexture] = &s_bloom[i];
                b.images[kOut2D] = &s_bloom_up[i];
                dispatch(s_bloom_pass, b, bp, (s_bloom_up[i].desc.width + 7) / 8, (s_bloom_up[i].desc.height + 7) / 8,
                         1, &s_bloom_up[i]);
                from = &s_bloom_up[i];
            }
        }
        // Adaptive tone matching to the reference (RBREW_TONE_MATCH=0: the fixed curve).
        static const bool tone_match = [] {
            const char* v = std::getenv("RBREW_TONE_MATCH");
            return v && v[0] == '1';
        }(); // experimental, off by default
        bool curve = tone_match && settings.realistic_look &&
                     make_image(s_hist, VK_IMAGE_TYPE_2D, VK_FORMAT_R32_UINT, 128, 1, 1,
                                kStorageSampled | VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
                     make_image(s_curve, VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, 256, 1, 1, kStorageSampled);
        if (curve) {
            static bool cleared = false;
            if (!cleared) {
                gx2vk::barrier_if_dirty();
                const VkClearColorValue zero{};
                const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(gx2vk::cmd(), s_hist.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
                gx2vk::note_gpu_write(s_hist);
                cleared = true;
            }
            Params hp = p;
            hp.look[1] = 0.8f; // how far towards the reference distribution (partly: no equalising of dark corners)
            hp.look[2] = s_curve_valid ? 1.f : 0.f;
            // The histogram was gathered by the previous frame's composite.
            Bind cb2;
            cb2.images[kDomeImage] = &s_hist;
            cb2.images[kOut2D] = &s_curve;
            dispatch(s_curve_pass, cb2, hp, 1, 1, 1, &s_curve);
            s_curve_valid = true;
        } else
            s_curve_valid = false;
        Params cp = p;
        cp.look[1] = curve ? 1.f : 0.f;
        if (!rays) cp.effects[0] = 0;
        if (!bloom) cp.depth_range[2] = 0;
        Bind c;
        c.images[kTransmittance] = &s_t_lut;
        c.images[kShape] = &s_shape;
        c.images[kDepth] = &depth;
        c.depth = true;
        c.images[kColor] = &s_scene_copy;
        c.images[kCover] = s_cover ? &s_cover : nullptr;
        c.images[kOut2D] = &s_result;
        c.images[kRaysTexture] = rays ? &s_rays : nullptr;
        c.images[kAo] = p.depth_range[3] > 0 ? &s_ao : nullptr;
        c.images[kSkyView] = &s_sv_lut;
        c.images[kDome] = &s_dome;
        c.images[kBloom0] = bloom ? &s_bloom_up[0] : nullptr;
        c.images[kToneMap] = curve ? &s_curve : nullptr;
        if (curve) c.images[kDomeImage] = &s_hist;
        dispatch(s_composite, c, cp, (w + 7) / 8, (h + 7) / 8, 1, &s_result);
        gx2vk::Image* final_image = &s_result;
        // Temporal AA (RBREW_TAA=0: off) with the previous frame's camera.
        static const bool taa_enabled = [] {
            const char* v = std::getenv("RBREW_TAA");
            return !(v && v[0] == '0');
        }();
        s_history_valid =
            s_history_valid && s_history && s_next_history && s_history.desc.width == w && s_history.desc.height == h;
        if (taa_enabled && settings.antialiasing && !(settings.camera_motion && settings.cinema_mode) &&
            make_image(s_history, VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, 1,
                       kStorageSampled | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
            make_image(s_next_history, VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, 1, kStorageSampled) &&
            make_image(s_taa_out, VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, 1,
                       kStorageSampled | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
            Params tp = p;
            for (int i = 0; i < 4; ++i) {
                tp.prev0[i] = s_prev_camera.inverse_view[i];
                tp.prev1[i] = s_prev_camera.inverse_view[4 + i];
                tp.prev2[i] = s_prev_camera.inverse_view[8 + i];
            }
            std::copy(s_prev_camera.projection.begin(), s_prev_camera.projection.end(), tp.prev_proj);
            tp.taa[0] = s_history_valid ? 1.f : 0.f;
            tp.taa[1] = 0.88f;
            tp.taa[3] = settings.motion_blur ? 0.5f : 0.f; // shutter fraction of the frame
            Bind t;
            t.images[kColor] = &s_result;
            t.images[kDepth] = &depth;
            t.depth = true;
            t.images[kRaysTexture] = &s_history;
            t.images[kOut2D] = &s_taa_out;
            t.images[kDomeImage] = &s_next_history;
            if (dispatch(s_taa_pass, t, tp, (w + 7) / 8, (h + 7) / 8, 1, &s_taa_out)) {
                gx2vk::note_gpu_write(s_next_history);
                std::swap(s_history, s_next_history);
                final_image = &s_taa_out;
                s_history_valid = true;
            } else
                s_history_valid = false;
        } else
            s_history_valid = false;
        s_prev_camera = camera;
        gx2vk::barrier_if_dirty();
        vkCmdBlitImage(gx2vk::cmd(), final_image->image, VK_IMAGE_LAYOUT_GENERAL, color.image, VK_IMAGE_LAYOUT_GENERAL,
                       1, &blit, VK_FILTER_NEAREST);
        gx2vk::note_gpu_write(color);
    }
    if (p.clouds[1] > 0.001f && s_cover && (color.desc.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        if (const auto pipeline = rain_pipeline(color.desc.format)) {
            const auto uniform = upload(p);
            Bind b;
            b.images[kDepth] = &depth;
            b.depth = true;
            b.images[kTransmittance] = &s_t_lut;
            b.images[kCover] = &s_cover;
            VkDescriptorSet set = uniform.data ? make_set(b, uniform) : VK_NULL_HANDLE;
            if (set) {
                const auto cb = gx2vk::cmd();
                gx2vk::barrier_if_dirty();
                VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                attachment.imageView = view(color);
                attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
                rendering.renderArea.extent = {w, h};
                rendering.layerCount = 1;
                rendering.colorAttachmentCount = 1;
                rendering.pColorAttachments = &attachment;
                vkCmdBeginRendering(cb, &rendering);
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 0, 1, &set, 0, nullptr);
                const VkViewport viewport{0, 0, float(w), float(h), 0, 1};
                const VkRect2D scissor{{0, 0}, {w, h}};
                vkCmdSetViewport(cb, 0, 1, &viewport);
                vkCmdSetScissor(cb, 0, 1, &scissor);
                const unsigned grid = unsigned(p.effects[2]);
                vkCmdDraw(cb, 12, grid * grid, 0, 0);
                vkCmdEndRendering(cb);
                gx2vk::note_gpu_write(color);
            }
        }
    }
    // RBREW_SKY_DUMP=<frame>: the dome and the sky-view LUT as raw RGBA16F
    // files next to the executable (diagnostics).
    static const uint64_t dump_at = [] {
        const char* v = std::getenv("RBREW_SKY_DUMP");
        return v ? std::strtoull(v, nullptr, 10) : 0ull;
    }();
    static bool dumped = false;
    if (dump_at && !dumped && gx2vk::present_count() >= dump_at) {
        dumped = true;
        std::vector<uint8_t> raw;
        for (auto [image, name] :
             {std::pair{&s_dome, "sky_dome.rgba16f"}, std::pair{&s_sv_lut, "sky_view.rgba16f"},
              std::pair{&s_t_lut, "sky_transmittance.rgba16f"}, std::pair{&s_curve, "tone_curve.rgba16f"},
              std::pair{&s_scene_copy, "scene_copy.rgba16f"}, std::pair{&s_result, "post_result.rgba16f"}}) {
            if (gx2vk::read_image(*image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, image->desc.width, image->desc.height,
                                  raw)) {
                if (FILE* out = std::fopen(name, "wb")) {
                    std::fwrite(raw.data(), 1, raw.size(), out);
                    std::fclose(out);
                }
                RBREW_LOG_INFO("weather", "dumped %s %ux%u", name, image->desc.width, image->desc.height);
            }
        }
    }
    // The next frame's sky draw samples the LUTs and the dome inside rendering.
    gx2vk::barrier_if_dirty();
    gx2r::note_framebuffer_changed();
    return true;
}

bool find_camera(uint8_t* memory, Camera& camera); // below
// A sky dome: every vertex about equally far from the model's origin, and
// far (a big sphere or hemisphere). Distant buildings are not spherical
// around their origin, so this does not catch them.
bool is_dome(uint8_t* memory, uint32_t addr, uint32_t size, uint32_t stride) {
    static std::map<uint64_t, bool> cache;
    const uint64_t key = (uint64_t(addr) << 32) | size;
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    bool dome = false;
    const uint32_t n = size / stride;
    if (n >= 64 && rbrew_memory_accessible(memory, addr, size)) {
        auto rf = [&](uint32_t a) {
            uint32_t v;
            std::memcpy(&v, memory + a, 4);
            v = rbrew_bswap32(v);
            float f;
            std::memcpy(&f, &v, 4);
            return f;
        };
        double sum = 0, lo = 1e30, hi = 0, ylo = 1e30, yhi = -1e30;
        uint32_t used = 0;
        for (uint32_t i = 0; i < n; i += std::max(1u, n / 96)) {
            const float x = rf(addr + i * stride), y = rf(addr + i * stride + 4), z = rf(addr + i * stride + 8);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
                used = 0;
                break;
            }
            const double r = std::sqrt(double(x) * x + double(y) * y + double(z) * z);
            sum += r;
            lo = std::min(lo, r);
            hi = std::max(hi, r);
            ylo = std::min(ylo, double(y));
            yhi = std::max(yhi, double(y));
            ++used;
        }
        if (used >= 32) {
            const double mean = sum / used;
            // A round dome, or a huge sky sphere that is not quite round (Octo
            // Valley: radius ~3.6-18.7 km, spanning far above and below).
            dome = (mean > 300.0 && (hi - lo) < 0.12 * mean && (yhi - ylo) > 0.4 * mean) ||
                   (mean > 8000.0 && (yhi - ylo) > mean);
        }
    }
    cache.emplace(key, dome);
    if (dome) RBREW_LOG_INFO("weather", "sky dome found (%u vertices, %u bytes)", n, size);
    return dome;
}
bool sky_draw(uint8_t* memory, uint32_t count) {
    if (atmosphere_off() || !rbrew_settings::g_shader_quality.load(std::memory_order_relaxed)) return false;
    const auto& s = gx2r::state();
    const auto& a = s.attrib[0];
    if (!a.guest_addr || !s.framebuffer.depth || s.framebuffer.depth->width < 640 ||
        std::abs(double(s.framebuffer.depth->width) * 9.0 - double(s.framebuffer.depth->height) * 16.0) >
            0.02 * double(s.framebuffer.depth->height) * 16.0)
        return false;
    const bool plaza_scene = s_frame.sky.plaza;
    if (plaza_scene && (!s.raster.depth_test || s.raster.depth_range[0] != 0 || s.raster.depth_range[1] != 1))
        return false;
    if (plaza_scene) { // the plaza's VR_Sky
        if (count != 2280 || a.size != 7024 || a.stride != 16 || !rbrew_memory_accessible(memory, a.guest_addr, a.size))
            return false;
        return is_sky_mesh(mesh_hash(memory + a.guest_addr, a.size), a.size, a.stride, count) && prepare();
    }
    // Other scenes with the shaders (Octo Valley, tutorial): their own dome,
    // found by its shape. RBREW_SKY_ANYWHERE=0: keep the game's sky there.
    static const bool anywhere = [] {
        const char* v = std::getenv("RBREW_SKY_ANYWHERE");
        return !(v && v[0] == '0');
    }();
    if (!anywhere || !s_frame.sky.active || s_frame.sky.post_only || count < 200 || a.stride < 12 || a.stride > 64)
        return false;
    return is_dome(memory, a.guest_addr, a.size, a.stride) && prepare();
}

bool draw_sky(uint8_t* memory, VkCommandBuffer cb, const VkFormat* colors, VkFormat depth, bool stencil) {
    // Only the colour draw into the linear HDR scene target; the depth/normal
    // pre-pass keeps the game's dome (at the far plane).
    static const bool replace = [] {
        const char* v = std::getenv("RBREW_SKY");
        return !(v && v[0] == '0');
    }();
    const auto& s = gx2r::state();
    auto* target = s.framebuffer.colors[0];
    if (!replace || !target || colors[0] != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || !s_static_ready || !s_skyview_ready ||
        !s_dome)
        return false;
    const auto sky = gx2env::sky_state();
    if (!sky.active || sky.post_only) return false; // levels keep the game's own sky
    Frame f = s_frame;
    Camera camera;
    if (!find_camera(memory, camera)) return false;
    f.camera = camera;
    f.sky = sky;
    const uint32_t w = target->image.desc.width, h = target->image.desc.height;
    const VkPipeline pipeline = sky_pipeline(colors, depth, stencil);
    if (!pipeline) return false;
    Params p{};
    fill(p, f, w, h);
    const auto uniform = upload(p);
    if (!uniform.data) return false;
    Bind b;
    b.images[kTransmittance] = &s_t_lut;
    b.images[kSkyView] = &s_sv_lut;
    b.images[kDetail] = &s_detail;
    b.images[kDome] = &s_dome;
    VkDescriptorSet set = make_set(b, uniform);
    if (!set) return false;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 0, 1, &set, 0, nullptr);
    const VkViewport viewport{0, 0, float(w), float(h), 1, 1};
    const VkRect2D scissor{{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdDraw(cb, 3, 1, 0, 0);
    gx2vk::note_gpu_write(target->image);
    s_hdr = &target->image;
    s_hdr_frame = gx2vk::present_count();
    return true;
}

namespace {
bool s_hud_hidden = false;
}
// The view block (VP, view, inverse view, near/far): bank 1 in the plaza,
// another bank in other scenes (Octo Valley) - the first that decodes.
bool find_camera(uint8_t* memory, Camera& camera) {
    const auto& s = gx2r::state();
    // A bank can remain valid while no geometry rebinds it during an upward
    // view. Prefer the fresh, identified main camera over that stale state.
    static const bool hook_camera = [] {
        const char* v = std::getenv("RBREW_HOOK_CAMERA");
        return !(v && v[0] == '0');
    }();
    gx2env::MainView v{};
    const auto* target = s.framebuffer.colors[0];
    const float aspect = target && target->image.desc.height
                             ? float(target->image.desc.width) / float(target->image.desc.height)
                             : 16.f / 9.f;
    const bool fresh = gx2env::main_view(v);
    auto log = [&](int bank) {
        static uint64_t logged = ~0ull;
        static unsigned calls = 0;
        const auto now = gx2vk::present_count();
        if (logged == ~0ull || now - logged >= 120) {
            logged = now;
            calls = 0;
        }
        if (std::getenv("RBREW_WEATHER_LOG") && now == logged && calls++ < 3) {
            const auto& iv = camera.inverse_view;
            RBREW_LOG_DEBUG("weather-camera",
                            "frame %llu bank %d hook %d fov %.2f clip %.2f %.1f view %.3f %.3f %.3f pos "
                            "%.1f %.1f %.1f target %u",
                            (unsigned long long)now, bank, int(fresh), v.fovy_deg, camera.projection[2],
                            camera.projection[3], -iv[2], -iv[6], -iv[10], iv[3], iv[7], iv[11],
                            target ? target->id : 0);
        }
    };
    if (hook_camera && fresh && look_at_camera(v.pos, v.at, v.up, v.fovy_deg, v.near_z, v.far_z, aspect, camera)) {
        log(-1);
        return true;
    }
    static int last_bank = 1;
    auto try_bank = [&](int i) {
        const auto& bank = s.uniform_blocks[0][i];
        return bank.address && bank.size >= 176 && rbrew_memory_accessible(memory, bank.address, 176) &&
               decode_camera(memory + bank.address, 176, camera);
    };
    if (try_bank(last_bank)) {
        log(last_bank);
        return true;
    }
    for (int i = 0; i < 16; ++i)
        if (i != last_bank && try_bank(i)) {
            if (i != last_bank) RBREW_LOG_DEBUG("weather", "camera block in vertex bank %d", i);
            last_bank = i;
            log(i);
            return true;
        }
    log(-2);
    return false;
}
bool hud_hidden() {
    const auto& s = gx2r::state();
    return s_hud_hidden && s.framebuffer.colors[0] && s.framebuffer.colors[0]->id == s.scan_tv_source_id;
}
void observe_draw(uint8_t* memory, uint32_t primitive, uint32_t count) {
    // Cinema mode on its own: it only needs the game's frame structure (the
    // 3D scene, its full-screen composite into the TV image, then the HUD),
    // so it also works with the shaders off and outside the plaza. The HUD
    // is every later draw into the TV image (hud_hidden). RBREW_CINEMA_ANY=0
    // limits it to the plaza with shaders on, as before.
    {
        static const bool any_scene = [] {
            const char* v = std::getenv("RBREW_CINEMA_ANY");
            return !(v && v[0] == '0');
        }();
        static uint32_t cine_frame = UINT32_MAX;
        static unsigned depth_draws = 0;
        static bool cinema = false;
        static uint32_t found_at = 0; // last frame the scene composite was found
        const auto& s = gx2r::state();
        const uint32_t now = uint32_t(gx2vk::present_count());
        if (now != cine_frame) {
            cine_frame = now;
            depth_draws = 0;
            s_hud_hidden = false;
            cinema = any_scene && rbrew_settings::get().cinema_mode;
        }
        const auto* target = s.framebuffer.colors[0];
        if (cinema && !s_hud_hidden && target) {
            if (s.framebuffer.depth) ++depth_draws;
            // Looking up at the sky leaves only a few 3D draws: once the
            // composite was found in the last second, 2 are enough.
            else if (depth_draws >= (now - found_at < 60 ? 2u : 64u) && target->id == s.scan_tv_source_id &&
                     count == 3 && (primitive & 31u) == 4u && !s.raster.depth_test && !s.raster.blend &&
                     (s.target_masks & 15u) == 15u && !s.raster.viewport[0] && !s.raster.viewport[1] &&
                     s.raster.viewport[2] == int(target->width) && s.raster.viewport[3] == int(target->height)) {
                s_hud_hidden = true;
                found_at = now;
            }
        }
    }
    if (atmosphere_off() || !rbrew_settings::g_shader_quality.load(std::memory_order_relaxed)) return;
    auto& s = gx2r::state();
    static uint32_t frame = UINT32_MAX;
    static gx2vk::Image* depth = nullptr;
    static unsigned world_draws = 0;
    static uint32_t scene_width = 16, scene_height = 9;
    static bool applied = false, allowed = false;
    static rbrew_settings::Values settings;
    const uint32_t now = uint32_t(gx2vk::present_count());
    if (now != frame) {
        for (const auto& [key, color] : s.cb_map)
            if (color.id == s.scan_tv_source_id && color.image.desc.height) {
                scene_width = color.image.desc.width;
                scene_height = color.image.desc.height;
                break;
            }
        if (!applied) s_history_valid = false;
        frame = now;
        depth = nullptr;
        world_draws = 0;
        applied = false;
        s_hud_hidden = false;
        const auto sky = gx2env::sky_state();
        if (sky.scene_type != s_frame.sky.scene_type || sky.scene_id != s_frame.sky.scene_id) {
            s_history_valid = false;
            s_curve_valid = false;
        }
        s_frame.sky = sky;
        allowed = s_frame.sky.active;
        settings = rbrew_settings::get();
        s_frame.settings = settings;
        if (allowed && !s_cover_started) {
            s_cover_started = true;
            try {
                s_cover_loading = std::async(std::launch::async, load_cover);
            } catch (const std::exception&) {
                RBREW_LOG_WARN("weather", "Could not start loading the collision cover; rain stays off");
            }
        }
    }
    auto* target = s.framebuffer.colors[0];
    // RBREW_WEATHER_WHY=1 (diagnostics): every 300 frames, why the effects
    // did or did not apply in that frame (new scenes such as Octo Valley).
    static const bool why = [] {
        const char* v = std::getenv("RBREW_WEATHER_WHY");
        return v && v[0] == '1';
    }();
    struct Why {
        unsigned draws = 0, depth_draws = 0, bank_ok = 0, cam_ok = 0, tv_tris = 0, tri_rejected = 0;
        uint32_t bank_size = 0, tw = 0, th = 0, dw = 0, dh = 0;
    };
    static Why w_now, w_last;
    static uint32_t why_frame = UINT32_MAX;
    if (why) {
        if (now != why_frame) {
            if (why_frame != UINT32_MAX && why_frame % 300 == 0)
                RBREW_LOG_DEBUG("weather-why",
                                "frame %u allowed %d applied %d draws %u depth %u bank %u (size %u) cam %u "
                                "tv-tris %u rejected %u target %ux%u depth %ux%u",
                                why_frame, int(allowed), int(applied), w_last.draws, w_last.depth_draws, w_last.bank_ok,
                                w_last.bank_size, w_last.cam_ok, w_last.tv_tris, w_last.tri_rejected, w_last.tw,
                                w_last.th, w_last.dw, w_last.dh);
            why_frame = now;
            w_now = {};
        }
        ++w_now.draws;
        if (s.framebuffer.depth) ++w_now.depth_draws;
        if (target && target->width >= 1000) {
            w_now.tw = target->width;
            w_now.th = target->height;
        }
        if (s.depth_target) {
            w_now.dw = s.depth_target->width;
            w_now.dh = s.depth_target->height;
        }
        const auto& bk = s.uniform_blocks[0][1];
        if (bk.address) {
            ++w_now.bank_ok;
            w_now.bank_size = bk.size;
        }
        if (target && target->id == s.scan_tv_source_id && count == 3 && !s.framebuffer.depth) {
            ++w_now.tv_tris;
            if (s.raster.depth_test || s.raster.blend || (s.target_masks & 15u) != 15u ||
                s.raster.viewport[2] != int(target->width))
                ++w_now.tri_rejected;
        }
        w_last = w_now;
    }
    if (!allowed || !target || applied || !scene_color_extent(target->width, target->height, s_frame.sky.plaza)) return;
    // The 3D scene's depth may be smaller than the TV image: Octo Valley
    // renders at 848x480 and scales up in the final pass (same aspect).
    auto* scene_depth = s.framebuffer.depth ? s.framebuffer.depth : s.depth_target;
    const bool same_aspect = scene_depth && scene_depth->image.desc.width >= 640 &&
                             matching_scene_aspect(scene_width, scene_height, scene_depth->image.desc.width,
                                                   scene_depth->image.desc.height) &&
                             matching_scene_aspect(target->image.desc.width, target->image.desc.height,
                                                   scene_depth->image.desc.width, scene_depth->image.desc.height);
    if (same_aspect && !depth) {
        Camera camera;
        if (find_camera(memory, camera)) {
            depth = &scene_depth->image;
            if (why) ++w_now.cam_ok;
            camera.depth_range = {float(s.raster.depth_range[0]), float(s.raster.depth_range[1])};
            s_frame.camera = camera;
        }
    }
    if (s.framebuffer.depth) {
        ++world_draws;
        if (depth == &s.framebuffer.depth->image && s.raster.depth_test) {
            const float lo = float(std::min(s.raster.depth_range[0], s.raster.depth_range[1]));
            const float hi = float(std::max(s.raster.depth_range[0], s.raster.depth_range[1]));
            if (s.raster.depth_func == gx2r::wire::GREATER || s.raster.depth_func == gx2r::wire::GEQUAL)
                s_frame.camera.depth_range = {hi, lo};
            else if (s.raster.depth_func == gx2r::wire::LESS || s.raster.depth_func == gx2r::wire::LEQUAL)
                s_frame.camera.depth_range = {lo, hi};
        }
        return;
    }
    // The game's final full-screen scene triangle precedes every HUD draw.
    // The plaza is known here: looking at the sky leaves only a few 3D
    // draws, and requiring 64 dropped every effect (a flash in the picture).
    if (!depth || world_draws < 2 || target->id != s.scan_tv_source_id || count != 3 || (primitive & 31u) != 4u ||
        s.raster.depth_test || s.raster.blend || (s.target_masks & 15u) != 15u || s.raster.viewport[0] ||
        s.raster.viewport[1] || s.raster.viewport[2] != int(target->width) ||
        s.raster.viewport[3] != int(target->height))
        return;
    applied = apply(target->image, *depth, s_frame.camera, settings, 0, 0);
    if (std::getenv("RBREW_WEATHER_LOG")) {
        static uint32_t last = 0;
        if (now - last >= 120) {
            last = now;
            RBREW_LOG_INFO("graphics", "scene %d:%d applied %d color %ux%u depth %ux%u quality %d original_sky %d",
                           s_frame.sky.scene_type, s_frame.sky.scene_id, int(applied), target->width, target->height,
                           depth->desc.width, depth->desc.height, settings.shader_quality, int(s_frame.sky.post_only));
        }
    }
    // RBREW_CAM_SCAN=<frame> (diagnostics): guest addresses holding the
    // camera position (three big-endian floats), with 192 bytes around them,
    // to cam_scan.txt next to the executable; finds the game's camera object.
    static const uint32_t cam_scan = [] {
        const char* v = std::getenv("RBREW_CAM_SCAN");
        return v ? uint32_t(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    if (cam_scan && now == cam_scan) {
        const auto& iv = s_frame.camera.inverse_view;
        const float eye[3] = {iv[3], iv[7], iv[11]};
        if (FILE* out = std::fopen("cam_scan.txt", "w")) {
            std::fprintf(out, "eye %.4f %.4f %.4f proj %.5f %.5f near %.3f far %.1f\n", eye[0], eye[1], eye[2],
                         s_frame.camera.projection[0], s_frame.camera.projection[1], s_frame.camera.projection[2],
                         s_frame.camera.projection[3]);
            auto rd = [&](uint32_t a) {
                uint32_t v;
                std::memcpy(&v, memory + a, 4);
                v = rbrew_bswap32(v);
                float f;
                std::memcpy(&f, &v, 4);
                return f;
            };
            unsigned hits = 0;
            for (uint32_t a = 0x28000000u; a + 12 < 0x68000000u && hits < 60; a += 4) {
                if (!rbrew_memory_accessible(memory, a, 12)) {
                    a = (a | 0xFFFFu);
                    continue;
                }
                if (!(std::abs(rd(a) - eye[0]) <= 0.02f) || !(std::abs(rd(a + 4) - eye[1]) <= 0.02f) ||
                    !(std::abs(rd(a + 8) - eye[2]) <= 0.02f))
                    continue;
                if (std::abs(rd(a) - eye[0]) > 0.02f || std::abs(rd(a + 4) - eye[1]) > 0.02f ||
                    std::abs(rd(a + 8) - eye[2]) > 0.02f)
                    continue;
                ++hits;
                std::fprintf(out, "hit %08X\n", a);
                for (uint32_t q = a - 320; q < a + 160; q += 16) {
                    std::fprintf(out, "  %08X:", q);
                    for (int k = 0; k < 4; ++k) std::fprintf(out, " %12.5f", rd(q + 4 * k));
                    std::fputc('\n', out);
                }
            }
            std::fprintf(out, "hits %u\n", hits);
            // sead::PerspectiveProjection: near, far, fovy (radians), sin, cos, tan, aspect.
            for (uint32_t a = 0x28000000u; a + 32 < 0x68000000u; a += 4) {
                if (!rbrew_memory_accessible(memory, a, 32)) {
                    a = (a | 0xFFFFu);
                    continue;
                }
                if (rd(a) != s_frame.camera.projection[2] ||
                    !(std::abs(rd(a + 4) - s_frame.camera.projection[3]) < 1.f))
                    continue;
                std::fprintf(out, "proj %08X:", a);
                for (int k = -4; k < 12; ++k) std::fprintf(out, " %.5f", rd(a + 4 * k));
                std::fputc('\n', out);
            }
            std::fclose(out);
        }
    }
    if (std::getenv("RBREW_WEATHER_LOG") && now % 120 == 0) {
        const auto& iv = s_frame.camera.inverse_view;
        RBREW_LOG_DEBUG("weather", "frame %u forward %.3f %.3f %.3f", now, -iv[2], -iv[6], -iv[10]);
    }
    // Cinema mode: the HUD draws that follow the final scene pass are skipped.
    if (applied && settings.cinema_mode) s_hud_hidden = true;
}

void shutdown() {
    const auto device = gx2vk::device();
    s_cover_loading = {};
    s_cover_started = false;
    for (auto* image : {&s_t_lut,
                        &s_ms_lut,
                        &s_sv_lut,
                        &s_shape,
                        &s_detail,
                        &s_dome,
                        &s_dummy2d,
                        &s_dummy3d,
                        &s_dummy_storage2d,
                        &s_dummy_storage3d,
                        &s_scene_copy,
                        &s_result,
                        &s_rays,
                        &s_cover,
                        &s_ao,
                        &s_bloom[0],
                        &s_bloom[1],
                        &s_bloom[2],
                        &s_bloom[3],
                        &s_bloom[4],
                        &s_bloom_up[0],
                        &s_bloom_up[1],
                        &s_bloom_up[2],
                        &s_bloom_up[3],
                        &s_history,
                        &s_next_history,
                        &s_taa_out,
                        &s_hist,
                        &s_curve})
        gx2vk::destroy_image(*image);
    for (const auto& [key, pipeline] : s_sky_pipelines)
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
    for (const auto& [format, pipeline] : s_rain_pipelines)
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
    s_sky_pipelines.clear();
    s_rain_pipelines.clear();
    for (VkPipeline* pipeline : {&s_transmittance, &s_multiscatter, &s_skyview, &s_noise, &s_dome_pass, &s_rays_pass,
                                 &s_composite, &s_ao_pass, &s_bloom_pass, &s_taa_pass, &s_hist_pass, &s_curve_pass}) {
        if (*pipeline) vkDestroyPipeline(device, *pipeline, nullptr);
        *pipeline = VK_NULL_HANDLE;
    }
    if (s_pool) vkDestroyDescriptorPool(device, s_pool, nullptr);
    if (s_layout) vkDestroyPipelineLayout(device, s_layout, nullptr);
    if (s_set_layout) vkDestroyDescriptorSetLayout(device, s_set_layout, nullptr);
    s_pool = VK_NULL_HANDLE;
    s_pool_sets = 0;
    s_layout = VK_NULL_HANDLE;
    s_set_layout = VK_NULL_HANDLE;
    s_cover_mapping = {};
    s_failed = false;
    s_static_ready = s_skyview_ready = false;
    s_dome_quality = 0;
    s_dome_frames = 0;
}
} // namespace gx2weather

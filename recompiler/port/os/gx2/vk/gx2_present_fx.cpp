// gx2_present_fx.cpp - Vulkan compute post-processing for FXAA, SMAA, FSR 1.0, and Anime4K.
#include "gx2_present_fx.h"
#include "gx2_present_fx_shaders.h"
#include "gx2_vk_shader.h"
#include "../../../runtime/port_settings.h"
#include "rebrewu/logger.hpp"

#include <algorithm>
#include <array>

namespace gx2_present_fx {
namespace {

struct PushConstants {
    float src_size[4];
    float dst_size[4];
    float params[4];
};
static_assert(sizeof(PushConstants) == 48);

enum PipelineId {
    kPipeFxaa,
    kPipeSmaaEdge,
    kPipeSmaaWeight,
    kPipeSmaaBlend,
    kPipeFsrEasu,
    kPipeFsrRcas,
    kPipeAnime4kUpsample,
    kPipeAnime4kThin,
    kPipeAnime4kRefine,
    kPipeCount
};

constexpr const char* kShaderSources[kPipeCount] = {
    gx2_present_fx_shaders::kFxaaShader,
    gx2_present_fx_shaders::kSmaaEdgeShader,
    gx2_present_fx_shaders::kSmaaWeightShader,
    gx2_present_fx_shaders::kSmaaBlendShader,
    gx2_present_fx_shaders::kFsrEasuShader,
    gx2_present_fx_shaders::kFsrRcasShader,
    gx2_present_fx_shaders::kAnime4kUpsampleShader,
    gx2_present_fx_shaders::kAnime4kThinShader,
    gx2_present_fx_shaders::kAnime4kRefineShader,
};

struct State {
    bool layout_failed = false;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    std::array<VkPipeline, kPipeCount> pipelines{};
    VkDescriptorPool pool = VK_NULL_HANDLE;
    uint32_t pool_sets = 0;

    gx2vk::Image staging;
    gx2vk::Image aa_a;
    gx2vk::Image aa_b;
    gx2vk::Image up_a;
    gx2vk::Image up_b;
    gx2vk::Image dummy;
};

State s_fx;

constexpr VkImageUsageFlags kFxUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

bool ensure_image(gx2vk::Image& img, uint32_t w, uint32_t h) {
    if (img.image && img.desc.width == w && img.desc.height == h && img.desc.format == VK_FORMAT_R8G8B8A8_UNORM &&
        (img.desc.usage & (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) ==
            (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) {
        return true;
    }
    gx2vk::destroy_image(img);
    gx2vk::ImageDesc desc{};
    desc.type = VK_IMAGE_TYPE_2D;
    desc.format = VK_FORMAT_R8G8B8A8_UNORM;
    desc.width = w;
    desc.height = h;
    desc.usage = kFxUsage;
    return gx2vk::create_image(img, desc);
}

bool ensure_layouts() {
    if (s_fx.pipeline_layout) return true;
    if (s_fx.layout_failed) return false;
    const VkDevice dev = gx2vk::device();
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl.bindingCount = 3;
    dsl.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(dev, &dsl, nullptr, &s_fx.set_layout) != VK_SUCCESS) {
        s_fx.layout_failed = true;
        RBREW_LOG_ERROR("present_fx", "failed to create descriptor set layout\n");
        return false;
    }
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.offset = 0;
    push.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &s_fx.set_layout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(dev, &pl, nullptr, &s_fx.pipeline_layout) != VK_SUCCESS) {
        s_fx.layout_failed = true;
        RBREW_LOG_ERROR("present_fx", "failed to create pipeline layout\n");
        return false;
    }
    if (!ensure_image(s_fx.dummy, 1, 1)) {
        s_fx.layout_failed = true;
        return false;
    }
    return true;
}

VkPipeline pipeline_for(PipelineId id) {
    if (!ensure_layouts()) return VK_NULL_HANDLE;
    if (s_fx.pipelines[id]) return s_fx.pipelines[id];
    const VkShaderModule mod = gx2vk::shader_module(VK_SHADER_STAGE_COMPUTE_BIT, kShaderSources[id]);
    if (!mod) {
        RBREW_LOG_ERROR("present_fx", "failed to compile compute shader %d\n", int(id));
        return VK_NULL_HANDLE;
    }
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = mod;
    ci.stage.pName = "main";
    ci.layout = s_fx.pipeline_layout;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(gx2vk::device(), gx2vk::pipeline_cache(), 1, &ci, nullptr, &pipe) != VK_SUCCESS) {
        RBREW_LOG_ERROR("present_fx", "failed to create compute pipeline %d\n", int(id));
        return VK_NULL_HANDLE;
    }
    s_fx.pipelines[id] = pipe;
    return pipe;
}

VkDescriptorSet allocate_set() {
    const VkDevice dev = gx2vk::device();
    if (s_fx.pool_sets == 64) {
        const VkDescriptorPool retired = s_fx.pool;
        gx2vk::defer([dev, retired] { vkDestroyDescriptorPool(dev, retired, nullptr); });
        s_fx.pool = VK_NULL_HANDLE;
        s_fx.pool_sets = 0;
    }
    if (!s_fx.pool) {
        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 * 2},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64},
        };
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = 64;
        ci.poolSizeCount = 2;
        ci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(dev, &ci, nullptr, &s_fx.pool) != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = s_fx.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &s_fx.set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(dev, &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    ++s_fx.pool_sets;
    return set;
}

VkSampler clamp_sampler(VkFilter filter) {
    VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    s.magFilter = s.minFilter = filter;
    s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return gx2vk::sampler(s);
}

VkImageView view2d(gx2vk::Image& img) {
    gx2vk::ViewDesc v{};
    v.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    return gx2vk::image_view(img, v);
}

bool dispatch_pass(PipelineId pipe_id, gx2vk::Image& in0, gx2vk::Image* in1, gx2vk::Image& out, uint32_t src_w,
                   uint32_t src_h, uint32_t dst_w, uint32_t dst_h, float sharpness, bool linear0 = true) {
    const VkPipeline pipe = pipeline_for(pipe_id);
    if (!pipe) return false;
    const VkDescriptorSet set = allocate_set();
    if (!set) return false;
    gx2vk::Image& tex1_img = in1 ? *in1 : s_fx.dummy;
    const VkDescriptorImageInfo infos[3] = {
        {clamp_sampler(linear0 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST), view2d(in0), VK_IMAGE_LAYOUT_GENERAL},
        {clamp_sampler(VK_FILTER_LINEAR), view2d(tex1_img), VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, view2d(out), VK_IMAGE_LAYOUT_GENERAL},
    };
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType =
            (i < 2) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(gx2vk::device(), 3, writes, 0, nullptr);

    const PushConstants pc{
        {float(src_w), float(src_h), 1.0f / float(std::max(1u, src_w)), 1.0f / float(std::max(1u, src_h))},
        {float(dst_w), float(dst_h), 1.0f / float(std::max(1u, dst_w)), 1.0f / float(std::max(1u, dst_h))},
        {sharpness, 0.0f, 0.0f, 0.0f},
    };
    const VkCommandBuffer cb = gx2vk::cmd();
    gx2vk::barrier_if_dirty();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, s_fx.pipeline_layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cb, s_fx.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cb, (dst_w + 7u) / 8u, (dst_h + 7u) / 8u, 1u);
    gx2vk::note_gpu_write(out);
    return true;
}

gx2vk::Image* sampled_source(const gx2vk::Image& source, uint32_t w, uint32_t h) {
    auto& mut = const_cast<gx2vk::Image&>(source);
    if ((source.desc.usage & VK_IMAGE_USAGE_SAMPLED_BIT) && source.desc.format == VK_FORMAT_R8G8B8A8_UNORM &&
        source.desc.width == w && source.desc.height == h) {
        return &mut;
    }
    if (!ensure_image(s_fx.staging, w, h)) return nullptr;
    const VkCommandBuffer cb = gx2vk::cmd();
    gx2vk::barrier_if_dirty();
    if (source.desc.format == VK_FORMAT_R8G8B8A8_UNORM) {
        VkImageCopy cp{};
        cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.extent = {w, h, 1};
        vkCmdCopyImage(cb, mut.image, VK_IMAGE_LAYOUT_GENERAL, s_fx.staging.image, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    } else {
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(w), int32_t(h), 1};
        blit.dstOffsets[1] = {int32_t(w), int32_t(h), 1};
        vkCmdBlitImage(cb, mut.image, VK_IMAGE_LAYOUT_GENERAL, s_fx.staging.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                       VK_FILTER_NEAREST);
    }
    gx2vk::note_gpu_write(s_fx.staging);
    return &s_fx.staging;
}

} // namespace

Output apply_explicit(const gx2vk::Image& source, uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                      int aa_mode, int upscaler, int sharpness_pct) {
    if (!gx2vk::ready() || !source.image || !src_w || !src_h || !dst_w || !dst_h) return {};
    if (aa_mode == rbrew_settings::kAAOff && upscaler == rbrew_settings::kUpscalerOff) return {};

    gx2vk::Image* cur = sampled_source(source, src_w, src_h);
    if (!cur) return {};
    uint32_t cur_w = src_w, cur_h = src_h;
    const float sharpness = std::clamp(float(sharpness_pct) / 100.0f, 0.0f, 1.0f);

    if (aa_mode == rbrew_settings::kAAFXAA) {
        if (!ensure_image(s_fx.aa_a, src_w, src_h) ||
            !dispatch_pass(kPipeFxaa, *cur, nullptr, s_fx.aa_a, src_w, src_h, src_w, src_h, sharpness)) {
            return {};
        }
        cur = &s_fx.aa_a;
    } else if (aa_mode == rbrew_settings::kAASMAA) {
        if (!ensure_image(s_fx.aa_a, src_w, src_h) || !ensure_image(s_fx.aa_b, src_w, src_h)) return {};
        if (!dispatch_pass(kPipeSmaaEdge, *cur, nullptr, s_fx.aa_a, src_w, src_h, src_w, src_h, sharpness)) return {};
        if (!dispatch_pass(kPipeSmaaWeight, s_fx.aa_a, nullptr, s_fx.aa_b, src_w, src_h, src_w, src_h, sharpness))
            return {};
        if (!dispatch_pass(kPipeSmaaBlend, *cur, &s_fx.aa_b, s_fx.aa_a, src_w, src_h, src_w, src_h, sharpness))
            return {};
        cur = &s_fx.aa_a;
    }

    if (upscaler == rbrew_settings::kUpscalerFSR) {
        if (!ensure_image(s_fx.up_a, dst_w, dst_h) || !ensure_image(s_fx.up_b, dst_w, dst_h)) return {};
        if (src_w == dst_w && src_h == dst_h) {
            if (!dispatch_pass(kPipeFsrRcas, *cur, nullptr, s_fx.up_a, cur_w, cur_h, dst_w, dst_h, sharpness))
                return {};
            cur = &s_fx.up_a;
        } else {
            if (!dispatch_pass(kPipeFsrEasu, *cur, nullptr, s_fx.up_a, cur_w, cur_h, dst_w, dst_h, sharpness))
                return {};
            if (!dispatch_pass(kPipeFsrRcas, s_fx.up_a, nullptr, s_fx.up_b, dst_w, dst_h, dst_w, dst_h, sharpness))
                return {};
            cur = &s_fx.up_b;
        }
        cur_w = dst_w;
        cur_h = dst_h;
    } else if (upscaler == rbrew_settings::kUpscalerAnime4K) {
        if (!ensure_image(s_fx.up_a, dst_w, dst_h) || !ensure_image(s_fx.up_b, dst_w, dst_h)) return {};
        if (src_w != dst_w || src_h != dst_h) {
            if (!dispatch_pass(kPipeAnime4kUpsample, *cur, nullptr, s_fx.up_a, cur_w, cur_h, dst_w, dst_h, sharpness))
                return {};
            if (!dispatch_pass(kPipeAnime4kThin, s_fx.up_a, nullptr, s_fx.up_b, dst_w, dst_h, dst_w, dst_h, sharpness))
                return {};
            if (!dispatch_pass(kPipeAnime4kRefine, s_fx.up_b, nullptr, s_fx.up_a, dst_w, dst_h, dst_w, dst_h,
                               sharpness))
                return {};
            cur = &s_fx.up_a;
        } else {
            if (!dispatch_pass(kPipeAnime4kThin, *cur, nullptr, s_fx.up_a, cur_w, cur_h, dst_w, dst_h, sharpness))
                return {};
            if (!dispatch_pass(kPipeAnime4kRefine, s_fx.up_a, nullptr, s_fx.up_b, dst_w, dst_h, dst_w, dst_h,
                               sharpness))
                return {};
            cur = &s_fx.up_b;
        }
        cur_w = dst_w;
        cur_h = dst_h;
    }

    return {cur, cur_w, cur_h};
}

Output apply(const gx2vk::Image& source, uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h) {
    int aa = rbrew_settings::g_aa_mode.load(std::memory_order_relaxed);
    const int shaders = rbrew_settings::g_shader_quality.load(std::memory_order_relaxed);
    const int upscaler = rbrew_settings::g_upscaler.load(std::memory_order_relaxed);
    const int sharpness = rbrew_settings::g_upscaler_sharpness.load(std::memory_order_relaxed);
    // Enhanced Shaders already run FXAA + TAA on the 3D scene before HUD;
    // keep present-time AA for Original mode or when SMAA is selected.
    if (aa == rbrew_settings::kAAFXAA && shaders != rbrew_settings::kShadersOff) aa = rbrew_settings::kAAOff;
    return apply_explicit(source, src_w, src_h, dst_w, dst_h, aa, upscaler, sharpness);
}

void shutdown() {
    if (!gx2vk::ready()) {
        s_fx = {};
        return;
    }
    const VkDevice dev = gx2vk::device();
    for (VkPipeline& p : s_fx.pipelines) {
        if (p) vkDestroyPipeline(dev, p, nullptr);
        p = VK_NULL_HANDLE;
    }
    if (s_fx.pipeline_layout) vkDestroyPipelineLayout(dev, s_fx.pipeline_layout, nullptr);
    if (s_fx.set_layout) vkDestroyDescriptorSetLayout(dev, s_fx.set_layout, nullptr);
    if (s_fx.pool) vkDestroyDescriptorPool(dev, s_fx.pool, nullptr);
    gx2vk::destroy_image(s_fx.staging);
    gx2vk::destroy_image(s_fx.aa_a);
    gx2vk::destroy_image(s_fx.aa_b);
    gx2vk::destroy_image(s_fx.up_a);
    gx2vk::destroy_image(s_fx.up_b);
    gx2vk::destroy_image(s_fx.dummy);
    s_fx = {};
}

} // namespace gx2_present_fx

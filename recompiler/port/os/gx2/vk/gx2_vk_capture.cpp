#include "gx2_vk_capture.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <vector>

namespace gx2vk::capture {
namespace {
// Previous functions in the chain (the render-split wrappers or the driver).
struct Prev {
    PFN_vkCmdBindPipeline BindPipeline;
    PFN_vkCmdBindDescriptorSets BindDescriptorSets;
    PFN_vkCmdBindIndexBuffer BindIndexBuffer;
    PFN_vkCmdBindVertexBuffers BindVertexBuffers;
    PFN_vkCmdPushDescriptorSetKHR PushDescriptorSetKHR;
    PFN_vkCmdPushConstants PushConstants;
    PFN_vkCmdSetViewport SetViewport;
    PFN_vkCmdSetScissor SetScissor;
    PFN_vkCmdSetCullMode SetCullMode;
    PFN_vkCmdSetFrontFace SetFrontFace;
    PFN_vkCmdSetDepthTestEnable SetDepthTestEnable;
    PFN_vkCmdSetDepthWriteEnable SetDepthWriteEnable;
    PFN_vkCmdSetDepthCompareOp SetDepthCompareOp;
    PFN_vkCmdSetStencilTestEnable SetStencilTestEnable;
    PFN_vkCmdSetStencilOp SetStencilOp;
    PFN_vkCmdSetStencilCompareMask SetStencilCompareMask;
    PFN_vkCmdSetStencilWriteMask SetStencilWriteMask;
    PFN_vkCmdSetStencilReference SetStencilReference;
    PFN_vkCmdSetDepthBias SetDepthBias;
    PFN_vkCmdSetDepthBiasEnable SetDepthBiasEnable;
    PFN_vkCmdSetPrimitiveRestartEnable SetPrimitiveRestartEnable;
    PFN_vkCmdSetBlendConstants SetBlendConstants;
    PFN_vkCmdDraw Draw;
    PFN_vkCmdDrawIndexed DrawIndexed;
    PFN_vkCmdBeginRendering BeginRendering;
    PFN_vkCmdEndRendering EndRendering;
    PFN_vkCmdPipelineBarrier2 PipelineBarrier2;
    PFN_vkCmdPipelineBarrier PipelineBarrier;
    PFN_vkCmdClearAttachments ClearAttachments;
    PFN_vkCmdCopyImage CopyImage;
    PFN_vkCmdBlitImage BlitImage;
    PFN_vkCmdClearColorImage ClearColorImage;
    PFN_vkCmdClearDepthStencilImage ClearDepthStencilImage;
    PFN_vkCmdCopyBufferToImage CopyBufferToImage;
    PFN_vkCmdCopyImageToBuffer CopyImageToBuffer;
    PFN_vkCmdBeginTransformFeedbackEXT BeginTransformFeedbackEXT;
    PFN_vkCmdEndTransformFeedbackEXT EndTransformFeedbackEXT;
} prev{};
bool s_installed = false, s_enabled = false, s_replaying = false, s_tf_active = false;
// Only draws of the scene passes get blended uniforms (targets: see replay()).
// Every other pass (the ink map, shadows, effects, HUD) is replayed with
// frame N's data: a blended paint draw painted the ink map for good and
// turned the whole level blue. In the plaza the shadow maps (square) and the
// GamePad image have more draws than either scene target.
struct PassInfo {
    uint64_t target;
    uint32_t draws;
    bool screen;
    uint32_t w = 0, h = 0;
    bool color = false, depth = false;
};
uint32_t s_unit = UINT32_MAX;
Substitute s_substitute = nullptr;

// Packets: {header, body}; body = a trivially copyable functor taking the
// command buffer, followed by the arrays it points into. Blocks never move,
// so the pointers stay valid until the frame is discarded.
using Thunk = void (*)(uint8_t* body, VkCommandBuffer cb);
struct Header {
    uint32_t size;
    uint32_t pad;
    Thunk fn;
};
constexpr uint32_t kAttachmentWrite = 1u << 31;
constexpr size_t kBlock = size_t(4) << 20;
constexpr size_t align16(size_t n) {
    return (n + 15) & ~size_t(15);
}
// Pass (index + 1) the packets being captured belong to; 0 outside passes.
uint32_t s_pass_tag = 0;
struct Arena {
    std::vector<std::unique_ptr<uint8_t[]>> blocks;
    size_t block = 0, used = 0, commands = 0, set_binds = 0;
    bool complete = true;
    struct Spot {
        size_t block, offset;
    };
    std::vector<Spot> packets;
    std::vector<PassInfo> passes; // per BeginRendering
    void clear() {
        block = 0;
        used = 0;
        commands = 0;
        set_binds = 0;
        complete = true;
        packets.clear();
        passes.clear();
    }
    uint8_t* reserve(size_t body, Thunk fn, bool attachment_write) {
        const size_t size = sizeof(Header) + align16(body);
        if (size > kBlock) {
            complete = false;
            return nullptr;
        }
        if (blocks.empty()) blocks.emplace_back(new uint8_t[kBlock]);
        if (used + size > kBlock) {
            ++block;
            used = 0;
            if (block == blocks.size()) blocks.emplace_back(new uint8_t[kBlock]);
        }
        uint8_t* p = blocks[block].get() + used;
        Header h{uint32_t(size), s_pass_tag | (attachment_write ? kAttachmentWrite : 0), fn};
        std::memcpy(p, &h, sizeof(h));
        packets.push_back({block, used});
        used += size;
        ++commands;
        return p + sizeof(Header);
    }
};
Arena s_arenas[2];
Arena* s_capture = &s_arenas[0];
Arena* s_replay = &s_arenas[1];

// Initialization barriers belong to the prologue command buffer and run
// before the main commands. Replaying them inline can place image barriers
// inside a rendering instance and reset images that already contain data.
bool capturing(VkCommandBuffer cb) {
    return s_enabled && !s_replaying && gx2vk::is_current_command_buffer(cb);
}
std::vector<uint64_t> s_blend_targets; // replay: the scene targets
std::atomic<bool> s_plaza{false};
bool blend_target(uint64_t target) {
    return target && std::find(s_blend_targets.begin(), s_blend_targets.end(), target) != s_blend_targets.end();
}
// Replay: the pass a captured push belongs to may blend.
bool blend_pass(uint32_t pass) {
    return pass < s_replay->passes.size() && blend_target(s_replay->passes[pass].target);
}
template <class F> void thunk(uint8_t* body, VkCommandBuffer cb) {
    (*reinterpret_cast<F*>(body))(cb);
}
template <class Make> void cap(size_t extra, Make make, bool attachment_write = false) {
    using F = decltype(make(static_cast<uint8_t*>(nullptr)));
    static_assert(std::is_trivially_copyable_v<F>);
    uint8_t* body = s_capture->reserve(align16(sizeof(F)) + extra, &thunk<F>, attachment_write);
    if (!body) return;
    new (body) F(make(body + align16(sizeof(F))));
}
template <class T> const T* put(uint8_t*& cursor, const T* src, uint32_t count) {
    if (!src || !count) return nullptr;
    T* out = reinterpret_cast<T*>(cursor);
    std::memcpy(out, src, sizeof(T) * count);
    cursor += align16(sizeof(T) * count);
    return out;
}
template <class T> constexpr size_t bytes(const T*, uint32_t count) {
    return align16(sizeof(T) * count);
}

#define CAP0(Name, Params, Args, Call)                                                                                 \
    VKAPI_ATTR void VKAPI_CALL c_##Name Params {                                                                       \
        if (capturing(cb)) cap(0, [=](uint8_t*) { return [=](VkCommandBuffer c) { vkCmd##Name Call; }; });             \
        prev.Name Args;                                                                                                \
    }
CAP0(BindPipeline, (VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p), (cb, bp, p), (c, bp, p))
CAP0(BindIndexBuffer, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t), (cb, b, o, t), (c, b, o, t))
CAP0(SetCullMode, (VkCommandBuffer cb, VkCullModeFlags m), (cb, m), (c, m))
CAP0(SetFrontFace, (VkCommandBuffer cb, VkFrontFace f), (cb, f), (c, f))
CAP0(SetDepthTestEnable, (VkCommandBuffer cb, VkBool32 e), (cb, e), (c, e))
CAP0(SetDepthWriteEnable, (VkCommandBuffer cb, VkBool32 e), (cb, e), (c, e))
CAP0(SetDepthCompareOp, (VkCommandBuffer cb, VkCompareOp o), (cb, o), (c, o))
CAP0(SetStencilTestEnable, (VkCommandBuffer cb, VkBool32 e), (cb, e), (c, e))
CAP0(SetStencilOp,
     (VkCommandBuffer cb, VkStencilFaceFlags f, VkStencilOp a, VkStencilOp b, VkStencilOp d, VkCompareOp o),
     (cb, f, a, b, d, o), (c, f, a, b, d, o))
CAP0(SetStencilCompareMask, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m), (cb, f, m), (c, f, m))
CAP0(SetStencilWriteMask, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m), (cb, f, m), (c, f, m))
CAP0(SetStencilReference, (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v), (cb, f, v), (c, f, v))
CAP0(SetDepthBias, (VkCommandBuffer cb, float a, float b, float d), (cb, a, b, d), (c, a, b, d))
CAP0(SetDepthBiasEnable, (VkCommandBuffer cb, VkBool32 e), (cb, e), (c, e))
CAP0(SetPrimitiveRestartEnable, (VkCommandBuffer cb, VkBool32 e), (cb, e), (c, e))
#undef CAP0
VKAPI_ATTR void VKAPI_CALL c_EndRendering(VkCommandBuffer cb) {
    if (capturing(cb)) cap(0, [=](uint8_t*) { return [=](VkCommandBuffer c) { vkCmdEndRendering(c); }; });
    s_pass_tag = 0;
    prev.EndRendering(cb);
}
// Draws inside a transform-feedback section produce guest data: not replayed.
VKAPI_ATTR void VKAPI_CALL c_Draw(VkCommandBuffer cb, uint32_t v, uint32_t i, uint32_t fv, uint32_t fi) {
    if (capturing(cb) && !s_tf_active) {
        if (!s_capture->passes.empty()) ++s_capture->passes.back().draws;
        cap(0, [=](uint8_t*) { return [=](VkCommandBuffer c) { vkCmdDraw(c, v, i, fv, fi); }; }, true);
    }
    prev.Draw(cb, v, i, fv, fi);
}
VKAPI_ATTR void VKAPI_CALL c_DrawIndexed(VkCommandBuffer cb, uint32_t n, uint32_t i, uint32_t fi, int32_t vo,
                                         uint32_t fin) {
    if (capturing(cb) && !s_tf_active) {
        if (!s_capture->passes.empty()) ++s_capture->passes.back().draws;
        cap(0, [=](uint8_t*) { return [=](VkCommandBuffer c) { vkCmdDrawIndexed(c, n, i, fi, vo, fin); }; }, true);
    }
    prev.DrawIndexed(cb, n, i, fi, vo, fin);
}
VKAPI_ATTR void VKAPI_CALL c_BeginTransformFeedbackEXT(VkCommandBuffer cb, uint32_t f, uint32_t n, const VkBuffer* b,
                                                       const VkDeviceSize* o) {
    if (!s_replaying) s_tf_active = true;
    prev.BeginTransformFeedbackEXT(cb, f, n, b, o);
}
VKAPI_ATTR void VKAPI_CALL c_EndTransformFeedbackEXT(VkCommandBuffer cb, uint32_t f, uint32_t n, const VkBuffer* b,
                                                     const VkDeviceSize* o) {
    if (!s_replaying) s_tf_active = false;
    prev.EndTransformFeedbackEXT(cb, f, n, b, o);
}
VKAPI_ATTR void VKAPI_CALL c_SetBlendConstants(VkCommandBuffer cb, const float k[4]) {
    if (capturing(cb)) {
        struct V {
            float v[4];
        } values;
        std::memcpy(values.v, k, 16);
        cap(0, [=](uint8_t*) { return [=](VkCommandBuffer c) { vkCmdSetBlendConstants(c, values.v); }; });
    }
    prev.SetBlendConstants(cb, k);
}
VKAPI_ATTR void VKAPI_CALL c_SetViewport(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkViewport* v) {
    if (capturing(cb))
        cap(bytes(v, count), [=](uint8_t* p) {
            const VkViewport* x = put(p, v, count);
            return [=](VkCommandBuffer c) { vkCmdSetViewport(c, first, count, x); };
        });
    prev.SetViewport(cb, first, count, v);
}
VKAPI_ATTR void VKAPI_CALL c_SetScissor(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkRect2D* s) {
    if (capturing(cb))
        cap(bytes(s, count), [=](uint8_t* p) {
            const VkRect2D* x = put(p, s, count);
            return [=](VkCommandBuffer c) { vkCmdSetScissor(c, first, count, x); };
        });
    prev.SetScissor(cb, first, count, s);
}
VKAPI_ATTR void VKAPI_CALL c_BindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkBuffer* b,
                                               const VkDeviceSize* o) {
    if (capturing(cb))
        cap(bytes(b, count) + bytes(o, count), [=](uint8_t* p) {
            const VkBuffer* bx = put(p, b, count);
            const VkDeviceSize* ox = put(p, o, count);
            return [=](VkCommandBuffer c) { vkCmdBindVertexBuffers(c, first, count, bx, ox); };
        });
    prev.BindVertexBuffers(cb, first, count, b, o);
}
VKAPI_ATTR void VKAPI_CALL c_BindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l,
                                                uint32_t first, uint32_t count, const VkDescriptorSet* sets,
                                                uint32_t dyn_count, const uint32_t* dyn) {
    if (capturing(cb)) ++s_capture->set_binds;
    if (capturing(cb))
        cap(bytes(sets, count) + bytes(dyn, dyn_count), [=](uint8_t* p) {
            const VkDescriptorSet* sx = put(p, sets, count);
            const uint32_t* dx = put(p, dyn, dyn_count);
            return [=](VkCommandBuffer c) { vkCmdBindDescriptorSets(c, bp, l, first, count, sx, dyn_count, dx); };
        });
    prev.BindDescriptorSets(cb, bp, l, first, count, sets, dyn_count, dyn);
}
VKAPI_ATTR void VKAPI_CALL c_PushConstants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags s,
                                           uint32_t offset, uint32_t size, const void* v) {
    if (capturing(cb))
        cap(align16(size), [=](uint8_t* p) {
            const uint8_t* vx = put(p, static_cast<const uint8_t*>(v), size);
            return [=](VkCommandBuffer c) { vkCmdPushConstants(c, l, s, offset, size, vx); };
        });
    prev.PushConstants(cb, l, s, offset, size, v);
}
VKAPI_ATTR void VKAPI_CALL c_ClearAttachments(VkCommandBuffer cb, uint32_t n, const VkClearAttachment* a, uint32_t rn,
                                              const VkClearRect* r) {
    if (capturing(cb))
        cap(
            bytes(a, n) + bytes(r, rn),
            [=](uint8_t* p) {
                const VkClearAttachment* ax = put(p, a, n);
                const VkClearRect* rx = put(p, r, rn);
                return [=](VkCommandBuffer c) { vkCmdClearAttachments(c, n, ax, rn, rx); };
            },
            true);
    prev.ClearAttachments(cb, n, a, rn, r);
}
bool buffer_type(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
           t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}
bool image_type(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_SAMPLER || t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
           t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
           t == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}
// Replay of a push-descriptor write: its uniform buffers are substituted
// with the in-between frame's data of the draw unit that bound them.
void replay_push(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t set, uint32_t n,
                 const VkWriteDescriptorSet* writes, uint32_t unit) {
    VkWriteDescriptorSet local[64];
    VkDescriptorBufferInfo infos[64];
    if (!s_substitute || unit == UINT32_MAX || n > 64) {
        vkCmdPushDescriptorSetKHR(c, bp, l, set, n, writes);
        return;
    }
    uint32_t used = 0;
    for (uint32_t i = 0; i < n; ++i) {
        local[i] = writes[i];
        if (writes[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && writes[i].descriptorCount == 1 &&
            writes[i].pBufferInfo && used < 64) {
            VkDescriptorBufferInfo info = *writes[i].pBufferInfo;
            if (s_substitute(unit, info.buffer, info.offset, info.range, info.buffer, info.offset)) {
                infos[used] = info;
                local[i].pBufferInfo = &infos[used++];
            }
        }
    }
    vkCmdPushDescriptorSetKHR(c, bp, l, set, n, local);
}
VKAPI_ATTR void VKAPI_CALL c_PushDescriptorSet(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l,
                                               uint32_t set, uint32_t n, const VkWriteDescriptorSet* writes) {
    if (capturing(cb)) {
        bool simple = true;
        size_t extra = bytes(writes, n);
        for (uint32_t i = 0; i < n; ++i) {
            const auto& w = writes[i];
            if (w.pNext) {
                simple = false;
                break;
            }
            if (image_type(w.descriptorType))
                extra += bytes(w.pImageInfo, w.descriptorCount);
            else if (buffer_type(w.descriptorType))
                extra += bytes(w.pBufferInfo, w.descriptorCount);
            else {
                simple = false;
                break;
            }
        }
        if (!simple)
            s_capture->complete = false;
        else {
            const uint32_t unit = s_unit;
            const uint32_t pass = s_capture->passes.empty() ? UINT32_MAX : uint32_t(s_capture->passes.size() - 1);
            cap(extra, [=](uint8_t* p) {
                VkWriteDescriptorSet* wx = const_cast<VkWriteDescriptorSet*>(put(p, writes, n));
                for (uint32_t i = 0; i < n; ++i) {
                    auto& w = wx[i];
                    if (image_type(w.descriptorType)) {
                        w.pImageInfo = put(p, w.pImageInfo, w.descriptorCount);
                        w.pBufferInfo = nullptr;
                    } else {
                        w.pBufferInfo = put(p, w.pBufferInfo, w.descriptorCount);
                        w.pImageInfo = nullptr;
                    }
                    w.pTexelBufferView = nullptr;
                }
                return
                    [=](VkCommandBuffer c) { replay_push(c, bp, l, set, n, wx, blend_pass(pass) ? unit : UINT32_MAX); };
            });
        }
    }
    prev.PushDescriptorSetKHR(cb, bp, l, set, n, writes);
}
VKAPI_ATTR void VKAPI_CALL c_BeginRendering(VkCommandBuffer cb, const VkRenderingInfo* info) {
    if (capturing(cb)) {
        VkImageView target = info->colorAttachmentCount && info->pColorAttachments
                                 ? info->pColorAttachments[0].imageView
                                 : VK_NULL_HANDLE;
        if (!target && info->pDepthAttachment) target = info->pDepthAttachment->imageView;
        const uint64_t w = info->renderArea.extent.width, h = info->renderArea.extent.height;
        const bool screen = w >= 320 && h && (w * 9 > h * 16 ? w * 9 - h * 16 : h * 16 - w * 9) * 32 <= w * 9;
        s_pass_tag = uint32_t(s_capture->passes.size() + 1);
        s_capture->passes.push_back({uint64_t(reinterpret_cast<uintptr_t>(target)), 0, screen, uint32_t(w), uint32_t(h),
                                     info->colorAttachmentCount > 0,
                                     info->pDepthAttachment && info->pDepthAttachment->imageView != VK_NULL_HANDLE});
        const bool simple = !info->pNext &&
                            std::all_of(info->pColorAttachments, info->pColorAttachments + info->colorAttachmentCount,
                                        [](const VkRenderingAttachmentInfo& a) { return !a.pNext; }) &&
                            (!info->pDepthAttachment || !info->pDepthAttachment->pNext) &&
                            (!info->pStencilAttachment || !info->pStencilAttachment->pNext);
        if (!simple)
            s_capture->complete = false;
        else
            cap(bytes(info, 1) + bytes(info->pColorAttachments, info->colorAttachmentCount) +
                    bytes(info->pDepthAttachment, 1) + bytes(info->pStencilAttachment, 1),
                [=](uint8_t* p) {
                    VkRenderingInfo* ix = const_cast<VkRenderingInfo*>(put(p, info, 1));
                    ix->pColorAttachments = put(p, info->pColorAttachments, info->colorAttachmentCount);
                    ix->pDepthAttachment = put(p, info->pDepthAttachment, info->pDepthAttachment ? 1u : 0u);
                    ix->pStencilAttachment = put(p, info->pStencilAttachment, info->pStencilAttachment ? 1u : 0u);
                    return [=](VkCommandBuffer c) { vkCmdBeginRendering(c, ix); };
                });
    }
    prev.BeginRendering(cb, info);
}
VKAPI_ATTR void VKAPI_CALL c_PipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo* d) {
    if (capturing(cb)) {
        const bool simple =
            !d->pNext &&
            std::all_of(d->pMemoryBarriers, d->pMemoryBarriers + d->memoryBarrierCount,
                        [](const VkMemoryBarrier2& b) { return !b.pNext; }) &&
            std::all_of(d->pBufferMemoryBarriers, d->pBufferMemoryBarriers + d->bufferMemoryBarrierCount,
                        [](const VkBufferMemoryBarrier2& b) { return !b.pNext; }) &&
            std::all_of(d->pImageMemoryBarriers, d->pImageMemoryBarriers + d->imageMemoryBarrierCount,
                        [](const VkImageMemoryBarrier2& b) { return !b.pNext; });
        if (!simple)
            s_capture->complete = false;
        else
            cap(bytes(d, 1) + bytes(d->pMemoryBarriers, d->memoryBarrierCount) +
                    bytes(d->pBufferMemoryBarriers, d->bufferMemoryBarrierCount) +
                    bytes(d->pImageMemoryBarriers, d->imageMemoryBarrierCount),
                [=](uint8_t* p) {
                    VkDependencyInfo* dx = const_cast<VkDependencyInfo*>(put(p, d, 1));
                    dx->pMemoryBarriers = put(p, d->pMemoryBarriers, d->memoryBarrierCount);
                    dx->pBufferMemoryBarriers = put(p, d->pBufferMemoryBarriers, d->bufferMemoryBarrierCount);
                    dx->pImageMemoryBarriers = put(p, d->pImageMemoryBarriers, d->imageMemoryBarrierCount);
                    return [=](VkCommandBuffer c) { vkCmdPipelineBarrier2(c, dx); };
                });
    }
    prev.PipelineBarrier2(cb, d);
}
VKAPI_ATTR void VKAPI_CALL c_PipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                                             VkDependencyFlags f, uint32_t mn, const VkMemoryBarrier* m, uint32_t bn,
                                             const VkBufferMemoryBarrier* b, uint32_t in,
                                             const VkImageMemoryBarrier* im) {
    if (capturing(cb))
        cap(bytes(m, mn) + bytes(b, bn) + bytes(im, in), [=](uint8_t* p) {
            const VkMemoryBarrier* mx = put(p, m, mn);
            const VkBufferMemoryBarrier* bx = put(p, b, bn);
            const VkImageMemoryBarrier* ix = put(p, im, in);
            return [=](VkCommandBuffer c) { vkCmdPipelineBarrier(c, src, dst, f, mn, mx, bn, bx, in, ix); };
        });
    prev.PipelineBarrier(cb, src, dst, f, mn, m, bn, b, in, im);
}
VKAPI_ATTR void VKAPI_CALL c_CopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl,
                                       uint32_t n, const VkImageCopy* r) {
    if (capturing(cb))
        cap(bytes(r, n), [=](uint8_t* p) {
            const VkImageCopy* rx = put(p, r, n);
            return [=](VkCommandBuffer c) { vkCmdCopyImage(c, s, sl, d, dl, n, rx); };
        });
    prev.CopyImage(cb, s, sl, d, dl, n, r);
}
VKAPI_ATTR void VKAPI_CALL c_BlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl,
                                       uint32_t n, const VkImageBlit* r, VkFilter f) {
    if (capturing(cb))
        cap(bytes(r, n), [=](uint8_t* p) {
            const VkImageBlit* rx = put(p, r, n);
            return [=](VkCommandBuffer c) { vkCmdBlitImage(c, s, sl, d, dl, n, rx, f); };
        });
    prev.BlitImage(cb, s, sl, d, dl, n, r, f);
}
VKAPI_ATTR void VKAPI_CALL c_ClearColorImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue* v,
                                             uint32_t n, const VkImageSubresourceRange* r) {
    if (capturing(cb))
        cap(bytes(v, 1) + bytes(r, n), [=](uint8_t* p) {
            const VkClearColorValue* vx = put(p, v, v ? 1u : 0u);
            const VkImageSubresourceRange* rx = put(p, r, n);
            return [=](VkCommandBuffer c) { vkCmdClearColorImage(c, i, l, vx, n, rx); };
        });
    prev.ClearColorImage(cb, i, l, v, n, r);
}
VKAPI_ATTR void VKAPI_CALL c_ClearDepthStencilImage(VkCommandBuffer cb, VkImage i, VkImageLayout l,
                                                    const VkClearDepthStencilValue* v, uint32_t n,
                                                    const VkImageSubresourceRange* r) {
    if (capturing(cb))
        cap(bytes(v, 1) + bytes(r, n), [=](uint8_t* p) {
            const VkClearDepthStencilValue* vx = put(p, v, v ? 1u : 0u);
            const VkImageSubresourceRange* rx = put(p, r, n);
            return [=](VkCommandBuffer c) { vkCmdClearDepthStencilImage(c, i, l, vx, n, rx); };
        });
    prev.ClearDepthStencilImage(cb, i, l, v, n, r);
}
// Image uploads and image <-> buffer round trips (a depth buffer copied into a
// color image through a staging buffer). Not captured, an in-between frame in
// an Octo Valley level showed a quarter of the screen from an older frame.
// Their buffers outlive the replay (retain_until_current_submission keeps the
// frame's upload chunks and deferred destructions). RBREW_INTERP_CAPTURE_COPIES=0:
// not captured (the 2026-10-26 behaviour).
bool capture_copies() {
    static const bool on = [] {
        const char* v = std::getenv("RBREW_INTERP_CAPTURE_COPIES");
        return !(v && v[0] == '0');
    }();
    return on;
}
VKAPI_ATTR void VKAPI_CALL c_CopyBufferToImage(VkCommandBuffer cb, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n,
                                               const VkBufferImageCopy* r) {
    if (capturing(cb) && capture_copies())
        cap(bytes(r, n), [=](uint8_t* p) {
            const VkBufferImageCopy* rx = put(p, r, n);
            return [=](VkCommandBuffer c) { vkCmdCopyBufferToImage(c, b, i, l, n, rx); };
        });
    prev.CopyBufferToImage(cb, b, i, l, n, r);
}
// Image -> buffer only into GPU-side staging buffers the renderer registered:
// a readback buffer the CPU reads later must never get in-between data.
std::vector<VkBuffer> s_staging_buffers;
VKAPI_ATTR void VKAPI_CALL c_CopyImageToBuffer(VkCommandBuffer cb, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n,
                                               const VkBufferImageCopy* r) {
    if (capturing(cb) && capture_copies() &&
        std::find(s_staging_buffers.begin(), s_staging_buffers.end(), b) != s_staging_buffers.end())
        cap(bytes(r, n), [=](uint8_t* p) {
            const VkBufferImageCopy* rx = put(p, r, n);
            return [=](VkCommandBuffer c) { vkCmdCopyImageToBuffer(c, i, l, b, n, rx); };
        });
    prev.CopyImageToBuffer(cb, i, l, b, n, r);
}

template <class P> void hook(P& volk_pointer, P& saved, P wrapper) {
    if (!volk_pointer || volk_pointer == wrapper) return;
    saved = volk_pointer;
    volk_pointer = wrapper;
}
} // namespace

void install() {
    if (s_installed) return;
    s_installed = true;
#define H(Name) hook(vkCmd##Name, prev.Name, &c_##Name)
    H(BindPipeline);
    H(BindDescriptorSets);
    H(BindIndexBuffer);
    H(BindVertexBuffers);
    H(PushConstants);
    hook(vkCmdPushDescriptorSetKHR, prev.PushDescriptorSetKHR, &c_PushDescriptorSet);
    H(SetViewport);
    H(SetScissor);
    H(SetCullMode);
    H(SetFrontFace);
    H(SetDepthTestEnable);
    H(SetDepthWriteEnable);
    H(SetDepthCompareOp);
    H(SetStencilTestEnable);
    H(SetStencilOp);
    H(SetStencilCompareMask);
    H(SetStencilWriteMask);
    H(SetStencilReference);
    H(SetDepthBias);
    H(SetDepthBiasEnable);
    H(SetPrimitiveRestartEnable);
    H(SetBlendConstants);
    H(Draw);
    H(DrawIndexed);
    H(BeginRendering);
    H(EndRendering);
    H(PipelineBarrier2);
    H(PipelineBarrier);
    H(ClearAttachments);
    H(CopyImage);
    H(BlitImage);
    H(ClearColorImage);
    H(ClearDepthStencilImage);
    H(BeginTransformFeedbackEXT);
    H(EndTransformFeedbackEXT);
    H(CopyBufferToImage);
    H(CopyImageToBuffer);
#undef H
}
void set_enabled(bool enabled) {
    s_enabled = enabled && s_installed;
}
bool enabled() {
    return s_enabled;
}
void set_unit(uint32_t unit) {
    s_unit = unit;
}
void set_plaza(bool plaza) {
    s_plaza.store(plaza, std::memory_order_relaxed);
}
void register_staging(VkBuffer buffer, VkBuffer replaced) {
    s_staging_buffers.erase(std::remove(s_staging_buffers.begin(), s_staging_buffers.end(), replaced),
                            s_staging_buffers.end());
    if (buffer) s_staging_buffers.push_back(buffer);
}
bool plaza() {
    return s_plaza.load(std::memory_order_relaxed);
}
void finish_frame() {
    std::swap(s_capture, s_replay);
    s_capture->clear();
    s_unit = UINT32_MAX;
}
void discard() {
    s_capture->clear();
    s_replay->clear();
}
size_t replay_command_count() {
    return s_replay->commands;
}
size_t replay_set_binds() {
    return s_replay->set_binds;
}
bool replay_complete() {
    return s_replay->complete && s_replay->commands;
}
void replay(Substitute substitute) {
    // Which targets blend. The 3D scene is drawn in two passes with depth (a
    // pre-pass and the main pass) at the largest screen-shaped size.
    // Default: outside the plaza only the target with the most draws (the
    // main pass, 2026-10-26 rule): blending the pre-pass too showed a huge
    // green blob in tutorial frames (flicker pixels 0.02% -> 0.19% median).
    // In the plaza the motion is in the pre-pass and the most draws go to the
    // square shadow maps and the 854x480 GamePad image: every full-size
    // target except the last (the output with the HUD) blends there.
    // RBREW_INTERP_BLEND_RULE=0/2: one rule everywhere (0: most draws,
    // 2: every full-size target but the last); =3: the full-size target with
    // the most draws (no motion in the plaza).
    static const int forced = [] {
        const char* v = std::getenv("RBREW_INTERP_BLEND_RULE");
        return v && v[0] >= '0' && v[0] <= '3' ? v[0] - '0' : -1;
    }();
    const int rule = forced >= 0 ? forced : 2;
    s_blend_targets.clear();
    if (rule == 3) {
        uint64_t area = 0, best = 0;
        for (const auto& pass : s_replay->passes)
            if (pass.target && pass.color && pass.screen && pass.draws)
                area = std::max(area, uint64_t(pass.w) * pass.h);
        std::vector<PassInfo> totals;
        for (const auto& pass : s_replay->passes) {
            if (!pass.target || !pass.color || !pass.screen || uint64_t(pass.w) * pass.h != area) continue;
            auto it =
                std::find_if(totals.begin(), totals.end(), [&](const PassInfo& t) { return t.target == pass.target; });
            if (it == totals.end())
                totals.push_back(pass);
            else
                it->draws += pass.draws;
        }
        uint32_t most = 0;
        for (const auto& t : totals)
            if (t.draws > most) {
                most = t.draws;
                best = t.target;
            }
        if (best) s_blend_targets.push_back(best);
    }
    if (rule == 2) {
        uint64_t area = 0, last = 0;
        for (const auto& pass : s_replay->passes)
            if (pass.target && pass.color && pass.screen && pass.draws)
                area = std::max(area, uint64_t(pass.w) * pass.h);
        for (const auto& pass : s_replay->passes)
            if (pass.target && pass.color && pass.screen && uint64_t(pass.w) * pass.h == area) last = pass.target;
        for (const auto& pass : s_replay->passes)
            if (pass.target && pass.color && pass.screen && uint64_t(pass.w) * pass.h == area && pass.target != last &&
                !blend_target(pass.target))
                s_blend_targets.push_back(pass.target);
    }
    if (s_blend_targets.empty()) {
        std::vector<PassInfo> totals;
        for (const auto& pass : s_replay->passes) {
            auto it =
                std::find_if(totals.begin(), totals.end(), [&](const PassInfo& t) { return t.target == pass.target; });
            if (it == totals.end())
                totals.push_back(pass);
            else
                it->draws += pass.draws;
        }
        uint64_t best = 0;
        uint32_t most = 0;
        for (const auto& t : totals)
            if (t.target && (t.screen || rule == 0) && t.draws > most) {
                most = t.draws;
                best = t.target;
            }
        if (!best)
            for (const auto& t : totals)
                if (t.target && t.draws > most) {
                    most = t.draws;
                    best = t.target;
                }
        if (best) s_blend_targets.push_back(best);
    }
    // RBREW_INTERP_PASS_LOG=<file> (diagnostics): the frame's passes every 300 replays.
    static FILE* pass_log = [] {
        const char* p = std::getenv("RBREW_INTERP_PASS_LOG");
        return p && *p ? std::fopen(p, "w") : nullptr;
    }();
    static unsigned replays = 0;
    if (pass_log && ++replays % 300 == 1) {
        std::fprintf(pass_log, "replay %u: %zu passes, %zu blend targets\n", replays, s_replay->passes.size(),
                     s_blend_targets.size());
        for (const auto& pass : s_replay->passes)
            std::fprintf(pass_log, "  %llx %u %ux%u%s%s%s%s\n", (unsigned long long)pass.target, pass.draws, pass.w,
                         pass.h, pass.color ? " color" : "", pass.depth ? "+depth" : "", pass.screen ? " screen" : "",
                         blend_target(pass.target) ? "  <- blend" : "");
        std::fflush(pass_log);
    }
    s_substitute = substitute;
    s_replaying = true;
    VkCommandBuffer cb = gx2vk::cmd();
    // Tiny passes (128x1, 1x1: the eye adaptation's luminance) blend the old
    // value with the new one. Run again per in-between frame they sped the
    // adaptation up, and with two in-between frames (180 FPS) the exposure ran
    // off and the second one came out nearly black. Their frame-N result is
    // still in place: skip attachment writes, but keep their state commands
    // and rendering boundaries. Later passes can inherit these bindings.
    // RBREW_INTERP_REPLAY_TINY=1: replay their writes as well.
    static const bool replay_tiny = [] {
        const char* v = std::getenv("RBREW_INTERP_REPLAY_TINY");
        return v && v[0] == '1';
    }();
    for (const auto& spot : s_replay->packets) {
        uint8_t* p = s_replay->blocks[spot.block].get() + spot.offset;
        Header h;
        std::memcpy(&h, p, sizeof(h));
        const uint32_t pass_index = h.pad & ~kAttachmentWrite;
        if (!replay_tiny && (h.pad & kAttachmentWrite) && pass_index && pass_index <= s_replay->passes.size()) {
            const PassInfo& pass = s_replay->passes[pass_index - 1];
            if (pass.color && uint64_t(pass.w) * pass.h <= 128) continue;
        }
        h.fn(p + sizeof(Header), cb);
    }
    s_replaying = false;
    s_substitute = nullptr;
}
} // namespace gx2vk::capture

#include "gx2_mod_capture.h"

#include "gx2_vk.h"
#include "../../../runtime/mod_loader.h"

#include <algorithm>

namespace gx2_mod_capture {
namespace {

constexpr int kSlotCount = 2;

struct CaptureSlot {
    gx2vk::Image scaled;
    gx2vk::Buffer staging;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint64_t serial = 0;
};

CaptureSlot s_slots[kSlotCount];
int s_next_slot = 0;

VkFormat stream_format(VkFormat source) {
    // Preserve sRGB encoding across blits so captures keep accurate brightness.
    return (source == VK_FORMAT_R8G8B8A8_SRGB || source == VK_FORMAT_B8G8R8A8_SRGB) ? VK_FORMAT_R8G8B8A8_SRGB
                                                                                    : VK_FORMAT_R8G8B8A8_UNORM;
}

void compute_auto_extent(uint32_t src_w, uint32_t src_h, uint32_t& out_w, uint32_t& out_h) {
    constexpr uint64_t kMaxW = 1920u;
    constexpr uint64_t kMaxH = 1080u;
    if (src_w < 2u || src_h < 2u) {
        out_w = 1280u;
        out_h = 720u;
        return;
    }
    uint64_t w = src_w;
    uint64_t h = src_h;
    if (w > kMaxW) {
        h = (h * kMaxW) / w;
        w = kMaxW;
    }
    if (h > kMaxH) {
        w = (w * kMaxH) / h;
        h = kMaxH;
    }
    w &= ~uint64_t(1);
    h &= ~uint64_t(1);
    out_w = std::max(2u, uint32_t(w));
    out_h = std::max(2u, uint32_t(h));
}

void release_slot(CaptureSlot& slot) {
    if (slot.staging.buffer) gx2vk::destroy_buffer(slot.staging);
    if (slot.scaled.image) gx2vk::destroy_image(slot.scaled);
    slot.staging = {};
    slot.scaled = {};
    slot.width = 0;
    slot.height = 0;
    slot.format = VK_FORMAT_UNDEFINED;
    slot.serial = 0;
}

bool ensure_slot(CaptureSlot& slot, uint32_t width, uint32_t height, VkFormat format) {
    if (slot.scaled.image && slot.staging.buffer && slot.width == width && slot.height == height &&
        slot.format == format) {
        return true;
    }
    release_slot(slot);

    gx2vk::ImageDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!gx2vk::create_image(slot.scaled, desc)) return false;

    const VkDeviceSize bytes = VkDeviceSize(width) * VkDeviceSize(height) * 4u;
    slot.staging = gx2vk::create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gx2vk::MemoryUse::Readback);
    if (!slot.staging.buffer || !slot.staging.memory.mapped) {
        release_slot(slot);
        return false;
    }

    slot.width = width;
    slot.height = height;
    slot.format = format;
    return true;
}

} // namespace

void capture_if_wanted(gx2vk::Image* tv_img, uint32_t tv_w, uint32_t tv_h, gx2vk::Image* drc_img, uint32_t drc_w,
                       uint32_t drc_h) {
    if (!rbrew_mod::has_frame_capture()) return;
    if (!gx2vk::ready()) return;

    const uint64_t completed = gx2vk::completed_serial();
    for (int i = 0; i < kSlotCount; ++i) {
        const int idx = (s_next_slot + i) % kSlotCount;
        CaptureSlot& slot = s_slots[idx];
        if (slot.serial != 0 && completed >= slot.serial) {
            gx2vk::invalidate_mapped(slot.staging.memory);
            if (slot.staging.memory.mapped && slot.width > 0 && slot.height > 0) {
                rbrew_mod::submit_captured_frame(slot.staging.memory.mapped, slot.width, slot.height);
            }
            slot.serial = 0;
        }
    }

    uint32_t view = SPLATMOD_SCREEN_GAMEPAD;
    uint32_t cap_w = 0;
    uint32_t cap_h = 0;
    if (!rbrew_mod::query_frame_capture(&view, &cap_w, &cap_h)) return;

    gx2vk::Image* src_img = (view == SPLATMOD_SCREEN_TV) ? tv_img : drc_img;
    uint32_t src_w = (view == SPLATMOD_SCREEN_TV) ? tv_w : drc_w;
    uint32_t src_h = (view == SPLATMOD_SCREEN_TV) ? tv_h : drc_h;
    if (!src_img || !src_img->image || src_w == 0 || src_h == 0) return;

    src_w = std::min(src_w, src_img->desc.width);
    src_h = std::min(src_h, src_img->desc.height);
    if (src_w == 0 || src_h == 0) return;

    if (!gx2vk::format_supports(src_img->desc.format, VK_FORMAT_FEATURE_BLIT_SRC_BIT)) return;

    if (cap_w == 0 || cap_h == 0) compute_auto_extent(src_w, src_h, cap_w, cap_h);
    if (cap_w == 0 || cap_h == 0 || cap_w > 4096u || cap_h > 4096u) return;

    int free_idx = -1;
    for (int i = 0; i < kSlotCount; ++i) {
        const int idx = (s_next_slot + i) % kSlotCount;
        if (s_slots[idx].serial == 0) {
            free_idx = idx;
            break;
        }
    }
    if (free_idx < 0) return; // Both readback buffers still in flight on GPU.

    CaptureSlot& slot = s_slots[free_idx];
    if (!ensure_slot(slot, cap_w, cap_h, stream_format(src_img->desc.format))) return;

    VkCommandBuffer cb = gx2vk::cmd();
    if (!cb) return;

    gx2vk::barrier_if_dirty();
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(src_w), int32_t(src_h), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {int32_t(cap_w), int32_t(cap_h), 1};
    vkCmdBlitImage(cb, src_img->image, VK_IMAGE_LAYOUT_GENERAL, slot.scaled.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                   VK_FILTER_LINEAR);
    gx2vk::note_gpu_write(slot.scaled);
    gx2vk::barrier_if_dirty();

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {cap_w, cap_h, 1};
    vkCmdCopyImageToBuffer(cb, slot.scaled.image, VK_IMAGE_LAYOUT_GENERAL, slot.staging.buffer, 1, &region);

    VkMemoryBarrier host_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_barrier, 0,
                         nullptr, 0, nullptr);
    gx2vk::note_gpu_write();

    slot.serial = gx2vk::recording_serial();
    s_next_slot = (free_idx + 1) % kSlotCount;
}

void shutdown() {
    for (int i = 0; i < kSlotCount; ++i) release_slot(s_slots[i]);
    s_next_slot = 0;
}

} // namespace gx2_mod_capture

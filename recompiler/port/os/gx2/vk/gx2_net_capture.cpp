#include "gx2_net_capture.h"
#include "../../../runtime/netplay.h"

#include <atomic>

namespace {
struct Slot {
    gx2vk::Image image;
    gx2vk::Buffer buffer;
    uint32_t width = 0, height = 0;
    std::atomic<bool> busy{false};
};
Slot s_slots[3];

VkFormat stream_format(VkFormat source) {
    // sRGB -> sRGB keeps the stored bytes, which are what the window shows.
    return source == VK_FORMAT_R8G8B8A8_SRGB || source == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_R8G8B8A8_SRGB
                                                                                  : VK_FORMAT_R8G8B8A8_UNORM;
}
} // namespace

void gx2_net_capture(gx2vk::Image& tv, uint32_t width, uint32_t height) {
    if (!tv.image || !width || !height || !rbrew_net::host_wants_video()) return;
    Slot* slot = nullptr;
    for (Slot& s : s_slots)
        if (!s.busy.load(std::memory_order_acquire)) {
            slot = &s;
            break;
        }
    if (!slot) return;

    uint32_t cap_w = 0, cap_h = 0;
    rbrew_net::stream_extent(width, height, cap_w, cap_h);
    const VkFormat format = stream_format(tv.desc.format);
    if (!slot->image || slot->image.desc.format != format || slot->width != cap_w || slot->height != cap_h) {
        gx2vk::destroy_image(slot->image);
        gx2vk::ImageDesc desc;
        desc.width = cap_w;
        desc.height = cap_h;
        desc.format = format;
        desc.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!gx2vk::create_image(slot->image, desc)) return;
        slot->width = cap_w;
        slot->height = cap_h;
    }
    const VkDeviceSize needed = VkDeviceSize(cap_w) * cap_h * 4;
    if (!slot->buffer.buffer || slot->buffer.size < needed) {
        if (slot->buffer.buffer) gx2vk::destroy_buffer(slot->buffer);
        slot->buffer = gx2vk::create_buffer(needed, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gx2vk::MemoryUse::Readback);
        if (!slot->buffer.buffer) return;
    }

    VkCommandBuffer cb = gx2vk::cmd();
    gx2vk::barrier_if_dirty();
    VkImageBlit blit{};
    blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(width), int32_t(height), 1};
    blit.dstOffsets[1] = {int32_t(cap_w), int32_t(cap_h), 1};
    vkCmdBlitImage(cb, tv.image, VK_IMAGE_LAYOUT_GENERAL, slot->image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                   VK_FILTER_LINEAR);
    gx2vk::note_gpu_write(slot->image);
    gx2vk::barrier_if_dirty();

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {cap_w, cap_h, 1};
    vkCmdCopyImageToBuffer(cb, slot->image.image, VK_IMAGE_LAYOUT_GENERAL, slot->buffer.buffer, 1, &region);
    VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &host;
    vkCmdPipelineBarrier2(cb, &dependency);

    slot->busy.store(true, std::memory_order_release);
    gx2vk::defer([slot, cap_w, cap_h] {
        if (gx2vk::invalidate_mapped(slot->buffer.memory))
            rbrew_net::host_submit_frame(slot->buffer.memory.mapped, cap_w, cap_h);
        slot->busy.store(false, std::memory_order_release);
    });
}

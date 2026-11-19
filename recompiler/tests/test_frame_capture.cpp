#include "os/gx2/vk/gx2_vk_capture.h"
#include "rebrewu/logger.hpp"

#include <stdexcept>
#include <vector>

namespace {
bool rendering = false;
unsigned begins = 0, ends = 0, clears = 0;
VkBool32 depth_write = VK_FALSE;
VkPipeline pipeline = VK_NULL_HANDLE;
struct Draw {
    uint32_t width;
    VkBool32 depth_write;
    VkPipeline pipeline;
};
std::vector<Draw> draws;
uint32_t width = 0;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
VKAPI_ATTR void VKAPI_CALL begin(VkCommandBuffer, const VkRenderingInfo* info) {
    require(!rendering, "replay nested a rendering instance");
    rendering = true;
    width = info->renderArea.extent.width;
    ++begins;
}
VKAPI_ATTR void VKAPI_CALL end(VkCommandBuffer) {
    require(rendering, "replay ended a rendering instance that was not open");
    rendering = false;
    ++ends;
}
VKAPI_ATTR void VKAPI_CALL bind(VkCommandBuffer, VkPipelineBindPoint, VkPipeline value) {
    pipeline = value;
}
VKAPI_ATTR void VKAPI_CALL set_depth(VkCommandBuffer, VkBool32 value) {
    depth_write = value;
}
VKAPI_ATTR void VKAPI_CALL draw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t) {
    require(rendering, "replay drew outside a rendering instance");
    draws.push_back({width, depth_write, pipeline});
}
VKAPI_ATTR void VKAPI_CALL clear(VkCommandBuffer, uint32_t, const VkClearAttachment*, uint32_t, const VkClearRect*) {
    require(rendering, "replay cleared outside a rendering instance");
    ++clears;
}
VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer cb, const VkDependencyInfo*) {
    require(cb != gx2vk::cmd(), "replay moved a prologue barrier into the main command buffer");
}
void pass(uint32_t w, uint32_t h, VkImageView target, bool set_state) {
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = target;
    attachment.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea.extent = {w, h};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &attachment;
    vkCmdBeginRendering(gx2vk::cmd(), &info);
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    vkCmdPipelineBarrier2(reinterpret_cast<VkCommandBuffer>(uintptr_t(2)), &dependency);
    if (set_state) {
        vkCmdBindPipeline(gx2vk::cmd(), VK_PIPELINE_BIND_POINT_GRAPHICS, reinterpret_cast<VkPipeline>(uintptr_t(3)));
        vkCmdSetDepthWriteEnable(gx2vk::cmd(), VK_TRUE);
    }
    VkClearAttachment color{};
    color.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    VkClearRect rect{info.renderArea, 0, 1};
    vkCmdClearAttachments(gx2vk::cmd(), 1, &color, 1, &rect);
    vkCmdDraw(gx2vk::cmd(), 3, 1, 0, 0);
    vkCmdEndRendering(gx2vk::cmd());
}
} // namespace

// The capture test runs authored commands through mock Vulkan entry points.
// It needs neither a GPU nor game assets.
namespace gx2vk {
VkCommandBuffer cmd() {
    return reinterpret_cast<VkCommandBuffer>(uintptr_t(1));
}
bool is_current_command_buffer(VkCommandBuffer command_buffer) {
    return command_buffer == cmd();
}
} // namespace gx2vk

int main() {
    try {
        vkCmdBeginRendering = begin;
        vkCmdEndRendering = end;
        vkCmdBindPipeline = bind;
        vkCmdSetDepthWriteEnable = set_depth;
        vkCmdDraw = draw;
        vkCmdClearAttachments = clear;
        vkCmdPipelineBarrier2 = barrier;
        gx2vk::capture::install();
        gx2vk::capture::set_enabled(true);
        pass(128, 1, reinterpret_cast<VkImageView>(uintptr_t(1)), true);
        pass(1920, 1080, reinterpret_cast<VkImageView>(uintptr_t(2)), false);
        gx2vk::capture::set_enabled(false);
        gx2vk::capture::finish_frame();
        require(gx2vk::capture::replay_complete(), "authored frame capture was incomplete");
        for (unsigned replay = 0; replay < 2; ++replay) {
            begins = ends = clears = 0;
            depth_write = VK_FALSE;
            pipeline = VK_NULL_HANDLE;
            draws.clear();
            gx2vk::capture::replay(nullptr);
            require(!rendering && begins == ends, "replay left a rendering instance open");
            require(draws.size() == 1 && draws[0].width == 1920,
                    "replay repeated an exposure draw or lost a scene draw");
            require(clears == 1, "replay repeated an exposure clear or lost a scene clear");
            require(draws[0].depth_write == VK_TRUE && draws[0].pipeline != VK_NULL_HANDLE,
                    "replay dropped state inherited from the exposure pass");
        }
        RBREW_LOG_INFO("test", "Frame capture preserves inherited state without repeating exposure writes");
        return 0;
    } catch (const std::exception& error) {
        RBREW_LOG_ERROR("test", "%s", error.what());
        return 1;
    }
}

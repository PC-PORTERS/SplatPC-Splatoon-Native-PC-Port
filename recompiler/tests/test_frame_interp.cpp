#include "os/gx2/gx2_frame_interp.h"
#include "os/gx2/gx2_render.h"
#include "os/gx2/vk/gx2_vk_capture.h"
#include "os/gx2/vk/gx2_render_vk_state.h"
#include "runtime/benchmark_input.h"
#include "runtime/port_settings.h"
#include "rebrewu/logger.hpp"

#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void record_frame() {
    gx2interp::before_item();
    gx2interp::draw_unit(1, false);
    require(gx2r::ensure_rendering(), "authored frame could not open its rendering instance");
    gx2interp::present_begin();
    require(!gx2r::rendering_active(), "presentation disabled capture before closing the rendering instance");
}
} // namespace

int main() {
    if (!gx2vk::init(nullptr)) return 77;
    try {
        gx2_render_init();
        gx2r::ColorTarget target;
        target.width = target.height = 32;
        gx2vk::ImageDesc desc;
        desc.width = desc.height = 32;
        desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        require(gx2vk::create_image(target.image, desc), "authored target allocation failed");
        auto& framebuffer = gx2r::state().framebuffer;
        framebuffer.colors[0] = &target;
        framebuffer.valid = true;
        rbrew_settings::g_fps120.store(true);
        gx2interp::set_wanted(true);
        rbrew_benchmark::frame_draws.store(1000);
        record_frame();
        require(!gx2interp::frame_complete(0), "first frame unexpectedly interpolated without prior transforms");
        record_frame();
        require(gx2interp::frame_complete(0), "second authored frame did not schedule interpolation");
        require(!gx2interp::capturing(), "a pending replay reopened the next frame's capture");
        gx2interp::before_item();
        require(!gx2vk::capture::enabled(), "queued presentation commands can enter the next frame's capture");
        gx2interp::service();
        require(gx2interp::capturing(), "capture did not resume after interpolation completed");
        require(gx2vk::finish(), "authored interpolation commands failed to complete");
        require(gx2vk::validation_error_count() == 0, "authored interpolation reported Vulkan validation errors");
        framebuffer = {};
        gx2vk::destroy_image(target.image);
        gx2_render_shutdown();
        RBREW_LOG_INFO("test", "Pending interpolation keeps presentation out of the next capture");
        return 0;
    } catch (const std::exception& error) {
        RBREW_LOG_ERROR("test", "%s", error.what());
        gx2_render_shutdown();
        return 1;
    }
}

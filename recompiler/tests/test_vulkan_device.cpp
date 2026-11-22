#include "os/gx2/vk/gx2_vk_device.h"
#include "rebrewu/logger.hpp"
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void policy() {
    using namespace gx2vk::device_detail;
    require(matches("", 0, "AMD Radeon Graphics"), "automatic selection rejected AMD");
    require(matches("9060 xt", 1, "AMD Radeon RX 9060 XT"), "case insensitive AMD name selection failed");
    require(matches("radeon graphics", 0, "AMD Radeon Graphics"), "integrated AMD selection failed");
    require(matches("Intel", 2, "Intel Graphics"), "Intel selection failed");
    require(matches("NVIDIA", 3, "NVIDIA GeForce"), "NVIDIA selection failed");
    require(matches("1", 1, "AMD Radeon RX 9060 XT"), "index selection failed");
    require(!matches("0", 1, "AMD Radeon RX 9060 XT"), "index selection ignored");
    require(!matches("NVIDIA", 1, "AMD Radeon RX 9060 XT"), "unmatched vendor selected");
    require(!matches("999999999999999999999", 1, "AMD Radeon RX 9060 XT"), "overflowing index accepted");
    require(score(VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) > score(VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU),
            "discrete GPU preference lost");
    require(score(VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) > score(VK_PHYSICAL_DEVICE_TYPE_CPU),
            "integrated GPU preference lost");

    VkPhysicalDeviceFeatures core{};
    VkPhysicalDeviceVulkan12Features f12{};
    VkPhysicalDeviceVulkan13Features f13{};
    VkBool32* required[] = {&f13.dynamicRendering,      &f13.synchronization2,
                            &f12.timelineSemaphore,     &f12.hostQueryReset,
                            &core.geometryShader,       &core.independentBlend,
                            &core.textureCompressionBC, &core.imageCubeArray,
                            &core.largePoints,          &f13.shaderDemoteToHelperInvocation};
    for (auto feature : required) *feature = VK_TRUE;
    require(!missing_feature(core, f12, f13), "compatible adapter rejected");
    for (auto feature : required) {
        *feature = VK_FALSE;
        require(missing_feature(core, f12, f13), "missing required feature accepted");
        *feature = VK_TRUE;
    }
    require(!missing_feature(core, f12, f13), "optional features became requirements");
}
} // namespace

int main() {
    try {
        policy();
        RBREW_LOG_INFO("test", "GPU selection and required feature checks passed\n");
        return 0;
    } catch (const std::exception& error) {
        RBREW_LOG_ERROR("test", "%s\n", error.what());
        return 1;
    }
}

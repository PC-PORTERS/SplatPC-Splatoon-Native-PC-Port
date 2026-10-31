// Synthetic memory topologies, injected driver failures and real GPU round trips.
// No guest code, game files, saves or shader caches are used.
#include "os/gx2/vk/gx2_vk_memory.h"
#include "rebrewu/logger.hpp"
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace gx2vk;
constexpr auto local = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
constexpr auto visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
constexpr auto coherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
constexpr auto cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

void policy() {
    VkPhysicalDeviceMemoryProperties memory{};
    memory.memoryHeapCount = 2;
    memory.memoryHeaps[0] = {256ull << 20, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    memory.memoryHeaps[1] = {8ull << 30, 0};
    memory.memoryTypeCount = 7;
    memory.memoryTypes[0] = {visible | coherent | cached, 1};
    memory.memoryTypes[1] = {local, 0};
    memory.memoryTypes[2] = {local | visible | coherent, 0};
    memory.memoryTypes[3] = {visible | cached, 1};
    memory.memoryTypes[4] = {0, 1}; // GPU-accessible, not host-mappable
    memory.memoryTypes[5] = {local | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, 0};
    memory.memoryTypes[6] = {local | VK_MEMORY_PROPERTY_PROTECTED_BIT, 0};
    auto types = memory_detail::memory_types(memory, ~0u, MemoryUse::DeviceLocal);
    require(types.count == 5 && types.indices[0] == 1 && types.indices[1] == 2,
            "GPU-local preference or unsupported-type exclusion failed");
    require(types.indices[4] == 4, "non-mappable system memory was excluded from GPU-only fallback");
    types = memory_detail::memory_types(memory, ~0u, MemoryUse::Upload);
    require(types.count == 3 && types.indices[0] == 2, "small local mapped heap was excluded");
    require(types.indices[1] == 0 && types.indices[2] == 3, "upload coherence preference failed");
    types = memory_detail::memory_types(memory, ~0u, MemoryUse::Readback);
    require(types.count == 3 && types.indices[0] == 0 && types.indices[1] == 3, "readback caching preference failed");
    types = memory_detail::memory_types(memory, 1u << 3, MemoryUse::Upload);
    require(types.count == 1 && types.indices[0] == 3, "non-coherent mapped fallback was excluded");
    types = memory_detail::memory_types(memory, 1u << 4, MemoryUse::DeviceLocal);
    require(types.count == 1 && types.indices[0] == 4, "resource compatibility mask was ignored");
    require(memory_detail::memory_types(memory, 0, MemoryUse::Upload).count == 0, "empty mask accepted");
    memory.memoryTypeCount = 1;
    memory.memoryHeapCount = 1;
    memory.memoryHeaps[0] = {8ull << 30, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    memory.memoryTypes[0] = {local | visible | coherent | cached, 0};
    for (const auto use : {MemoryUse::DeviceLocal, MemoryUse::Upload, MemoryUse::Readback}) {
        types = memory_detail::memory_types(memory, 1, use);
        require(types.count == 1 && types.indices[0] == 0, "single-heap UMA is unsupported");
    }
    Allocation a;
    a.offset = 512;
    a.size = 512;
    a.backing_size = 2048;
    auto range = memory_detail::mapped_range(a, 256, 13, 17);
    require(range.offset == 512 && range.size == 256, "suballocation atom alignment failed");
    range = memory_detail::mapped_range(a, 256, 255, 2);
    require(range.offset == 512 && range.size == 512, "cross-atom range failed");
    a.offset = 0;
    a.size = 300;
    a.backing_size = 300;
    range = memory_detail::mapped_range(a, 256, 257, VK_WHOLE_SIZE);
    require(range.offset == 256 && range.size == 44, "dedicated tail crossed backing allocation");
    std::puts("memory policy: discrete, small BAR, shared fallback, non-coherent, UMA and atom ranges passed");
}

PFN_vkAllocateMemory original_allocate;
PFN_vkMapMemory original_map;
PFN_vkFlushMappedMemoryRanges original_flush;
PFN_vkInvalidateMappedMemoryRanges original_invalidate;
uint32_t rejected_types = 0, map_failures = 0, map_calls = 0;
VkDeviceSize maximum_request = VK_WHOLE_SIZE;
VkResult forced_error = VK_SUCCESS;
std::vector<uint32_t> attempted_types;
std::vector<VkMappedMemoryRange> flushed, invalidated;
VKAPI_ATTR VkResult VKAPI_CALL allocate_hook(VkDevice device, const VkMemoryAllocateInfo* info,
                                             const VkAllocationCallbacks* callbacks, VkDeviceMemory* memory) {
    attempted_types.push_back(info->memoryTypeIndex);
    if (forced_error != VK_SUCCESS) return forced_error;
    if ((rejected_types & (1u << info->memoryTypeIndex)) || info->allocationSize > maximum_request)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    return original_allocate(device, info, callbacks, memory);
}
VKAPI_ATTR VkResult VKAPI_CALL map_hook(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
                                        VkMemoryMapFlags flags, void** data) {
    ++map_calls;
    if (map_failures) {
        --map_failures;
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    return original_map(device, memory, offset, size, flags, data);
}
VKAPI_ATTR VkResult VKAPI_CALL flush_hook(VkDevice, uint32_t count, const VkMappedMemoryRange* ranges) {
    flushed.insert(flushed.end(), ranges, ranges + count);
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL invalidate_hook(VkDevice, uint32_t count, const VkMappedMemoryRange* ranges) {
    invalidated.insert(invalidated.end(), ranges, ranges + count);
    return VK_SUCCESS;
}
struct Hooks {
    Hooks() {
        original_allocate = vkAllocateMemory;
        original_map = vkMapMemory;
        original_flush = vkFlushMappedMemoryRanges;
        original_invalidate = vkInvalidateMappedMemoryRanges;
        vkAllocateMemory = allocate_hook;
        vkMapMemory = map_hook;
    }
    ~Hooks() {
        vkAllocateMemory = original_allocate;
        vkMapMemory = original_map;
        vkFlushMappedMemoryRanges = original_flush;
        vkInvalidateMappedMemoryRanges = original_invalidate;
    }
};
struct Device {
    Device() {
        if (!init(nullptr)) throw std::runtime_error("Vulkan 1.3 device initialization failed");
    }
    ~Device() { shutdown(); }
};

void allocator() {
    Device device_guard;
    const auto& memory = memory_properties();
    std::printf("GPU: %s, %u heaps, %u memory types\n", caps().device_name.c_str(), memory.memoryHeapCount,
                memory.memoryTypeCount);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        std::printf("  type %u: flags 0x%x, heap %u (%llu MiB)\n", i, memory.memoryTypes[i].propertyFlags,
                    memory.memoryTypes[i].heapIndex,
                    (unsigned long long)(memory.memoryHeaps[memory.memoryTypes[i].heapIndex].size >> 20));
    Hooks hooks;
    const auto before = allocated_bytes();
    VkMemoryRequirements requirements{20ull << 20, 256, ~0u};
    const auto types = memory_detail::memory_types(memory, requirements.memoryTypeBits, MemoryUse::DeviceLocal);
    require(types.count != 0, "no GPU memory types");
    auto a = allocate(requirements, MemoryUse::DeviceLocal, false);
    require(bool(a) && a.memory_type == types.indices[0], "fastest type was not selected");
    require(!a.mapped && map_calls == 0, "GPU-only memory was unnecessarily mapped");
    free_now(a);
    require(allocated_bytes() == before, "dedicated allocation accounting leaked");
    uint32_t mapped_types = 0;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if (memory.memoryTypes[i].propertyFlags & visible) mapped_types |= 1u << i;
    requirements.memoryTypeBits = mapped_types;
    a = allocate(requirements, MemoryUse::DeviceLocal, false);
    require(bool(a) && !a.mapped && map_calls == 0, "GPU-only host-visible memory was unnecessarily mapped");
    free_now(a);
    requirements.memoryTypeBits = ~0u;

    // Exhaust every local type. No real memory pressure or giant allocation is needed.
    uint32_t nonlocal = 0;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if (memory.memoryTypes[i].propertyFlags & local)
            rejected_types |= 1u << i;
        else
            nonlocal |= 1u << i;
    }
    attempted_types.clear();
    a = allocate(requirements, MemoryUse::DeviceLocal, false);
    if (memory_detail::memory_types(memory, nonlocal, MemoryUse::DeviceLocal).count) {
        require(bool(a) && !(a.properties & local), "local allocation failure did not reach shared memory");
        std::printf("injected local OOM: recovered on system-memory type %u after %zu attempts\n", a.memory_type,
                    attempted_types.size());
        free_now(a);
    } else
        require(!a, "UMA exhaustion should fail without an incompatible type");
    require(allocated_bytes() == before, "fallback allocation leaked");
    rejected_types = ~0u;
    a = allocate(requirements, MemoryUse::Upload, true);
    require(!a && allocated_bytes() == before, "failed candidates leaked accounting");
    rejected_types = 0;
    forced_error = VK_ERROR_DEVICE_LOST;
    attempted_types.clear();
    require(!allocate(requirements, MemoryUse::Upload, true) && attempted_types.size() == 1,
            "device loss was retried as memory pressure");
    forced_error = VK_SUCCESS;

    // A failed pool block must still try a fitting allocation in the fast heap.
    requirements.size = 4096;
    maximum_request = 4096;
    attempted_types.clear();
    a = allocate(requirements, MemoryUse::Upload, true);
    const auto upload_types = memory_detail::memory_types(memory, ~0u, MemoryUse::Upload);
    require(bool(a) && a.block < 0 && a.memory_type == upload_types.indices[0], "exact-size recovery failed");
    require(attempted_types.size() == 2, "expected pool failure then exact-size success");
    free_now(a);
    maximum_request = VK_WHOLE_SIZE;
    map_failures = 1;
    requirements.size = 20ull << 20;
    attempted_types.clear();
    a = allocate(requirements, MemoryUse::Upload, true);
    if (upload_types.count > 1)
        require(bool(a) && map_failures == 0 && attempted_types.size() >= 2,
                "mapping failure did not try another type");
    else
        require(!a, "single mapped type unexpectedly survived mapping failure");
    free_now(a);
    require(allocated_bytes() == before, "mapping failure leaked memory accounting");

    // Validate the actual flush API with captured driver calls, even on a fully
    // coherent GPU. No fake property flags are sent to a real Vulkan driver.
    vkFlushMappedMemoryRanges = flush_hook;
    vkInvalidateMappedMemoryRanges = invalidate_hook;
    const auto atom = caps().limits.nonCoherentAtomSize;
    uint8_t byte = 0;
    Allocation fake;
    fake.mapped = &byte;
    fake.properties = visible;
    fake.offset = atom * 2;
    fake.size = atom * 2;
    fake.backing_size = atom * 8;
    require(flush_mapped(fake, atom / 2, 1) && invalidate_mapped(fake, atom - 1, 2), "mapped synchronization failed");
    require(flushed.size() == 1 && flushed[0].offset == atom * 2 && flushed[0].size == atom,
            "flush range is not atom aligned");
    require(invalidated.size() == 1 && invalidated[0].size == atom * 2, "invalidate range was truncated");
    fake.properties |= coherent;
    require(flush_mapped(fake) && invalidate_mapped(fake) && flushed.size() == 1 && invalidated.size() == 1,
            "coherent memory made unnecessary driver synchronization calls");
    std::puts("allocator: placement, OOM recovery, map recovery, fatal errors and cache synchronization passed");
}

void transfers() {
    Device device_guard;
    // Exercise every driver-supported mapped type, including non-coherent memory
    // when present, instead of only the default preferred allocation.
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = 4096;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    const auto& memory = memory_properties();
    for (uint32_t type = 0; type < memory.memoryTypeCount; ++type) {
        VkBuffer source = VK_NULL_HANDLE;
        require(vkCreateBuffer(device(), &info, nullptr, &source) == VK_SUCCESS, "source buffer creation failed");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device(), source, &requirements);
        requirements.memoryTypeBits &= 1u << type;
        if (!memory_detail::memory_types(memory, requirements.memoryTypeBits, MemoryUse::Upload).count) {
            vkDestroyBuffer(device(), source, nullptr);
            continue;
        }
        auto allocation = allocate(requirements, MemoryUse::Upload, true);
        require(bool(allocation), "compatible mapped allocation failed");
        require(vkBindBufferMemory(device(), source, allocation.memory, allocation.offset) == VK_SUCCESS,
                "source bind failed");
        for (unsigned i = 0; i < 4096; ++i) allocation.mapped[i] = uint8_t(i * 13 + type);
        require(flush_mapped(allocation, 0, 4096), "source flush failed");
        auto destination = create_buffer(4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUse::Readback);
        require(bool(destination.memory), "readback allocation failed");
        VkBufferCopy copy{0, 0, 4096};
        vkCmdCopyBuffer(cmd(), source, destination.buffer, 1, &copy);
        VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        host.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &host;
        vkCmdPipelineBarrier2(cmd(), &dependency);
        require(finish() && invalidate_mapped(destination.memory), "GPU readback completion failed");
        require(std::memcmp(allocation.mapped, destination.memory.mapped, 4096) == 0,
                "shared-memory GPU transfer corrupted bytes");
        vkDestroyBuffer(device(), source, nullptr);
        free_now(allocation);
        vkDestroyBuffer(device(), destination.buffer, nullptr);
        free_now(destination.memory);
        std::printf("mapped type %u: exact 4096-byte GPU round trip passed\n", type);
    }
    // Ring writes flush at submission, while optimal-tiled images keep the
    // existing buffer-to-image transfer and are checked through image readback.
    ImageDesc desc;
    desc.width = 32;
    desc.height = 32;
    desc.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    // Run shared placement first, before any local image pool exists. This
    // verifies binding and actual image access, not only raw memory allocation.
    uint32_t local_types = 0;
    for (uint32_t type = 0; type < memory.memoryTypeCount; ++type)
        if (memory.memoryTypes[type].propertyFlags & local) local_types |= 1u << type;
    VkImageCreateInfo probe_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    probe_info.imageType = VK_IMAGE_TYPE_2D;
    probe_info.format = desc.format;
    probe_info.extent = {32, 32, 1};
    probe_info.mipLevels = 1;
    probe_info.arrayLayers = 1;
    probe_info.samples = VK_SAMPLE_COUNT_1_BIT;
    probe_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    probe_info.usage = desc.usage;
    VkImage probe = VK_NULL_HANDLE;
    require(vkCreateImage(device(), &probe_info, nullptr, &probe) == VK_SUCCESS, "image compatibility probe failed");
    VkMemoryRequirements image_requirements{};
    vkGetImageMemoryRequirements(device(), probe, &image_requirements);
    vkDestroyImage(device(), probe, nullptr);
    if (memory_detail::memory_types(memory, image_requirements.memoryTypeBits & ~local_types, MemoryUse::DeviceLocal)
            .count) {
        Image shared;
        {
            Hooks hooks;
            rejected_types = local_types;
            require(create_image(shared, desc) && !(shared.memory.properties & local),
                    "optimal image did not fall back to system memory");
            rejected_types = 0;
        }
        VkClearColorValue clear{};
        clear.float32[0] = 1.0f;
        clear.float32[3] = 1.0f;
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd(), shared.image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        note_gpu_write(shared);
        std::vector<uint8_t> pixels;
        require(read_image(shared, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 32, 32, pixels),
                "system-memory image readback failed");
        require(pixels.size() == 4096, "system-memory image size changed");
        for (unsigned i = 0; i < pixels.size(); i += 4)
            require(pixels[i] == 255 && pixels[i + 1] == 0 && pixels[i + 2] == 0 && pixels[i + 3] == 255,
                    "GPU writes to system-memory image were corrupted");
        destroy_image(shared);
        require(finish(), "shared image retirement failed");
        std::puts("optimal image in system memory: injected local OOM, binding, GPU clear and exact readback passed");
    } else
        std::puts("optimal image: driver exposes local/UMA memory only for this resource");
    Image image;
    require(create_image(image, desc), "optimal image allocation failed");
    require(image.memory.properties & local, "system memory was forced while local memory was available");
    for (unsigned iteration = 0; iteration < 32; ++iteration) {
        const auto span = upload(4096, 16);
        require(span.data != nullptr, "ring allocation failed");
        std::memset(span.data, uint8_t(iteration + 42), 4096);
        barrier_if_dirty();
        VkBufferImageCopy copy{};
        copy.bufferOffset = span.offset;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {32, 32, 1};
        vkCmdCopyBufferToImage(cmd(), span.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        note_gpu_write(image);
        std::vector<uint8_t> pixels;
        require(read_image(image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 32, 32, pixels), "image readback failed");
        require(pixels.size() == 4096, "image size changed");
        for (auto pixel : pixels) require(pixel == uint8_t(iteration + 42), "ring-to-image bytes changed");
    }
    destroy_image(image);
    require(finish(), "deferred image destruction failed");
    require(validation_error_count() == 0, "Vulkan validation errors");
    std::puts("ring-to-optimal-image: 32 exact texture round trips passed");
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected policy, allocator or transfers");
        const std::string_view mode = argv[1];
        if (mode == "policy")
            policy();
        else if (mode == "allocator")
            allocator();
        else if (mode == "transfers")
            transfers();
        else
            throw std::runtime_error("unknown memory test");
        return 0;
    } catch (const std::exception& error) {
        RBREW_LOG_ERROR("test", "FAIL: %s", error.what());
        return 1;
    }
}

// Measure the production implementations with synthetic data and unchanged work.
#include "Standalone.h"
#include "os/gx2/gx2_detile.h"
#include "os/gx2/gx2_native_context.h"
#include "os/gx2/vk/gx2_vk_shader.h"
#include "os/gx2/vk/gx2_cache_io.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "rebrewu/logger.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace allocation_probe {
thread_local bool enabled = false;
thread_local uint64_t calls = 0;
} // namespace allocation_probe
void* operator new(size_t bytes) {
    if (void* p = std::malloc(bytes ? bytes : 1)) {
        if (allocation_probe::enabled) ++allocation_probe::calls;
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, size_t) noexcept {
    std::free(p);
}
void* operator new[](size_t bytes) {
    return ::operator new(bytes);
}
void operator delete[](void* p) noexcept {
    ::operator delete(p);
}
void operator delete[](void* p, size_t) noexcept {
    ::operator delete(p);
}

void gx2_detile_set_workers_for_test(unsigned);
namespace {
using Clock = std::chrono::steady_clock;
PFN_vkGetPipelineCacheData real_cache_data = nullptr;
VKAPI_ATTR VkResult VKAPI_CALL incomplete_cache_data(VkDevice device, VkPipelineCache cache, size_t* size, void* data) {
    const auto result = real_cache_data(device, cache, size, data);
    return data && result == VK_SUCCESS ? VK_INCOMPLETE : result;
}
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
template <class F> double median_ms(F fn, unsigned repeats = 7) {
    std::vector<double> samples;
    fn();
    for (unsigned i = 0; i < repeats; ++i) {
        const auto start = Clock::now();
        fn();
        samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}
uint32_t original_address(const Gx2DetileDesc& d, uint32_t x, uint32_t y) {
    const auto mode = static_cast<Latte::E_HWTILEMODE>(d.tile_mode);
    const auto height = d.surface_height ? d.surface_height : d.height;
    if (d.tile_mode <= 1 || d.tile_mode == 16)
        return LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, d.slice, 0, d.bits_per_element, d.pitch, height,
                                                               1);
    if (d.tile_mode <= 3)
        return LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, d.slice, d.bits_per_element, d.pitch, height,
                                                                   mode, d.is_depth);
    return LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiled(x, y, d.slice, 0, d.bits_per_element, d.pitch, height, 1,
                                                               mode, d.is_depth, (d.swizzle >> 8) & 1,
                                                               (d.swizzle >> 9) & 3);
}
void detile(bool benchmark) {
    // Reference bytes use the original uncached AddrLib entry points, so a
    // shared mistake in retile/detile cannot satisfy this comparison.
    std::vector<uint8_t> tiled(2u << 20);
    for (size_t i = 0; i < tiled.size(); ++i) tiled[i] = uint8_t((i * 13) ^ (i >> 9));
    std::vector<uint8_t> roundtrip(tiled.size(), 0xa5);
    std::vector<uint8_t> before(tiled.size());
    std::vector<uint8_t> expected, actual;
    std::vector<uint32_t> addrs(35 * 39);

    for (uint32_t mode = 0; mode <= 16; ++mode)
        for (uint32_t bits : {8u, 16u, 32u, 64u, 128u})
            for (uint32_t slice : {0u, 1u, 4u, 7u})
                for (bool depth : {false, true}) {
                    Gx2DetileDesc d;
                    d.width = 35;
                    d.height = 39;
                    d.pitch = 96;
                    d.surface_height = 64;
                    d.tile_mode = mode;
                    d.bits_per_element = bits;
                    d.slice = slice;
                    d.is_depth = depth;
                    d.swizzle = ((slice + bits) & 7) << 8;
                    const size_t bytes = bits / 8, count = size_t(d.width) * d.height * bytes;
                    expected.resize(count);
                    actual.resize(count);
                    std::memset(roundtrip.data(), 0xa5, roundtrip.size());
                    size_t required = 0;
                    for (uint32_t y = 0; y < d.height; ++y)
                        for (uint32_t x = 0; x < d.width; ++x) {
                            const auto address = original_address(d, x, y);
                            require(size_t(address) + bytes <= tiled.size(), "bad authored source bounds");
                            required = std::max(required, size_t(address) + bytes);
                            addrs[size_t(y) * d.width + x] = address;
                            std::memcpy(expected.data() + (size_t(y) * d.width + x) * bytes, tiled.data() + address,
                                        bytes);
                        }
                    require(gx2_detile_mip0(d, tiled.data(), tiled.size(), actual.data()),
                            "detile rejected valid layout");
                    require(actual == expected, "detile differs from original AddrLib");
                    require(gx2_retile_mip0(d, expected.data(), roundtrip.data(), roundtrip.size()),
                            "retile rejected valid layout");
                    require(gx2_retile_mip0(d, expected.data(), roundtrip.data(), required),
                            "retile rejected a valid partial final tile");
                    for (uint32_t y = 0; y < d.height; ++y)
                        for (uint32_t x = 0; x < d.width; ++x)
                            require(!std::memcmp(roundtrip.data() + addrs[size_t(y) * d.width + x],
                                                 expected.data() + (size_t(y) * d.width + x) * bytes, bytes),
                                    "retile differs from original AddrLib");
                    std::memcpy(before.data(), roundtrip.data(), roundtrip.size());
                    require(!gx2_retile_mip0(d, expected.data(), roundtrip.data(), required - 1),
                            "retile accepted an undersized destination");
                    require(roundtrip == before, "failed retile modified destination bytes");
                }
    std::puts("detile/retile: 680 layout/depth/slice/bit-depth cases match independent AddrLib addresses");
    if (!benchmark) return;
    for (uint32_t mode : {4u, 8u, 12u}) {
        Gx2DetileDesc d;
        d.width = d.height = d.pitch = d.surface_height = 1024;
        d.tile_mode = mode;
        d.bits_per_element = 32;
        std::vector<uint8_t> tiled(16u << 20, 0x7b), linear(4u << 20);
        for (unsigned workers : {0u, 3u}) {
            gx2_detile_set_workers_for_test(workers);
            const double read = median_ms([&] {
                require(gx2_detile_mip0(d, tiled.data(), tiled.size(), linear.data()), "benchmark detile failed");
            });
            std::printf("detile 1024x1024 RGBA8 mode=%u helpers=%u: %.3f ms\n", mode, workers, read);
        }
        const double write = median_ms(
            [&] { require(gx2_retile_mip0(d, linear.data(), tiled.data(), tiled.size()), "benchmark retile failed"); });
        std::printf("retile 1024x1024 RGBA8 mode=%u: %.3f ms\n", mode, write);
    }
    gx2_detile_set_workers_for_test(0);
}
void shader(bool benchmark) {
    require(gx2vk::init(nullptr), "compatible Vulkan device unavailable");
    std::printf("Vulkan: %s\n", gx2vk::caps().device_name.c_str());
    for (size_t source_bytes : {size_t(128), size_t(65536)}) {
        std::string source = "#version 450\nvoid main(){gl_Position=vec4(0.0,0.0,0.0,1.0);}\n//";
        source.resize(source_bytes, 'x');
        source += '\n';
        const auto handle = gx2vk::shader_module(VK_SHADER_STAGE_VERTEX_BIT, source);
        require(handle != VK_NULL_HANDLE, "authored shader did not compile");
        const auto compilations = gx2vk::shader_compile_count();
        const unsigned iterations = source_bytes < 1024 ? 100000 : 2000;
        uint64_t allocations = 0;
        auto hits = [&] {
            allocation_probe::calls = 0;
            allocation_probe::enabled = true;
            for (unsigned i = 0; i < iterations; ++i)
                require(gx2vk::shader_module(VK_SHADER_STAGE_VERTEX_BIT, source) == handle,
                        "module cache returned a different object");
            allocation_probe::enabled = false;
            allocations = allocation_probe::calls;
        };
        const double ms = median_ms(hits, benchmark ? 7 : 1);
        std::printf("shader module hits: bytes=%zu iterations=%u %.3f ms, allocations=%llu\n", source_bytes, iterations,
                    ms, (unsigned long long)allocations);
        require(gx2vk::shader_compile_count() == compilations, "cache hit compiled a shader again");
        if (!benchmark) require(allocations == 0, "shader cache hits allocate unused promise states");
    }
    const std::string shared = "#version 450\nvoid main(){gl_Position=vec4(0.25);}\n";
    const auto before = gx2vk::shader_compile_count();
    std::array<VkShaderModule, 8> modules{};
    std::array<std::thread, 8> workers;
    std::atomic<bool> go{false};
    for (unsigned i = 0; i < workers.size(); ++i)
        workers[i] = std::thread([&, i] {
            go.wait(false);
            modules[i] = gx2vk::shader_module(VK_SHADER_STAGE_VERTEX_BIT, shared);
        });
    go = true;
    go.notify_all();
    for (auto& worker : workers) worker.join();
    require(modules[0] != VK_NULL_HANDLE, "concurrent module compilation failed");
    for (auto module : modules) require(module == modules[0], "concurrent module requests compiled duplicates");
    require(gx2vk::shader_compile_count() == before + 1, "concurrent requests compiled the shader more than once");
    // The fused fingerprints must keep every existing SPIR-V filename valid.
    const auto directory = std::filesystem::temp_directory_path() /
                           ("splport-spirv-" + std::to_string(Clock::now().time_since_epoch().count()));
    gx2vk::shader_set_cache_dir(directory.string());
    for (auto stage : {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT}) {
        const std::string code = stage == VK_SHADER_STAGE_VERTEX_BIT
                                     ? shared
                                     : "#version 450\nlayout(location=0)out vec4 c;void main(){c=vec4(1.0);}\n";
        std::vector<uint32_t> compiled, loaded;
        std::string log;
        require(gx2vk::compile_glsl(stage, code, compiled, &log), "persistent-cache shader compilation failed");
        const auto a =
            gx2vk::stable_hash(code.data(), code.size(), 0xCBF29CE484222325ull ^ (uint64_t(stage) << 32) ^ 1);
        const auto b = gx2vk::stable_hash(code.data(), code.size(), 0x84222325CBF29CE4ull + 1);
        char name[64];
        std::snprintf(name, sizeof(name), "%016llx%016llx.spv", (unsigned long long)a, (unsigned long long)b);
        const auto filename = directory / "spirv" / name;
        require(std::filesystem::exists(filename), "legacy shader cache filename changed");
        const auto count = gx2vk::shader_compile_count();
        require(gx2vk::compile_glsl(stage, code, loaded, &log) && loaded == compiled,
                "persistent SPIR-V cache changed output");
        require(gx2vk::shader_compile_count() == count, "persistent cache hit recompiled a shader");
        std::filesystem::remove(filename);
    }
    require(gx2vk::pipeline_cache() != VK_NULL_HANDLE, "pipeline cache creation failed");
    real_cache_data = vkGetPipelineCacheData;
    vkGetPipelineCacheData = &incomplete_cache_data;
    gx2vk::shader_cache_flush();
    vkGetPipelineCacheData = real_cache_data;
    const auto pipeline_file = directory / "pipelines.bin";
    require(std::filesystem::exists(pipeline_file) && std::filesystem::file_size(pipeline_file) > 0,
            "valid VK_INCOMPLETE pipeline snapshot was discarded");
    std::filesystem::remove(pipeline_file);
    gx2vk::shader_set_cache_dir("");
    std::filesystem::remove(directory / "spirv");
    std::filesystem::remove(directory);
    gx2vk::shader_shutdown();
    gx2vk::shutdown();
}
void cache_io(bool benchmark) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("splport-cache-io-" + std::to_string(Clock::now().time_since_epoch().count()) + ".bin");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    } cleanup{path};
    std::vector<char> expected(16u << 20);
    for (size_t i = 0; i < expected.size(); ++i) expected[i] = char((i * 17) ^ (i >> 8));
    {
        std::ofstream out(path, std::ios::binary);
        out.write(expected.data(), expected.size());
        require(bool(out), "test cache write failed");
    }
    const auto filename = path.string();
    uint64_t allocations = 0;
    std::vector<char> loaded;
    const double ms = median_ms(
        [&] {
            allocation_probe::calls = 0;
            allocation_probe::enabled = true;
            loaded = gx2vk::read_cache_file(filename);
            allocation_probe::enabled = false;
            allocations = allocation_probe::calls;
            require(loaded == expected, "cache loader changed bytes");
        },
        benchmark ? 7 : 1);
    std::printf("cache file load 16 MiB: %.3f ms, allocations=%llu, capacity=%zu\n", ms,
                (unsigned long long)allocations, loaded.capacity());
    require(gx2vk::read_cache_file(filename + ".missing").empty(), "missing cache file did not fall back to empty");
}
void queue(bool benchmark) {
    gx2_native_start_render_thread();
    for (unsigned producers : {1u, 4u}) {
        const unsigned per_thread = benchmark ? 10000 : 1000;
        const double ms = median_ms(
            [&] {
                uint64_t completed = 0;
                std::vector<std::thread> threads;
                for (unsigned i = 0; i < producers; ++i)
                    threads.emplace_back([&] {
                        for (unsigned n = 0; n < per_thread; ++n) gx2_native_run(false, [&] { ++completed; });
                    });
                for (auto& thread : threads) thread.join();
                require(completed == uint64_t(producers) * per_thread, "concurrent render queue lost work");
            },
            benchmark ? 7 : 1);
        std::printf("render handoffs: producers=%u tasks=%u %.3f ms\n", producers, producers * per_thread, ms);
    }
    std::atomic<unsigned> caught{0};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 8; ++i)
        threads.emplace_back([&, i] {
            try {
                gx2_native_run(false, [i] { throw std::runtime_error(std::to_string(i)); });
            } catch (const std::exception& error) {
                if (error.what() == std::to_string(i)) ++caught;
            }
        });
    for (auto& thread : threads) thread.join();
    gx2_native_stop_render_thread();
    require(caught == 8, "concurrent queue exceptions reached the wrong callers");
}
} // namespace
int main(int argc, char** argv) {
    try {
        require(argc >= 2, "expected detile, shader or cache_io");
        const std::string_view name(argv[1]);
        const bool bench = argc > 2 && std::string_view(argv[2]) == "benchmark";
        if (name == "detile")
            detile(bench);
        else if (name == "shader")
            shader(bench);
        else if (name == "cache_io")
            cache_io(bench);
        else if (name == "queue")
            queue(bench);
        else
            throw std::runtime_error("unknown case");
        return 0;
    } catch (const std::exception& e) {
        allocation_probe::enabled = false;
        RBREW_LOG_ERROR("test", "FAIL: %s", e.what());
        return 1;
    }
}

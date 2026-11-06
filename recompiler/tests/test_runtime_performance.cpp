// Synthetic host/GPU regressions: no game code, files, saves or shader cache.
#include "runtime/host_sleep.h"
#include "runtime/rebrewu_runtime.h"
#include "os/gx2/gx2_native_context.h"
#include "os/gx2/gx2_addrs.h"
#include "os/gx2/gx2_render.h"
#include "os/gx2/gx2_surface.h"
#include "os/gx2/gx2_detile.h"
#include "os/gx2/vk/gx2_compile_queue.h"
#include "os/gx2/vk/gx2_render_vk_state.h"
#include "rebrewu/logger.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

void gx2_draw_register(CPUState*);
void gx2_display_list_register(CPUState*);
uint64_t gx2_query_fast_read_count_for_test();

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
double thread_cpu_seconds() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    require(GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) != 0, "GetThreadTimes failed");
    auto ticks = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return double(ticks(kernel) + ticks(user)) * 1e-7;
#else
    return 0;
#endif
}
void timing() {
    using Clock = std::chrono::steady_clock;
    // These waits model the audio/guest timer, rather than a tight workload.
    const auto start = Clock::now();
    const double cpu_start = thread_cpu_seconds();
    for (unsigned i = 0; i < 200; ++i) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(2);
        rbrew_host_sleep_until(deadline);
        require(Clock::now() >= deadline, "host sleep returned before its deadline");
    }
    const double wall = std::chrono::duration<double>(Clock::now() - start).count();
    const double cpu = thread_cpu_seconds() - cpu_start;
    std::printf("timer: wall %.3f ms, thread CPU %.3f ms (%.1f%%)\n", wall * 1000, cpu * 1000, cpu / wall * 100);
    // The vblank path uses absolute deadlines. Verify that kernel waits do
    // not accumulate the overshoot of repeated relative sleeps into its phase.
    const auto phase_start = Clock::now();
    const auto period = std::chrono::nanoseconds(16666667);
    for (unsigned i = 1; i <= 20; ++i) {
        const auto deadline = phase_start + period * i;
        rbrew_host_sleep_until(deadline);
        require(Clock::now() >= deadline, "absolute frame timer returned early");
    }
    std::printf("absolute 60 Hz timer: 20 deadlines in %.3f ms (333.333 ms requested)\n",
                std::chrono::duration<double, std::milli>(Clock::now() - phase_start).count());
}
struct Renderer {
    Renderer() { gx2_render_init(); }
    ~Renderer() { gx2_render_shutdown(); }
};
void frame() {
    gx2vk::cmd();
    gx2_render_present(nullptr, false);
}
void uploads() {
    Renderer renderer;
    constexpr uint64_t mib = 1ull << 20;
    const auto initial = gx2vk::allocated_bytes();
    std::array<gx2vk::Span, 8> spans;
    for (auto& span : spans) {
        span = gx2vk::upload(32 * mib, 16);
        require(span.data != nullptr, "upload allocation failed");
        std::memset(span.data, 0x6b, 16);
    }
    gx2vk::cmd();
    require(gx2vk::finish(), "GPU upload submission failed");
    const auto peak = gx2vk::allocated_bytes();
    // Cached spans must remain reusable across idle frames without a new upload.
    for (unsigned i = 0; i < 240; ++i) {
        require(gx2vk::span_alive(spans.front()), "live upload span was evicted");
        require(spans.front().data[0] == 0x6b, "live upload contents changed");
        frame();
    }
    require(gx2vk::finish(), "GPU completion failed");
    const auto retained = gx2vk::allocated_bytes();
    std::printf("upload allocations: initial %llu MiB, peak %llu MiB, retained %llu MiB\n",
                (unsigned long long)(initial / mib), (unsigned long long)(peak / mib),
                (unsigned long long)(retained / mib));
    for (const auto& span : spans)
        require(gx2vk::span_resident(span), "cached upload was evicted without reuse pressure");
    require(retained == peak, "idle frames changed upload residency");
}
void textures() {
    Renderer renderer;
    std::vector<uint8_t> mem(65536);
    constexpr uint32_t descriptor = 0x1000, image = 0x4000, size = 32 * 32 * 4;
    auto word = [&](uint32_t offset, uint32_t value) { rbrew_write32(mem.data(), descriptor + offset, value); };
    word(GX2SURF_DIM, 1);
    word(GX2SURF_WIDTH, 32);
    word(GX2SURF_HEIGHT, 32);
    word(GX2SURF_DEPTH, 1);
    word(GX2SURF_MIP_LEVELS, 1);
    word(GX2SURF_FORMAT, 0x1a);
    word(GX2SURF_IMAGE_SIZE, size);
    word(GX2SURF_TILE_MODE, 1);
    word(GX2SURF_PITCH, 32);
    word(GX2TEX_VIEW_NUM_SLICES, 1);
    word(GX2TEX_COMP_MAP, 0x06880000);
    auto bind = [&](uint32_t address, uint8_t value) {
        std::memset(mem.data() + address, value, size);
        word(GX2SURF_IMAGE, address);
        gx2_render_bind_texture(descriptor, 0, mem.data());
    };
    bind(image, 0x42);
    auto& state = gx2r::state();
    require(state.tex_map.size() == 1, "first texture was not cached");
    auto& cold = state.tex_map.begin()->second;
    require(bool(cold.image), "first texture upload failed");
    gx2_render_save_context_state(0x1234);
    bind(image + size, 0x71);
    require(state.tex_map.size() == 2, "second texture was not cached");
    auto* active = state.units[0][0];
    auto readback = gx2vk::create_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gx2vk::MemoryUse::Readback);
    require(readback.memory.mapped != nullptr, "in-flight readback allocation failed");
    for (unsigned i = 0; i < 240; ++i) {
        if (i == 179) {
            // Reference the about-to-be-retired image in commands that have
            // not been submitted yet. Retirement must preserve those handles.
            gx2vk::barrier_if_dirty();
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {32, 32, 1};
            auto cb = gx2vk::cmd();
            vkCmdCopyImageToBuffer(cb, cold.image.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &copy);
            VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            host.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &host;
            vkCmdPipelineBarrier2(cb, &dependency);
        }
        frame();
    }
    require(gx2vk::finish(), "GPU texture submission failed");
    require(bool(cold.image), "unbound reusable texture was evicted");
    require(active && active->image && *active->image, "bound texture storage was evicted");
    require(gx2vk::invalidate_mapped(readback.memory), "in-flight readback invalidation failed");
    for (unsigned i = 0; i < size; ++i)
        require(readback.memory.mapped[i] == 0x42, "in-flight texture copy was corrupted");
    gx2vk::destroy_buffer(readback);
    const auto count = gx2_render_texture_upload_count();
    gx2_render_restore_context_state(0x1234, mem.data());
    require(cold.image && gx2_render_texture_upload_count() == count,
            "saved context needlessly reuploaded a cached texture");
    std::vector<uint8_t> pixels;
    require(gx2vk::read_image(cold.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 32, 32, pixels), "texture readback failed");
    require(pixels.size() == size, "unexpected texture readback size");
    for (auto pixel : pixels) require(pixel == 0x42, "reloaded texture pixels changed");
    for (unsigned transition = 0; transition < 3; ++transition) {
        bind(image + size, 0x71);
        for (unsigned i = 0; i < 200; ++i) frame();
        require(bool(cold.image), "repeated transition evicted reusable texture storage");
        gx2_render_restore_context_state(0x1234, mem.data());
        require(gx2vk::read_image(cold.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 32, 32, pixels),
                "transition readback failed");
        for (auto pixel : pixels) require(pixel == 0x42, "repeated context transition changed pixels");
        require(gx2vk::image_count() <= 2, "repeated transition accumulated texture images");
    }
    require(gx2vk::validation_error_count() == 0, "Vulkan validation reported an error");
    require(gx2_render_texture_upload_count() == count, "repeated transitions reuploaded unchanged textures");
    std::puts("texture reuse: images, in-flight copies and saved contexts preserved with no repeated uploads");
}
void queue() {
    // Repeated sleep/wake transitions and FIFO ordering. A missed notification
    // hangs this test, bounded by CTest's timeout.
    gx2_native_start_render_thread();
    unsigned observed = 0;
    double render_cpu_start = 0, render_cpu_end = 0;
    gx2_native_run(true, [&] { render_cpu_start = thread_cpu_seconds(); });
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 1; i <= 200; ++i) {
        rbrew_host_sleep_for(2000);
        gx2_native_run(true, [&] {
            require(observed + 1 == i, "render queue reordered tasks");
            observed = i;
        });
    }
    gx2_native_run(true, [&] { render_cpu_end = thread_cpu_seconds(); });
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double cpu = render_cpu_end - render_cpu_start;
    std::printf("render queue idle handoffs: wall %.3f ms, render CPU %.3f ms\n", wall * 1000, cpu * 1000);
    struct Task final : Gx2AsyncWork {
        unsigned& value;
        unsigned expected;
        Task(unsigned& value, unsigned expected) : value(value), expected(expected) {}
        void run() override {
            require(value + 1 == expected, "async queue reordered tasks");
            value = expected;
        }
    };
    for (unsigned i = 201; i <= 1224; ++i) gx2_native_post(new Task(observed, i));
    gx2_native_run(true, [&] { require(observed == 1224, "waited task overtook asynchronous work"); });
    std::array<std::thread, 3> producers;
    for (auto& producer : producers)
        producer = std::thread([&] {
            for (unsigned i = 0; i < 100; ++i) gx2_native_run(true, [&] { ++observed; });
        });
    for (auto& producer : producers) producer.join();
    gx2_native_stop_render_thread();
    require(observed == 1524, "render queue lost a task");
    std::puts("render queue: sleep/wake, asynchronous FIFO and concurrent producers passed");
}
// Fixed integer work, so every run does identical work and only contention
// changes its wall time.
uint64_t busy_work(uint64_t seed, unsigned iterations) {
    for (unsigned i = 0; i < iterations; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
    }
    return seed;
}
// A small-CPU frame model: the game thread computes in chunks, posts bursts of
// render work and waits for the queue at the end of the frame (the plaza's
// occlusion read), while another guest thread works and sleeps. Restrict CPUs
// with RBREW_BENCH_AFFINITY=<hex mask>; compare RBREW_HOST_SPIN=full.
#if defined(_WIN32)
void contention() {
    if (const char* mask = std::getenv("RBREW_BENCH_AFFINITY"))
        require(SetProcessAffinityMask(GetCurrentProcess(), DWORD_PTR(std::strtoull(mask, nullptr, 16))) != 0,
                "SetProcessAffinityMask failed");
    const unsigned frames = 120, warmup = 10;
    gx2_native_start_render_thread();
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> worker_steps{0}, sink{0}, worker_sink{0};
    std::thread worker([&] {
        uint64_t seed = 7;
        while (!stop.load(std::memory_order_relaxed)) {
            seed = busy_work(seed, 1000000);
            worker_steps.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        worker_sink = seed;
    });
    struct Replay final : Gx2AsyncWork {
        std::atomic<uint64_t>& sink;
        uint64_t seed;
        Replay(std::atomic<uint64_t>& sink, uint64_t seed) : sink(sink), seed(seed) {}
        void run() override { sink.fetch_add(busy_work(seed, 12000), std::memory_order_relaxed); }
    };
    std::vector<double> times;
    uint64_t seed = 1, steps_start = 0;
    auto process_cpu = [] {
        FILETIME created{}, exited{}, kernel{}, user{};
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        auto ticks = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        return double(ticks(kernel) + ticks(user)) * 1e-7;
    };
    double cpu_start = 0;
    auto run_start = std::chrono::steady_clock::now();
    for (unsigned f = 0; f < frames + warmup; ++f) {
        if (f == warmup) {
            steps_start = worker_steps.load();
            cpu_start = process_cpu();
            run_start = std::chrono::steady_clock::now();
        }
        const auto start = std::chrono::steady_clock::now();
        for (unsigned chunk = 0; chunk < 8; ++chunk) {
            seed = busy_work(seed, 1000000);
            for (unsigned i = 0; i < 40; ++i) gx2_native_post(new Replay(sink, seed + i));
        }
        gx2_native_run(true, [] {});
        if (f >= warmup)
            times.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - run_start).count();
    const double cpu = process_cpu() - cpu_start;
    const uint64_t steps = worker_steps.load() - steps_start;
    stop = true;
    worker.join();
    gx2_native_stop_render_thread();
    std::sort(times.begin(), times.end());
    std::printf("{\"case\":\"contention\",\"frame_ms_p50\":%.3f,\"frame_ms_p95\":%.3f,\"frame_ms_max\":%.3f,"
                "\"worker_steps_per_s\":%.1f,\"process_cpu_per_frame_ms\":%.3f,\"checksum\":%llu}\n",
                times[times.size() / 2], times[times.size() * 95 / 100], times.back(), steps / wall,
                cpu * 1000 / frames, (unsigned long long)((sink.load() + seed) & 0xffff));
}
#endif
void queries() {
    Renderer renderer;
    std::vector<uint8_t> mem(65536);
    std::vector<HostFunc> table((ADDR_GX2QueryGetOcclusionResult >> 2) + 1);
    CPUState cpu{};
    cpu.mem = mem.data();
    cpu.guest_memory_size = uint32_t(mem.size());
    cpu.dispatch_table = table.data();
    cpu.dispatch_size = uint32_t(table.size());
    gx2_draw_register(&cpu);
    gx2_display_list_register(&cpu);
    auto call = [&](uint32_t fn, uint32_t a, uint32_t b = 0) {
        cpu.r[3] = a;
        cpu.r[4] = b;
        cpu.r[5] = 0;
        require(table[fn >> 2] != nullptr, "query import missing");
        table[fn >> 2](&cpu);
        return cpu.r[3];
    };
    constexpr uint32_t query = 0x100, sibling = 0x200, output = 0x300, list = 0x1000;
    call(ADDR_GX2BeginDisplayListEx, list, 4096);
    for (auto address : {query, sibling}) {
        call(ADDR_GX2QueryBegin, 0, address);
        call(ADDR_GX2QueryEnd, 0, address);
    }
    const uint32_t bytes = call(ADDR_GX2EndDisplayList, list);
    gx2_native_start_render_thread();
    gx2_dl_call(&cpu, list, bytes);
    gx2_native_run(true, [] {});
    auto idle = [] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!gx2_native_queue_idle() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        require(gx2_native_queue_idle(), "render queue did not become idle");
    };
    idle();
    require(call(ADDR_GX2QueryGetOcclusionResult, query, output) == 1, "completed query was not published");
    idle();
    const auto before = gx2_query_fast_read_count_for_test();
    require(call(ADDR_GX2QueryGetOcclusionResult, sibling, output) == 1, "sibling query was not published");
    const auto fast_start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 1000; ++i)
        require(call(ADDR_GX2QueryGetOcclusionResult, query, output) == 1 && rbrew_read64(mem.data(), output) == 0,
                "cached query changed its result");
    require(gx2_query_fast_read_count_for_test() - before == 1001,
            "listed queries still round-trip through the render queue");
    const double fast_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fast_start).count();
    const auto queued_start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 1000; ++i)
        gx2_native_run(true,
                       [&] { require(gx2_render_query_result(query, output, mem.data()), "queued query failed"); });
    const double queued_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - queued_start).count();
    rbrew_write64(mem.data(), query + 8, rbrew_bswap64((1ull << 63) | 7));
    require(call(ADDR_GX2QueryGetOcclusionResult, query, output) == 1 && rbrew_read64(mem.data(), output) == 7,
            "CPU counter updates were hidden by a cache");
    rbrew_write64(mem.data(), query, 0);
    require(call(ADDR_GX2QueryGetOcclusionResult, query, output) == 0 && rbrew_read64(mem.data(), output) == 0,
            "CPU-cleared counters reported a stale result");
    rbrew_write64(mem.data(), query, rbrew_bswap64(1ull << 63));
    std::array<uint8_t, 64> previous{};
    std::memcpy(previous.data(), mem.data() + query, previous.size());
    call(ADDR_GX2BeginDisplayListEx, list + 4096, 4096);
    call(ADDR_GX2QueryBegin, 0, query);
    const uint32_t begin_bytes = call(ADDR_GX2EndDisplayList, list + 4096);
    require(call(ADDR_GX2QueryGetOcclusionResult, query, output) == 0,
            "recording a CPU query did not clear its counters immediately");
    std::memcpy(mem.data() + query, previous.data(), previous.size());
    struct Gate final : Gx2AsyncWork {
        std::promise<void>& entered;
        std::shared_future<void> release;
        Gate(std::promise<void>& entered, std::shared_future<void> release) : entered(entered), release(release) {}
        void run() override {
            entered.set_value();
            release.wait();
        }
    };
    std::promise<void> entered, release;
    gx2_native_post(new Gate(entered, release.get_future().share()));
    entered.get_future().wait();
    const bool busy = !gx2_native_queue_idle();
    gx2_dl_call(&cpu, list + 4096, begin_bytes);
    auto pending = std::async(std::launch::async, [&] { return call(ADDR_GX2QueryGetOcclusionResult, query, output); });
    // A begin still queued behind render work waits for the render queue:
    // Splatoon uses the result in the same frame (RBREW_QUERY_ASYNC=1 opts
    // into an immediate "not ready" instead).
    const bool waited = pending.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
    release.set_value();
    const auto result = pending.get();
    require(busy && waited && result == 0, "queued query begin reused a stale completed result");
    gx2_native_run(true, [&] { gx2_render_query_end(0, query, mem.data()); });
    require(call(ADDR_GX2QueryGetOcclusionResult, query, output) == 1 && rbrew_read64(mem.data(), output) == 0,
            "query reuse failed after pending work");
    gx2_native_stop_render_thread();
    std::puts("queries: 1001 caller-side reads; sibling publication, CPU writes and queued begin ordering preserved");
    std::printf("synthetic 1000 completed-query reads: queued %.3f ms, caller-side %.3f ms (not game FPS)\n", queued_ms,
                fast_ms);
}
void compiler() {
    gx2vk::CompileQueue queue(1);
    std::promise<void> started, released;
    auto release = released.get_future().share();
    auto blocker = queue.post(
        [&] {
            started.set_value();
            release.wait();
            return 0;
        },
        true);
    started.get_future().wait();
    std::vector<unsigned> order;
    auto first = queue.post(
        [&] {
            order.push_back(1);
            return 1;
        },
        true);
    auto second = queue.post(
        [&] {
            order.push_back(2);
            return 2;
        },
        true);
    auto demand = queue.post([&] {
        order.push_back(9);
        return 9;
    });
    released.set_value();
    require(queue.get(demand) == 9, "demand compile job failed");
    require(queue.get(first) == 1 && queue.get(second) == 2 && queue.get(blocker) == 0,
            "background compile job failed");
    queue.drain();
    require(order.size() == 3 && order.front() == 9, "demand compilation did not overtake warm-up jobs");
    std::puts("compiler: one worker completes demand work ahead of queued warm-up without deadlock");
}
} // namespace
void gx2_detile_set_workers_for_test(unsigned);
unsigned gx2_detile_workers();
void detile() {
    for (unsigned mode : {1u, 2u, 4u})
        for (unsigned bpp : {8u, 16u, 32u, 64u, 128u}) {
            Gx2DetileDesc desc;
            desc.width = 129;
            desc.height = 130;
            desc.pitch = 256;
            desc.surface_height = 256;
            desc.bits_per_element = bpp;
            desc.tile_mode = mode;
            const size_t size = size_t(desc.width) * desc.height * (bpp / 8);
            std::vector<uint8_t> linear(size), serial(size), parallel(size), tiled(4u << 20);
            for (size_t i = 0; i < size; ++i) linear[i] = uint8_t((i * 17) ^ (i >> 9));
            require(gx2_retile_mip0(desc, linear.data(), tiled.data(), tiled.size()),
                    "synthetic texture retile failed");
            gx2_detile_set_workers_for_test(0);
            require(gx2_detile_mip0(desc, tiled.data(), tiled.size(), serial.data()), "serial detile failed");
            gx2_detile_set_workers_for_test(3);
            require(gx2_detile_mip0(desc, tiled.data(), tiled.size(), parallel.data()), "parallel detile failed");
            require(serial == linear && parallel == linear, "serial/parallel detiling changed texture bytes");
        }
    gx2_detile_set_workers_for_test(0);
    std::puts("detile: serial and parallel paths match across linear/micro/macro tiling and 8-128 bit elements");
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected one regression test name");
        const std::string_view mode(argv[1]);
        // These checks cover the waiting query path; the default
        // "assume visible" answer for busy listed reads is a separate choice.
#if defined(_WIN32)
        _putenv_s("RBREW_QUERY_ASSUME_VISIBLE", "0");
#else
        setenv("RBREW_QUERY_ASSUME_VISIBLE", "0", 1);
#endif
        if (mode == "timing")
            timing();
        else if (mode == "queue")
            queue();
#if defined(_WIN32)
        else if (mode == "contention")
            contention();
#endif
        else if (mode == "detile")
            detile();
        else if (mode == "compiler")
            compiler();
        else if (mode == "uploads" || mode == "textures" || mode == "queries") {
            if (!gx2vk::init(nullptr)) {
                std::puts("No compatible Vulkan device available");
                return 77;
            }
            std::printf("Vulkan device: %s\n", gx2vk::caps().device_name.c_str());
            if (mode == "uploads")
                uploads();
            else if (mode == "queries")
                queries();
            else
                textures();
        } else
            throw std::runtime_error("unknown test mode");
        return 0;
    } catch (const std::exception& error) {
        RBREW_LOG_ERROR("test", "FAIL: %s", error.what());
        return 1;
    }
}

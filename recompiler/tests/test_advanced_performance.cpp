#include "runtime/rebrewu_runtime.h"
#include "runtime/rebrewu_memory_observer.h"
#include "os/coreinit/coreinit_addrs.h"
#include "os/gx2/vk/gx2_compile_queue.h"
#include "os/gx2/vk/gx2_vk.h"
#include "os/gx2/vk/gx2_last_lookup.h"
#include "os/gx2/gx2_render.h"
#include "rebrewu/logger.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

void coreinit_sync_register(CPUState*);
bool gx2_render_vk_test_pipeline_cache();
namespace {
using Clock = std::chrono::steady_clock;
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
template <class F> double median_ms(F&& fn, unsigned repeats) {
    std::vector<double> samples;
    fn();
    for (unsigned i = 0; i < repeats; ++i) {
        const auto begin = Clock::now();
        fn();
        samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

void reference_store(uint8_t* out, double value, unsigned type, unsigned scale) {
    const int exponent = int(scale) - ((scale & 32) ? 64 : 0);
    uint32_t result;
    unsigned bytes;
    if (!type) {
        const uint64_t bits = std::bit_cast<uint64_t>(value);
        result = ((bits >> 52) & 2047) <= 896 ? (std::signbit(value) ? 0x80000000u : 0u)
                                              : std::bit_cast<uint32_t>(float(value));
        bytes = 4;
    } else {
        const bool negative = std::signbit(value);
        const double lo = type == 6 ? -128.0 : type == 7 ? -32768.0 : 0.0;
        const double hi = type == 4 ? 255.0 : type == 5 ? 65535.0 : type == 6 ? 127.0 : 32767.0;
        const double scaled = std::isnan(value) ? (negative ? lo : hi) : std::ldexp(value, exponent);
        result = uint32_t(int32_t(std::max(lo, std::min(scaled, hi))));
        bytes = type == 4 || type == 6 ? 1 : 2;
    }
    for (unsigned i = 0; i < bytes; ++i) out[i] = uint8_t(result >> ((bytes - i - 1) * 8));
}
void paired(bool benchmark) {
    std::vector<uint8_t> memory(8192, 0xa5);
    CPUState cpu{};
    cpu.mem = memory.data();
    cpu.r[4] = 0x1000;
    std::vector<double> values = {0.0,
                                  -0.0,
                                  0.5,
                                  -0.5,
                                  1.0,
                                  -1.0,
                                  127.999,
                                  -128.999,
                                  255.999,
                                  32767.9,
                                  -32768.9,
                                  65535.9,
                                  1e-200,
                                  -1e-200,
                                  1e200,
                                  -1e200,
                                  std::numeric_limits<double>::denorm_min(),
                                  std::numeric_limits<double>::min(),
                                  std::numeric_limits<double>::max(),
                                  std::numeric_limits<double>::infinity(),
                                  -std::numeric_limits<double>::infinity(),
                                  std::numeric_limits<double>::quiet_NaN(),
                                  -std::numeric_limits<double>::quiet_NaN(),
                                  std::bit_cast<double>(0x7ff0000000000001ull),
                                  std::bit_cast<double>(0xfff0000000000001ull),
                                  std::nextafter(1.0, 0.0),
                                  std::nextafter(1.0, 2.0)};
    uint32_t random = 0x98765432;
    for (unsigned i = 0; i < 64; ++i) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        values.push_back(std::bit_cast<double>((uint64_t(random) << 32) | (random * 0x85ebca6bu)));
    }
    unsigned cases = 0;
    for (unsigned type : {0u, 4u, 5u, 6u, 7u})
        for (unsigned scale = 0; scale < 64; ++scale)
            for (unsigned single : {0u, 1u})
                for (size_t i = 0; i < values.size(); ++i) {
                    const auto p0 = values[i], p1 = values[(i + 13) % values.size()];
                    cpu.f[3] = p0;
                    cpu.ps1[3] = p1;
                    cpu.gqr[0] = type | (scale << 8);
                    std::array<uint8_t, 16> expected;
                    expected.fill(0xa5);
                    const unsigned bytes = type == 4 || type == 6 ? 1 : type == 5 || type == 7 ? 2 : 4;
                    reference_store(expected.data(), p0, type, scale);
                    if (!single) reference_store(expected.data() + bytes, p1, type, scale);
                    std::memset(memory.data() + 0x1010, 0xa5, 16);
                    const uint32_t instruction = (60u << 26) | (3u << 21) | (4u << 16) | (single << 15) | 16u;
                    rbrew_ps(&cpu, instruction);
                    require(!std::memcmp(memory.data() + 0x1010, expected.data(), 16),
                            "psq store differs from independent ldexp reference");
                    require(cpu.r[4] == 0x1000, "non-updating psq store changed base register");
                    ++cases;
                }
    cpu.gqr[0] = 1;
    bool rejected = false;
    try {
        rbrew_ps(&cpu, (60u << 26) | (3u << 21) | (4u << 16));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "undefined quantization type was accepted");
    cpu.gqr[0] = 4;
    cpu.f[3] = 2;
    cpu.ps1[3] = 3;
    rbrew_ps(&cpu, (61u << 26) | (3u << 21) | (4u << 16) | 16u);
    require(cpu.r[4] == 0x1010 && memory[0x1010] == 2 && memory[0x1011] == 3, "updating psq store changed semantics");
    cpu.r[4] = 0x1000;
    cpu.gqr[0] = 5;
    cpu.f[3] = 42;
    cpu.ps1[3] = 43;
    rbrew_set_strict_reservations(true);
    rbrew_write32(cpu.mem, 0x1000, 0x002a002b);
    CPUState reserving{};
    reserving.mem = cpu.mem;
    rbrew_lwarx(&reserving, 0x1000);
    struct Observed {
        unsigned count = 0;
        uint32_t address[2]{}, size[2]{};
    } observed;
    const auto prior = rbrew_set_write_observer({cpu.mem,
                                                 [](uint32_t address, uint32_t size, void* user) noexcept {
                                                     auto& out = *static_cast<Observed*>(user);
                                                     if (out.count < 2) {
                                                         out.address[out.count] = address;
                                                         out.size[out.count] = size;
                                                     }
                                                     ++out.count;
                                                 },
                                                 &observed});
    rbrew_ps(&cpu, (60u << 26) | (3u << 21) | (4u << 16));
    rbrew_stwcx(&reserving, 0x1000, 0x11223344);
    rbrew_set_write_observer(prior);
    rbrew_set_strict_reservations(false);
    require(!(reserving.cr[0] & 2), "unchanged paired store failed to break a strict reservation");
    require(observed.count == 2 && observed.address[0] == 0x1000 && observed.address[1] == 0x1002 &&
                observed.size[0] == 2 && observed.size[1] == 2,
            "paired store changed ordered write-observer ranges");
    std::printf(
        "paired stores: %u exact cases, all 64 scales, NaNs, infinities, denormals and update addressing passed\n",
        cases);
    if (!benchmark) return;
    constexpr unsigned iterations = 1000000;
    for (unsigned type : {0u, 4u, 5u, 6u, 7u}) {
        cpu.gqr[0] = type | (3u << 8);
        cpu.f[3] = 2.375;
        cpu.ps1[3] = -4.875;
        const uint32_t instruction = (60u << 26) | (3u << 21) | (4u << 16);
        const auto ms = median_ms(
            [&] {
                for (unsigned i = 0; i < iterations; ++i) rbrew_ps(&cpu, instruction);
            },
            7);
        std::printf("paired store type=%u: %u instructions %.3f ms\n", type, iterations, ms);
    }
}

struct Imports {
    std::unique_ptr<HostFunc, decltype(&std::free)> table{nullptr, &std::free};
    CPUState cpu{};
    Imports() {
        const uint32_t count = 0x02D20000u >> 2;
        table.reset(static_cast<HostFunc*>(std::calloc(count, sizeof(HostFunc))));
        require(bool(table), "test dispatch allocation failed");
        cpu.dispatch_table = table.get();
        cpu.dispatch_size = count;
        coreinit_sync_register(&cpu);
    }
    HostFunc get(uint32_t addr) {
        auto fn = cpu.dispatch_table[addr >> 2];
        require(fn != nullptr, "missing synchronization import");
        return fn;
    }
};
void synchronization(bool benchmark) {
    Imports imports;
    const auto lock = imports.get(ADDR_OSLockMutex), unlock = imports.get(ADDR_OSUnlockMutex);
    const auto init = imports.get(ADDR_OSInitMutex), try_lock = imports.get(ADDR_OSTryLockMutex);
    CPUState cpu{};
    cpu.r[3] = 0x7000;
    init(&cpu);
    lock(&cpu);
    lock(&cpu);
    unlock(&cpu);
    unlock(&cpu);
    std::atomic<bool> tried = false;
    std::atomic<uint32_t> acquired = 2;
    lock(&cpu);
    std::thread other([&] {
        CPUState c{};
        c.r[3] = 0x7000;
        try_lock(&c);
        acquired = c.r[3];
        tried = true;
        tried.notify_one();
    });
    tried.wait(false);
    other.join();
    unlock(&cpu);
    require(acquired == 0, "try-lock bypassed cross-thread ownership");
    uint64_t protected_value = 0;
    std::vector<std::thread> workers;
    for (unsigned n = 0; n < 4; ++n)
        workers.emplace_back([&] {
            CPUState c{};
            for (unsigned i = 0; i < 2000; ++i) {
                c.r[3] = 0x7000;
                lock(&c);
                ++protected_value;
                unlock(&c);
            }
        });
    for (auto& worker : workers) worker.join();
    require(protected_value == 8000, "guest mutex lost mutual exclusion");
    // More addresses than the front cache: collisions must return the right object.
    const auto event_init = imports.get(ADDR_OSInitEvent), event_wait = imports.get(ADDR_OSWaitEvent);
    const auto signal = imports.get(ADDR_OSSignalEvent), reset = imports.get(ADDR_OSResetEvent);
    for (unsigned i = 0; i < 256; ++i) {
        cpu.r[3] = 0x10000 + i * 64;
        cpu.r[4] = 0;
        cpu.r[5] = 1;
        event_init(&cpu);
    }
    for (unsigned i = 0; i < 256; ++i) {
        cpu.r[3] = 0x10000 + i * 64;
        signal(&cpu);
        event_wait(&cpu);
        reset(&cpu);
    }
    const auto sem_init = imports.get(ADDR_OSInitSemaphore), sem_wait = imports.get(ADDR_OSWaitSemaphore),
               sem_signal = imports.get(ADDR_OSSignalSemaphore);
    cpu.r[3] = 0x20000;
    cpu.r[4] = 1;
    sem_init(&cpu);
    sem_wait(&cpu);
    sem_signal(&cpu);
    sem_wait(&cpu);
    // Reinitialization changes the shared object in place: caches cannot retain its old state.
    cpu.r[4] = 2;
    sem_init(&cpu);
    sem_wait(&cpu);
    sem_wait(&cpu);
    const auto cond_init = imports.get(ADDR_OSInitCond), cond_wait = imports.get(ADDR_OSWaitCond),
               cond_signal = imports.get(ADDR_OSSignalCond);
    cpu.r[3] = 0x24000;
    cond_init(&cpu);
    unsigned turn = 0, exchanged = 0;
    auto take = [&](CPUState& c) {
        c.r[3] = 0x25000;
        lock(&c);
    };
    auto release = [&](CPUState& c) {
        c.r[3] = 0x25000;
        unlock(&c);
    };
    auto wait = [&](CPUState& c) {
        c.r[3] = 0x24000;
        c.r[4] = 0x25000;
        cond_wait(&c);
    };
    auto wake = [&](CPUState& c) {
        c.r[3] = 0x24000;
        cond_signal(&c);
    };
    std::thread waiter([&] {
        CPUState c{};
        for (unsigned i = 0; i < 400; ++i) {
            take(c);
            while (turn != 1) wait(c);
            turn = 0;
            ++exchanged;
            wake(c);
            release(c);
        }
    });
    for (unsigned i = 0; i < 400; ++i) {
        take(cpu);
        while (turn != 0) wait(cpu);
        turn = 1;
        wake(cpu);
        release(cpu);
    }
    waiter.join();
    require(exchanged == 400, "condition-variable handoff lost a wakeup");
    std::puts("guest synchronization: recursion, ownership, mutual exclusion, address collisions, events and semaphore "
              "reinitialization passed");
    if (!benchmark) return;
    for (unsigned producers : {1u, 4u}) {
        constexpr unsigned per_thread = 50000;
        const auto ms = median_ms(
            [&] {
                std::vector<std::thread> threads;
                for (unsigned n = 0; n < producers; ++n)
                    threads.emplace_back([&, n] {
                        CPUState c{};
                        c.r[3] = 0x30000 + n * 64;
                        for (unsigned i = 0; i < per_thread; ++i) {
                            lock(&c);
                            unlock(&c);
                        }
                    });
                for (auto& thread : threads) thread.join();
            },
            7);
        std::printf("guest locks: %u independent threads, %u lock/unlock pairs %.3f ms\n", producers,
                    per_thread * producers, ms);
    }
}

struct KeyHash {
    using is_transparent = void;
    size_t operator()(std::string_view key) const { return std::hash<std::string_view>{}(key); }
};
void pipeline_lookup(bool benchmark) {
    gx2vk::LastLookup<512, int> lookup;
    int first = 1, second = 2;
    const std::string a("key\0a", 5), b("key\0b", 5);
    require(lookup.find(a) == nullptr, "empty front cache hit");
    lookup.remember(a, &first);
    require(lookup.find(a) == &first && lookup.find(b) == nullptr, "binary state-key comparison failed");
    require(lookup.find(std::string_view(a).substr(0, 4)) == nullptr, "state-key length was ignored");
    lookup.remember(b, &second);
    require(lookup.find(b) == &second, "state-key replacement failed");
    lookup.reset();
    require(!lookup.find(b), "cache survived owner reset");
    lookup.remember(std::string(513, 'x'), &first);
    require(!lookup.find(a), "oversize state key was truncated");
    gx2vk::CompileQueue queue(1);
    unsigned resolved = 0;
    gx2vk::ResolvedCompileJob<uint64_t> completed(queue.post([] { return uint64_t(42); }, true));
    auto resolve = [&](const auto& pending) {
        ++resolved;
        return queue.get(pending);
    };
    require(completed.get(resolve) == 42 && completed.get(resolve) == 42 && resolved == 1,
            "completed compilation was resolved more than once");
    gx2vk::ResolvedCompileJob<uint64_t> failed(queue.post([]() -> uint64_t { throw std::runtime_error("expected"); }));
    unsigned errors = 0;
    for (unsigned i = 0; i < 2; ++i) try {
            failed.get(resolve);
        } catch (const std::runtime_error&) {
            ++errors;
        }
    require(errors == 2, "compilation exception was replaced by a cached success");
    unsigned null_resolves = 0;
    gx2vk::ResolvedCompileJob<uint64_t> null_job(queue.post([] { return uint64_t(0); }));
    auto resolve_null = [&](const auto& pending) {
        ++null_resolves;
        return queue.get(pending);
    };
    require(null_job.get(resolve_null) == 0 && null_job.get(resolve_null) == 0 && null_resolves == 1,
            "null compilation result was repolled");
    queue.drain();
    std::puts("pipeline lookup: exact binary keys, reset, pending work, one-time resolution, failures and null results "
              "passed");
    if (!benchmark) return;
    using Job = gx2vk::CompileQueue::Job<uint64_t>;
    std::unordered_map<std::string, Job, KeyHash, std::equal_to<>> legacy;
    std::unordered_map<std::string, gx2vk::ResolvedCompileJob<uint64_t>, KeyHash, std::equal_to<>> cached;
    std::vector<std::string> keys;
    for (unsigned n = 0; n < 64; ++n) {
        auto key = std::string(637, char('a' + n % 26));
        key[0] = char(n);
        key.back() = char(n);
        std::promise<uint64_t> promise;
        Job job{promise.get_future().share(), {}};
        promise.set_value(n + 1);
        legacy.emplace(key, job);
        cached.emplace(key, job);
        keys.push_back(std::move(key));
    }
    volatile uint64_t sum = 0;
    constexpr unsigned iterations = 1000000;
    const auto before = median_ms(
        [&] {
            uint64_t total = 0;
            for (unsigned i = 0; i < iterations; ++i) {
                const auto& key = keys[(i / 16) % keys.size()];
                const auto& job = legacy.find(std::string_view(key))->second;
                require(job.ready(), "ready reference pipeline was pending");
                total += job.result.get();
            }
            sum = total;
        },
        7);
    gx2vk::LastLookup<1024, gx2vk::ResolvedCompileJob<uint64_t>> recent;
    const auto after = median_ms(
        [&] {
            uint64_t total = 0;
            for (unsigned i = 0; i < iterations; ++i) {
                const auto& key = keys[(i / 16) % keys.size()];
                auto* entry = recent.find(key);
                if (!entry) {
                    entry = &cached.find(std::string_view(key))->second;
                    recent.remember(key, entry);
                }
                total += entry->get([](const auto& job) { return job.result.get(); });
            }
            sum = total;
        },
        7);
    std::printf("pipeline lookup: %u lookups, 637-byte keys, 16-draw batches: reference %.3f ms, cached %.3f ms "
                "(checksum %llu)\n",
                iterations, before, after, (unsigned long long)sum);
}
void pipeline_gpu() {
    struct Renderer {
        Renderer() { gx2_render_init(); }
        ~Renderer() { gx2_render_shutdown(); }
    } renderer;
    require(gx2vk::ready(), "Vulkan renderer unavailable");
    std::printf("production pipeline cache: %s\n", gx2vk::caps().device_name.c_str());
    require(gx2_render_vk_test_pipeline_cache(), "real pipeline creation/cache/state/warm-up probe failed");
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(gx2vk::physical_device(), nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(gx2vk::physical_device(), nullptr, &count, extensions.data());
    for (const char* name :
         {"VK_EXT_graphics_pipeline_library", "VK_EXT_memory_budget", "VK_EXT_descriptor_indexing"}) {
        bool present = false;
        for (const auto& extension : extensions) present |= !std::strcmp(extension.extensionName, name);
        std::printf("  %s: %s\n", name, present ? "advertised" : "not advertised");
    }
    require(gx2vk::validation_error_count() == 0, "pipeline probe reported Vulkan validation errors");
    std::puts("actual graphics pipelines: repeated handles, changed blend/topology, warmed job reuse and return to "
              "prior state passed");
}
} // namespace
int main(int argc, char** argv) {
    try {
        require(argc >= 2, "expected paired or synchronization");
        const bool bench = argc > 2 && std::string_view(argv[2]) == "benchmark";
        if (std::string_view(argv[1]) == "paired")
            paired(bench);
        else if (std::string_view(argv[1]) == "synchronization")
            synchronization(bench);
        else if (std::string_view(argv[1]) == "pipeline_lookup")
            pipeline_lookup(bench);
        else if (std::string_view(argv[1]) == "pipeline_gpu")
            pipeline_gpu();
        else
            throw std::runtime_error("unknown test");
        return 0;
    } catch (const std::exception& error) {
        RBREW_LOG_ERROR("test", "FAIL: %s", error.what());
        return 1;
    }
}

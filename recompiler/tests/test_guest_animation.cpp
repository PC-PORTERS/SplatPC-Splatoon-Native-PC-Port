// Guest-CPU cost of skinned characters, measured on authored recompiled-form
// code (guest_skeleton_kernels.inl): 96 animated bones and a 64-bone palette
// per character, 1-100 characters. Build the same source against two
// runtimes to compare them; the palette checksum must match.
//
//   gambit_guest_bench npc_<1|10|25|50|100>   timing + exactness
//   gambit_guest_bench scaling                 every count, one JSON line each
//   gambit_guest_bench breakdown|instructions  per-kernel / per-form cost
#include "runtime/rebrewu_runtime.h"
#include "runtime/rebrewu_memory_observer.h"
#include "rebrewu/logger.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
std::atomic<uint64_t> g_allocations{0};
}
void* operator new(size_t size) {
    ++g_allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, size_t) noexcept {
    std::free(p);
}

#define GUEST_PS(word) rbrew_ps_constant<(word)>(cpu)
#define GUEST_SKELETON_NAMESPACE guest_inline
#include "../tests/guest_skeleton_kernels.inl"
#undef GUEST_PS
#undef GUEST_SKELETON_NAMESPACE

namespace {
using namespace guest_skeleton;
using Clock = std::chrono::steady_clock;
void require(bool ok, const char* text) {
    if (!ok) throw std::runtime_error(text);
}
void put_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}
void put_bef(uint8_t* p, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    put_be32(p, v);
}

// Guest layout. Characters share four clips (curve sets) and one model, as
// crowds of one character model playing a few animations do; each has its
// own transforms, matrices and palette.
constexpr uint32_t kClips = 4;
constexpr uint32_t kConstants = 0x00010000u, kModel = 0x00011000u, kClipBase = 0x00020000u;
constexpr uint32_t kStateBytes = kBones * (kTransformBytes + 2 * kMatrixBytes) + kMatrixBytes;
constexpr uint32_t kStackTop = 0x00800000u;

struct World {
    uint8_t* mem{};
    size_t bytes{};
    std::vector<HostFunc> table;
    CPUState cpu{};
    unsigned characters{};
    uint32_t state_base{}, palette_base{};
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    explicit World(unsigned count, HostFunc sample) : characters(count) {
        state_base = (kClipBase + kClips * kBones * kBoneCurveBytes + 4095u) & ~4095u;
        palette_base = (state_base + count * kStateBytes + 4095u) & ~4095u;
        bytes = std::max<size_t>(kStackTop + 4096u, palette_base + size_t(count) * kPaletteBones * kMatrixBytes);
#ifdef _WIN32
        mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        mem = static_cast<uint8_t*>(std::calloc(bytes, 1));
#endif
        require(mem != nullptr, "guest arena allocation failed");
        table.assign(0x00010000u >> 2, nullptr);
        table[kCurveSampleAddress >> 2] = sample;
        table[(kCurveSampleAddress + 4) >> 2] = [](CPUState*) {};
        cpu.mem = mem;
        cpu.dispatch_table = table.data();
        cpu.dispatch_size = uint32_t(table.size());
        rbrew_init_thread_gqrs(&cpu);
        const float constants[] = {0.f, 1.f, 2.f, 3.f};
        for (unsigned i = 0; i < 4; ++i) put_bef(mem + kConstants + i * 4, constants[i]);
        put_be32(mem + kConstants + 16, kCurveSampleAddress);
        const float pairs[] = {2.f, 2.f, 1.f, 1.f, 0.f, 0.f};
        for (unsigned i = 0; i < 6; ++i) put_bef(mem + kConstants + 24 + i * 4, pairs[i]);
        // Model: a branching hierarchy and per-bone inverse binds.
        for (uint32_t b = 0; b < kBones; ++b) {
            const uint16_t parent = b == 0 ? 0xFFFFu : uint16_t(b < 8 ? b - 1 : (b * 5 + 3) % b);
            mem[kModel + b * 2] = uint8_t(parent >> 8);
            mem[kModel + b * 2 + 1] = uint8_t(parent);
            const float inv[12] = {1.f, 0.f, 0.f, b == 0 ? 0.f : -0.01f * float(b),
                                   0.f, 1.f, 0.f, b == 0 ? 0.f : -0.02f * float(b % 7),
                                   0.f, 0.f, 1.f, b == 0 ? 0.f : 0.005f * float(b % 3)};
            for (unsigned w = 0; w < 12; ++w)
                put_bef(mem + kModel + ((kBones * 2 + 15) & ~15u) + b * kMatrixBytes + w * 4, inv[w]);
        }
        // Clips: ten Hermite curves per bone; rotations stay near identity.
        uint32_t seed = 0x2545F491u;
        auto next = [&] {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            return float(seed & 0xFFFFu) / 65535.f;
        };
        for (uint32_t clip = 0; clip < kClips; ++clip)
            for (uint32_t b = 0; b < kBones; ++b)
                for (uint32_t ch = 0; ch < kChannels; ++ch) {
                    uint8_t* curve = mem + kClipBase + (clip * kBones + b) * kBoneCurveBytes + ch * kCurveBytes;
                    put_be32(curve, kKeys);
                    for (uint32_t k = 0; k < kKeys; ++k) {
                        const float base = ch == 3 ? 1.f : ch >= 7 ? 1.f : 0.f;
                        const float amplitude = ch < 4 ? 0.2f : ch < 7 ? 0.05f : 0.02f;
                        put_bef(curve + 8 + k * kKeyBytes, float(k) * 8.f);
                        put_bef(curve + 12 + k * kKeyBytes, base + amplitude * (next() - 0.5f));
                        put_bef(curve + 16 + k * kKeyBytes, 0.01f * (next() - 0.5f));
                    }
                }
    }
    ~World() {
#ifdef _WIN32
        VirtualFree(mem, 0, MEM_RELEASE);
#else
        std::free(mem);
#endif
    }
    template <class Update> void frame(unsigned f, Update update) {
        for (unsigned n = 0; n < characters; ++n) {
            cpu.r[1] = kStackTop - 256;
            cpu.r[13] = kConstants;
            cpu.r[3] = kClipBase + (n % kClips) * kBones * kBoneCurveBytes;
            cpu.r[4] = state_base + n * kStateBytes;
            cpu.r[5] = kModel;
            cpu.r[6] = palette_base + n * kPaletteBones * kMatrixBytes;
            // Characters sharing a clip play it at different phases.
            cpu.f[1] = std::fmod(double(f) * 0.5 + double(n) * 7.25, 119.0);
            cpu.lr = 0x00009000u;
            update(&cpu);
        }
    }
    uint64_t checksum(uint32_t begin, size_t size) const {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < size; ++i) h = (h ^ mem[begin + i]) * 1099511628211ull;
        return h;
    }
};

double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(std::ceil(p * double(v.size())) - 1))];
}

// Root bone: its palette entry is the local matrix (identity inverse bind).
// Recompute it natively from the guest's own transform to check the math.
void check_root(const World& world, unsigned character) {
    auto bef = [&](uint32_t a) {
        const uint8_t* p = world.mem + a;
        uint32_t v = uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
        float f;
        std::memcpy(&f, &v, 4);
        return double(f);
    };
    const uint32_t t = world.state_base + character * kStateBytes;
    const double x = bef(t), y = bef(t + 4), z = bef(t + 8), w = bef(t + 12);
    require(std::abs(x * x + y * y + z * z + w * w - 1.0) < 1e-4, "guest quaternion was not normalized");
    const double s[3] = {bef(t + 28), bef(t + 32), bef(t + 36)}, tr[3] = {bef(t + 16), bef(t + 20), bef(t + 24)};
    const double r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                            {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                            {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
    const uint8_t* palette = world.mem + world.palette_base + character * kPaletteBones * kMatrixBytes;
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned col = 0; col < 4; ++col) {
            float got;
            std::memcpy(&got, palette + (row * 4 + col) * 4, 4); // byte-reversed store: host order
            const double want = col < 3 ? r[row][col] * s[col] : tr[row];
            require(std::abs(double(got) - want) < 1e-4, "guest palette differs from the native transform");
        }
}

struct Result {
    double median, p95, p99, total;
    uint64_t allocations, palette_checksum, state_checksum;
};
Result run(unsigned characters, unsigned frames) {
    World world(characters, &guest_inline::curve_sample);
    for (unsigned f = 0; f < 10; ++f) world.frame(f, guest_inline::skeleton_update);
    std::vector<double> samples;
    samples.reserve(frames);
    const uint64_t allocations = g_allocations.load();
    const auto begin = Clock::now();
    for (unsigned f = 10; f < 10 + frames; ++f) {
        const auto start = Clock::now();
        world.frame(f, guest_inline::skeleton_update);
        samples.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    const double total = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    const uint64_t allocated = g_allocations.load() - allocations;
    for (unsigned n = 0; n < characters; ++n) check_root(world, n);
    return {percentile(samples, .5),
            percentile(samples, .95),
            percentile(samples, .99),
            total,
            allocated,
            world.checksum(world.palette_base, size_t(characters) * kPaletteBones * kMatrixBytes),
            world.checksum(world.state_base, size_t(characters) * kStateBytes)};
}

void report(unsigned characters, unsigned frames) {
    const Result r = run(characters, frames);
    std::printf("{\"metric\":\"guest_skeleton\",\"characters\":%u,\"bones\":%u,\"frames\":%u,\"median_ms\":%.6f,\"p95_"
                "ms\":%.6f,\"p99_ms\":%.6f,"
                "\"per_character_us\":%.4f,\"per_bone_ns\":%.3f,\"allocations\":%llu,\"palette_checksum\":%llu,\"state_"
                "checksum\":%llu,\"passed\":true}\n",
                characters, kBones, frames, r.median, r.p95, r.p99, r.median * 1000.0 / characters,
                r.median * 1e6 / (double(characters) * kBones), (unsigned long long)r.allocations,
                (unsigned long long)r.palette_checksum, (unsigned long long)r.state_checksum);
    require(r.allocations == 0, "guest skeleton frames allocated host memory");
}

// Per-kernel cost (ns per call) on one character's data.
void breakdown() {
    World world(1, &guest_inline::curve_sample);
    world.frame(0, guest_inline::skeleton_update);
    CPUState& cpu = world.cpu;
    auto measure = [&](const char* name, auto setup, auto kernel) {
        constexpr unsigned calls = 200000;
        double best = 1e30;
        for (unsigned repeat = 0; repeat < 5; ++repeat) {
            const auto start = Clock::now();
            for (unsigned i = 0; i < calls; ++i) {
                setup(i);
                kernel(&cpu);
            }
            best = std::min(best, std::chrono::duration<double, std::nano>(Clock::now() - start).count() / calls);
        }
        std::printf("{\"metric\":\"guest_kernel\",\"kernel\":\"%s\",\"ns_per_call\":%.2f}\n", name, best);
    };
    const uint32_t state = world.state_base, locals = state + kBones * kTransformBytes;
    auto common = [&] {
        cpu.r[1] = kStackTop - 256;
        cpu.r[13] = kConstants;
        cpu.lr = 0x9000u;
    };
    measure(
        "curve_sample",
        [&](unsigned i) {
            common();
            cpu.r[3] = kClipBase + (i % (kBones * kChannels)) * kCurveBytes;
            cpu.f[1] = double(i % 113);
        },
        guest_inline::curve_sample);
    measure(
        "bone_animate",
        [&](unsigned i) {
            common();
            cpu.r[3] = kClipBase + (i % kBones) * kBoneCurveBytes;
            cpu.r[4] = state + (i % kBones) * kTransformBytes;
            cpu.f[1] = double(i % 113);
        },
        guest_inline::bone_animate);
    measure(
        "transform_to_matrix",
        [&](unsigned i) {
            common();
            cpu.r[3] = state + (i % kBones) * kTransformBytes;
            cpu.r[4] = locals + (i % kBones) * kMatrixBytes;
        },
        guest_inline::transform_to_matrix);
    measure(
        "matrix_concat",
        [&](unsigned i) {
            common();
            cpu.r[3] = locals + (i % kBones) * kMatrixBytes;
            cpu.r[4] = locals + ((i + 1) % kBones) * kMatrixBytes;
            cpu.r[5] = locals + kBones * kMatrixBytes + (i % kBones) * kMatrixBytes;
        },
        guest_inline::matrix_concat);
    measure("dispatch_only", [&](unsigned) { common(); }, [](CPUState* c) { rbrew_call_indirect(c, 0x00008004u); });
}

// ns per rbrew_ps call by instruction form: the same word repeated (best
// case for branch prediction), then the concat sequence's mix in order.
void instruction_costs() {
    World world(1, &guest_inline::curve_sample);
    CPUState& cpu = world.cpu;
    cpu.r[3] = world.state_base;
    cpu.r[4] = world.state_base + 256;
    cpu.r[13] = kConstants;
    for (unsigned i = 0; i < 32; ++i) {
        cpu.f[i] = 1.0 + i * 0.25;
        cpu.ps1[i] = 2.0 - i * 0.125;
    }
    struct Form {
        const char* name;
        uint32_t word;
    } forms[] = {
        {"psq_l", psq_l(2, 0, 3)},        {"psq_st", psq_st(10, 0, 4)},   {"ps_madds0", ps_madds0(10, 6, 9, 10)},
        {"ps_muls0", ps_muls0(10, 2, 8)}, {"ps_add", ps_add(11, 11, 12)}, {"ps_merge01", ps_merge01(12, 13, 9)},
        {"ps_sum0", ps_sum0(17, 8, 8, 8)}};
    constexpr unsigned calls = 2000000;
    for (const Form& form : forms) {
        double best = 1e30;
        for (unsigned repeat = 0; repeat < 5; ++repeat) {
            const auto start = Clock::now();
            for (unsigned i = 0; i < calls; ++i) rbrew_ps(&cpu, form.word);
            best = std::min(best, std::chrono::duration<double, std::nano>(Clock::now() - start).count() / calls);
            for (unsigned i = 0; i < 32; ++i) {
                cpu.f[i] = 1.0 + i * 0.25;
                cpu.ps1[i] = 2.0 - i * 0.125;
            }
        }
        std::printf("{\"metric\":\"guest_instruction\",\"form\":\"%s\",\"ns_per_call\":%.2f}\n", form.name, best);
    }
}

template <uint32_t word> void compare_paired() {
    uint32_t seed = word | 1u;
    auto random = [&] {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    };
    for (unsigned n = 0; n < 256; ++n) {
        std::vector<uint8_t> memory(8192), expected(8192);
        for (auto& byte : memory) byte = uint8_t(random());
        expected = memory;
        CPUState fast{}, reference{};
        fast.mem = memory.data();
        for (unsigned i = 0; i < 32; ++i) {
            fast.r[i] = 128 + (random() & 255);
            const uint64_t high0 = random(), low0 = random(), high1 = random(), low1 = random();
            fast.f[i] = std::bit_cast<double>((high0 << 32) | low0);
            fast.ps1[i] = std::bit_cast<double>((high1 << 32) | low1);
            constexpr uint64_t edges[] = {0,
                                          0x8000000000000000ull,
                                          0x7ff0000000000000ull,
                                          0xfff0000000000000ull,
                                          0x7ff8000000000001ull,
                                          0x7ff0000000000001ull,
                                          1,
                                          0x0010000000000000ull,
                                          0x7fefffffffffffffull,
                                          0x3ff0000000000000ull,
                                          0xbff0000000000000ull,
                                          0xfff8000000000001ull};
            if (n < 12) {
                fast.f[i] = std::bit_cast<double>(edges[(n + i) % 12]);
                fast.ps1[i] = std::bit_cast<double>(edges[(n + i + 5) % 12]);
            }
            fast.cr[i % 8] = random();
        }
        fast.fpscr = random();
        for (auto& gqr : fast.gqr) {
            const unsigned type = n % 8 < 4 ? 0 : 4 + n % 4;
            gqr = type | (type << 16) | ((n % 64) << 8) | ((n % 64) << 24);
        }
        reference = fast;
        reference.mem = expected.data();
        rbrew_ps(&reference, word);
        rbrew_ps_constant<word>(&fast);
        const bool same = !std::memcmp(fast.f, reference.f, sizeof fast.f) &&
                          !std::memcmp(fast.ps1, reference.ps1, sizeof fast.ps1) &&
                          !std::memcmp(fast.r, reference.r, sizeof fast.r) &&
                          !std::memcmp(fast.cr, reference.cr, sizeof fast.cr) && fast.fpscr == reference.fpscr &&
                          memory == expected;
        if (!same) RBREW_LOG_ERROR("guest", "paired mismatch word=%08X case=%u", word, n);
        require(same, "constant paired instruction differs from interpreter");
    }
}
template <unsigned xo> void compare_paired_math() {
    constexpr uint32_t base = (4u << 26) | (xo << 1);
    compare_paired<base | (3u << 21) | (3u << 16) | (4u << 11) | (5u << 6)>();
    compare_paired<base | (4u << 21) | (3u << 16) | (4u << 11) | (5u << 6) | 1u>();
    compare_paired<base | (5u << 21) | (3u << 16) | (4u << 11) | (5u << 6)>();
}
void paired_equivalence() {
    compare_paired_math<10>();
    compare_paired_math<11>();
    compare_paired_math<12>();
    compare_paired_math<13>();
    compare_paired_math<14>();
    compare_paired_math<15>();
    compare_paired_math<20>();
    compare_paired_math<21>();
    compare_paired_math<25>();
    compare_paired_math<28>();
    compare_paired_math<29>();
    compare_paired<0xE0050000u>();
    compare_paired<0xE1048008u>();
    compare_paired<0xE4640000u>();
    compare_paired<0xE4648FF0u>();
    compare_paired<0xF1440000u>();
    compare_paired<0xF5448FF0u>();
    compare_paired<0xF5408000u>();
    compare_paired<0xE4600000u>();
    compare_paired<0xF1408000u>();
    compare_paired<0xE0608000u>();
    std::puts("paired constant/interpreter: 11008 bit-exact cases, register aliases, quantization fallback and update "
              "addressing");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "scaling";
        const unsigned frames = [] {
            const char* v = std::getenv("RBREW_BENCH_FRAMES");
            const int n = v ? std::atoi(v) : 0;
            return n > 0 ? unsigned(n) : 120u;
        }();
        if (mode == "breakdown") {
            breakdown();
            return 0;
        }
        if (mode == "instructions") {
            instruction_costs();
            return 0;
        }
        if (mode == "paired_equivalence") {
            paired_equivalence();
            return 0;
        }
        if (mode == "scaling") {
            for (unsigned n : {1u, 10u, 25u, 50u, 100u}) report(n, frames);
            return 0;
        }
        if (mode.rfind("npc_", 0) == 0) {
            const unsigned n = unsigned(std::stoul(mode.substr(4)));
            require(n == 1 || n == 10 || n == 25 || n == 50 || n == 100, "unknown character count");
            report(n, frames);
            return 0;
        }
        throw std::runtime_error("unknown mode");
    } catch (const std::exception& e) {
        RBREW_LOG_ERROR("test", "FAILED: %s", e.what());
        return 1;
    }
}

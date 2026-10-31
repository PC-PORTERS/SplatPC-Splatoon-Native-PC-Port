// Authored synthetic shaders and R700 bytecode. No title assets, guest code,
// game entry point, window, audio, or game executable is used by this tool.
#include "os/gx2/r700_to_glsl.h"
#include "runtime/rebrewu_runtime.h"
#include "os/gx2/vk/gx2_vk_shader.h"
#include "os/gx2/vk/gx2_compile_queue.h"
#include "rebrewu/logger.hpp"
#ifdef DEFINE_ENUM_FLAG_OPERATORS
#undef DEFINE_ENUM_FLAG_OPERATORS
#endif
#include "third_party/cemu_shader/util/helpers/StringBuf.h"
#include "shader_reference_geometry.h"
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <psapi.h>
#endif

namespace gx2vk {
bool shader_test_load_disk(const std::string&, size_t, std::vector<uint32_t>&);
}

namespace {
using Clock = std::chrono::steady_clock;
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
double cpu_ms() {
#ifdef _WIN32
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto ticks = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return double(ticks(k) + ticks(u)) / 10000.0;
#else
    return double(std::clock()) * 1000.0 / CLOCKS_PER_SEC;
#endif
}
void metrics(const char* name, std::vector<double> samples, double wall_ms, double cpu, unsigned operations) {
    std::sort(samples.begin(), samples.end());
    auto percentile = [&](double p) {
        return samples[std::min(samples.size() - 1, size_t(std::ceil(samples.size() * p) - 1))];
    };
    std::printf("{\"metric\":\"%s\",\"operations\":%u,\"wall_ms\":%.6f,\"cpu_ms\":%.6f,\"ops_per_sec\":%.3f,\"median_"
                "ms\":%.6f,\"p95_ms\":%.6f,\"p99_ms\":%.6f}\n",
                name, operations, wall_ms, cpu, operations * 1000.0 / wall_ms, percentile(.5), percentile(.95),
                percentile(.99));
}
template <class F> void measure(const char* name, unsigned count, F&& fn) {
    std::vector<double> samples;
    samples.reserve(count);
    const auto cpu = cpu_ms();
    const auto start = Clock::now();
    for (unsigned i = 0; i < count; ++i) {
        auto t = Clock::now();
        fn(i);
        samples.push_back(elapsed(t));
    }
    metrics(name, std::move(samples), elapsed(start), cpu_ms() - cpu, count);
}
void memory() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                 sizeof(counters)) != 0,
            "process memory query failed");
    std::printf("{\"metric\":\"process_memory\",\"peak_working_set_bytes\":%llu,\"peak_commit_bytes\":%llu,\"private_"
                "bytes\":%llu}\n",
                (unsigned long long)counters.PeakWorkingSetSize, (unsigned long long)counters.PeakPagefileUsage,
                (unsigned long long)counters.PrivateUsage);
#endif
    const auto usage = gx2vk::memory_usage();
    std::printf("{\"metric\":\"vulkan_allocations\",\"owned_bytes\":%llu,\"local_bytes\":%llu,\"host_only_bytes\":%llu,"
                "\"mapped_local_bytes\":%llu}\n",
                (unsigned long long)gx2vk::allocated_bytes(), (unsigned long long)usage.device_local,
                (unsigned long long)usage.host_only, (unsigned long long)usage.host_visible_device_local);
}
class HeapProbe {
    // Benchmark-only telemetry. Budgets are never allocation limits. Driver
    // usage is an estimate of this process's allocations, not VRAM residency;
    // on UMA, a DEVICE_LOCAL heap is shared system RAM.
    std::jthread thread;
    std::array<uint64_t, VK_MAX_MEMORY_HEAPS> peak{}, budget{};
    unsigned heaps = 0;
    uint64_t owned_peak = 0;
    void sample() {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT usage{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
        props.pNext = &usage;
        vkGetPhysicalDeviceMemoryProperties2(gx2vk::physical_device(), &props);
        heaps = props.memoryProperties.memoryHeapCount;
        for (unsigned i = 0; i < heaps; ++i) {
            peak[i] = std::max(peak[i], uint64_t(usage.heapUsage[i]));
            budget[i] = usage.heapBudget[i];
        }
    }

public:
    explicit HeapProbe(bool enabled) {
        if (!enabled) return;
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(gx2vk::physical_device(), nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> extensions(count);
        vkEnumerateDeviceExtensionProperties(gx2vk::physical_device(), nullptr, &count, extensions.data());
        const bool supported = std::any_of(extensions.begin(), extensions.end(), [](const auto& e) {
            return std::strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0;
        });
        if (!supported) return;
        sample();
        thread = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                sample();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    }
    void checkpoint() { owned_peak = std::max(owned_peak, gx2vk::allocated_bytes()); }
    void finish() {
        if (thread.joinable()) {
            thread.request_stop();
            thread.join();
            sample();
        }
        for (unsigned i = 0; i < heaps; ++i)
            std::printf(
                "{\"metric\":\"driver_heap_%u\",\"sampled_peak_usage_bytes\":%llu,\"last_budget_bytes\":%llu,\"device_"
                "local\":%u,\"sampling_ms\":5}\n",
                i, (unsigned long long)peak[i], (unsigned long long)budget[i],
                unsigned(bool(gx2vk::memory_properties().memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)));
        std::printf("{\"metric\":\"application_allocation_peak\",\"checkpoint_peak_bytes\":%llu}\n",
                    (unsigned long long)owned_peak);
    }
    ~HeapProbe() {
        if (thread.joinable()) {
            thread.request_stop();
            thread.join();
        }
    }
};
struct Source {
    VkShaderStageFlagBits stage;
    std::string text;
};
// A dependency chain, vector operations, integer bitcasts, uniform accesses,
// macros and texture fetches prevent this being a collection of empty shaders.
Source authored(unsigned index) {
    const bool pixel = index & 1;
    const unsigned size = 8u << (index % 4);
    Source result{pixel ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT,
                  "#version 450\n#define ACC(a,b) ((a)+(b))\n"};
    result.text += "layout(set=0,binding=0) uniform U{vec4 u[64];};\n";
    if (pixel)
        result.text += "layout(location=0) in vec4 pass0;layout(location=0) out vec4 color;layout(set=0,binding=1) "
                       "uniform sampler2D tex;\n";
    else
        result.text += "layout(location=0) in vec4 a;layout(location=0) out vec4 pass0;\n";
    result.text += "void main(){vec4 r=" + std::string(pixel ? "pass0" : "a") + ";\n";
    for (unsigned i = 0; i < size; ++i)
        result.text += "r=ACC(r*u[" + std::to_string((i + index) % 64) + "],vec4(" + std::to_string(index + i) +
                       ".0/1024.0));r=uintBitsToFloat(floatBitsToUint(r));\n";
    result.text += pixel ? "color=r*texture(tex,r.xy);}\n" : "gl_Position=r;pass0=r.wzyx;}\n";
    return result;
}
// One ALU clause with MOVs followed by ADDs, four lanes per group; then a
// position/pixel export. Encodings are constructed here, not extracted assets.
std::vector<uint32_t> bytecode(unsigned instructions, bool pixel, unsigned variant) {
    std::vector<uint32_t> words{2, (8u << 26) | ((instructions - 1) << 18),
                                (1u << 15) | (pixel ? 0u : (1u << 13) | 60u), (0x28u << 23) | (1u << 21) | 0x688u};
    for (unsigned i = 0; i < instructions; ++i) {
        const unsigned lane = i % 4;
        const unsigned src = i < 4 ? 249u : 1u, op = i < 4 ? 0x19u : 0u;
        words.push_back(src | (lane << 10) | ((variant & 1 ? 252u : 249u) << 13) | (lane == 3 ? 1u << 31 : 0u));
        words.push_back((op << 7) | (1u << 4) | (1u << 21) | (lane << 29));
    }
    return words;
}
Source translate(const std::vector<uint32_t>& code, bool pixel) {
    R700TranslateInput input{reinterpret_cast<const uint8_t*>(code.data()), uint32_t(code.size() * 4), pixel, 0};
    input.vulkan = true;
    std::array<uint32_t, 0x10000> registers{};
    registers[0xA08F] = 15;
    input.context_registers = registers.data();
    auto* result = r700_to_glsl(input);
    require(result != nullptr, "synthetic R700 translation failed");
    Source source{pixel ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT, result};
    std::free(result);
    return source;
}
// Original two-pass compiler is retained only in this test for output parity.
bool reference(const Source& source, std::vector<uint32_t>& spirv) {
    glslang_input_t input{};
    input.language = GLSLANG_SOURCE_GLSL;
    input.stage = source.stage == VK_SHADER_STAGE_VERTEX_BIT     ? GLSLANG_STAGE_VERTEX
                  : source.stage == VK_SHADER_STAGE_GEOMETRY_BIT ? GLSLANG_STAGE_GEOMETRY
                  : source.stage == VK_SHADER_STAGE_COMPUTE_BIT  ? GLSLANG_STAGE_COMPUTE
                                                                 : GLSLANG_STAGE_FRAGMENT;
    input.client = GLSLANG_CLIENT_VULKAN;
    input.client_version = GLSLANG_TARGET_VULKAN_1_3;
    input.target_language = GLSLANG_TARGET_SPV;
    input.target_language_version = GLSLANG_TARGET_SPV_1_6;
    input.code = source.text.c_str();
    input.default_version = 450;
    input.default_profile = GLSLANG_NO_PROFILE;
    input.messages = static_cast<glslang_messages_t>(GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT);
    input.resource = glslang_default_resource();
    auto* shader = glslang_shader_create(&input);
    bool ok = shader && glslang_shader_preprocess(shader, &input) && glslang_shader_parse(shader, &input);
    auto* program = glslang_program_create();
    if (ok) {
        glslang_program_add_shader(program, shader);
        ok = glslang_program_link(program, input.messages) != 0;
    }
    if (ok) {
        glslang_program_SPIRV_generate(program, input.stage);
        spirv.resize(glslang_program_SPIRV_get_size(program));
        glslang_program_SPIRV_get(program, spirv.data());
    }
    if (!ok) RBREW_LOG_ERROR("shader", "reference: %s", glslang_shader_get_info_log(shader));
    glslang_program_delete(program);
    glslang_shader_delete(shader);
    return ok;
}
std::vector<Source> corpus() {
    std::vector<Source> sources;
    for (unsigned i = 0; i < 48; ++i) sources.push_back(authored(i));
    for (unsigned count : {4u, 32u, 128u})
        for (bool pixel : {false, true})
            for (unsigned variant : {0u, 1u}) sources.push_back(translate(bytecode(count, pixel, variant), pixel));
    return sources;
}
void compiler_correctness() {
    // Exercise preprocessor directives which an expanded-source shortcut
    // could mishandle. Both compiler paths must emit identical SPIR-V.
    const std::array<Source, 5> directives{
        {{VK_SHADER_STAGE_VERTEX_BIT,
          "#version 430\n#define JOIN(a,b) a ## b\n#define VALUE(x) ((x)+1)\n#if VALUE(2)==3\nvoid "
          "main(){JOIN(gl_,Position)=vec4(VALUE(1));}\n#else\n#error wrong branch\n#endif\n"},
         {VK_SHADER_STAGE_FRAGMENT_BIT,
          "#version 450\n#line 100\nlayout(location=0) out vec4 c;\nvoid main(){c=vec4(__LINE__,__VERSION__,0,1);}\n"},
         {VK_SHADER_STAGE_FRAGMENT_BIT, "#version 450\n#define T int\n#define V(x) vec4(x)\n#undef T\n#define T "
                                        "float\nlayout(location=0) out vec4 c;void main(){T x=1.25;c=V(x);}\n"},
         {VK_SHADER_STAGE_GEOMETRY_BIT,
          r700_rect_geometry("out gl_PerVertex{vec4 gl_Position;};\nlayout(location = 0) flat out vec4 p;\n")},
         {VK_SHADER_STAGE_COMPUTE_BIT,
          "#version 450\n#define GROUP_SIZE 8\nlayout(local_size_x=GROUP_SIZE) in;layout(set=0,binding=0,std430) "
          "buffer B{uint values[];};void main(){values[gl_GlobalInvocationID.x]=gl_LocalInvocationIndex^0x1234u;}\n"}}};
    for (const auto& source : directives) {
        std::vector<uint32_t> old, words;
        std::string log;
        require(reference(source, old), "directive reference failed");
        require(gx2vk::compile_glsl(source.stage, source.text, words, &log) && words == old,
                "single-pass directives changed SPIR-V");
    }
    const auto name = "splport-authored-include-" + std::to_string(Clock::now().time_since_epoch().count()) + ".glsl";
    struct IncludeCleanup {
        std::string path;
        ~IncludeCleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup{name};
    {
        std::ofstream file(name);
        file << "const float authored_value=0.75;\n";
        require(bool(file), "include fixture creation failed");
    }
    Source include{VK_SHADER_STAGE_VERTEX_BIT,
                   "#version 450\n#extension GL_GOOGLE_include_directive : require\n#include \"" + name +
                       "\"\nvoid main(){gl_Position=vec4(authored_value);}\n"};
    std::vector<uint32_t> old, words;
    std::string log;
    require(reference(include, old), "include reference failed");
    require(gx2vk::compile_glsl(include.stage, include.text, words, &log) && words == old,
            "file include behavior changed");
    for (const std::string invalid :
         {"#version 450\n#error authored failure\nvoid main(){}\n", "#version 450\nvoid main(){this is invalid;}\n"}) {
        std::vector<uint32_t> words;
        std::string log;
        require(!gx2vk::compile_glsl(VK_SHADER_STAGE_VERTEX_BIT, invalid, words, &log) && !log.empty(),
                "compiler accepted invalid source or lost diagnostics");
    }
}
void cache_readers(const std::filesystem::path& directory) {
    struct Fixture {
        std::string path;
        size_t source_bytes;
        std::vector<uint32_t> expected;
    };
    std::vector<Fixture> fixtures;
    // Original reader is a test-only reference. Both readers use the same
    // already-written files in alternating order, excluding file creation,
    // hashing and antivirus scans of newly written shaders from this metric.
    auto old = [](const Fixture& fixture, std::vector<uint32_t>& words) {
        std::ifstream file(fixture.path, std::ios::binary);
        uint64_t header[2]{};
        if (!file.read(reinterpret_cast<char*>(header), 16) || header[0] != (0x52425350ull << 32 | 1) ||
            header[1] >> 32 != fixture.source_bytes)
            return false;
        words.resize(uint32_t(header[1]));
        return bool(file.read(reinterpret_cast<char*>(words.data()), std::streamsize(words.size() * 4))) &&
               !words.empty() && words[0] == 0x07230203;
    };
    for (auto& entry : std::filesystem::directory_iterator(directory / "spirv")) {
        uint64_t header[2]{};
        {
            std::ifstream file(entry.path(), std::ios::binary);
            require(bool(file.read(reinterpret_cast<char*>(header), 16)), "cache fixture header failed");
        }
        Fixture fixture{entry.path().string(), size_t(header[1] >> 32), {}};
        require(old(fixture, fixture.expected), "reference cache fixture failed");
        fixtures.push_back(std::move(fixture));
    }
    auto run = [&](bool checked) {
        measure(checked ? "cache_reader_checked" : "cache_reader_reference", unsigned(fixtures.size() * 4),
                [&](unsigned i) {
                    const auto& fixture = fixtures[i % fixtures.size()];
                    std::vector<uint32_t> words;
                    require((checked ? gx2vk::shader_test_load_disk(fixture.path, fixture.source_bytes, words)
                                     : old(fixture, words)) &&
                                words == fixture.expected,
                            "cache reader changed SPIR-V");
                });
    };
    for (unsigned round = 0; round < 7; ++round) {
        run(round % 2 == 0);
        run(round % 2 != 0);
    }
}
void compiler() {
    gx2vk::shader_set_cache_dir("");
    auto sources = corpus();
    std::vector<std::vector<uint32_t>> compiled(sources.size());
    std::string log;
    measure("compiler_first_request", 1, [&](unsigned) {
        require(gx2vk::compile_glsl(sources[0].stage, sources[0].text, compiled[0], &log), log.c_str());
    });
    measure("cold_glsl_to_spirv", unsigned(sources.size()), [&](unsigned i) {
        require(gx2vk::compile_glsl(sources[i].stage, sources[i].text, compiled[i], &log), log.c_str());
    });
    for (unsigned i = 0; i < sources.size(); ++i) {
        std::vector<uint32_t> old;
        require(reference(sources[i], old), "reference compilation failed");
        require(old == compiled[i], "compiler changed SPIR-V words");
    }
    const auto before = gx2vk::shader_compile_count();
    gx2vk::CompileQueue workers(4);
    std::vector<gx2vk::CompileQueue::Job<bool>> jobs;
    std::vector<double> latency(sources.size());
    auto start = Clock::now();
    auto cpu = cpu_ms();
    for (unsigned i = 0; i < sources.size(); ++i)
        jobs.push_back(workers.post(
            [&, i] {
                auto t = Clock::now();
                std::vector<uint32_t> words;
                std::string error;
                const bool ok =
                    gx2vk::compile_glsl(sources[i].stage, sources[i].text, words, &error) && words == compiled[i];
                latency[i] = elapsed(t);
                return ok;
            },
            true));
    for (const auto& job : jobs) require(job.result.get(), "parallel shader compilation mismatch");
    metrics("parallel_cold_glsl_to_spirv", latency, elapsed(start), cpu_ms() - cpu, unsigned(sources.size()));
    require(gx2vk::shader_compile_count() - before == sources.size(), "cold compilation unexpectedly used cache");
    // A private directory distinguishes disk hits from the cold compiler path.
    auto dir = std::filesystem::temp_directory_path() /
               ("splport-cold-bench-" + std::to_string(Clock::now().time_since_epoch().count()));
    gx2vk::shader_set_cache_dir(dir.string());
    const auto cold_compiles = gx2vk::shader_compile_count(), cold_hits = gx2vk::shader_disk_hits();
    measure("cold_compile_and_store", unsigned(sources.size()), [&](unsigned i) {
        std::vector<uint32_t> out;
        require(gx2vk::compile_glsl(sources[i].stage, sources[i].text, out, &log) && out == compiled[i],
                "stored shader changed output");
    });
    std::printf(
        "{\"metric\":\"empty_disk_cache_requests\",\"requests\":%zu,\"compilations\":%llu,\"duplicate_hits\":%llu}\n",
        sources.size(), (unsigned long long)(gx2vk::shader_compile_count() - cold_compiles),
        (unsigned long long)(gx2vk::shader_disk_hits() - cold_hits));
    auto hits = gx2vk::shader_disk_hits();
    measure("warm_spirv_disk_load", unsigned(sources.size()), [&](unsigned i) {
        std::vector<uint32_t> out;
        require(gx2vk::compile_glsl(sources[i].stage, sources[i].text, out, &log) && out == compiled[i],
                "warm shader changed output");
    });
    require(gx2vk::shader_disk_hits() - hits == sources.size(), "warm cache missed a shader");
    std::printf("{\"metric\":\"warm_disk_cache_hits\",\"requests\":%zu,\"hits\":%llu,\"hit_rate\":1.0}\n",
                sources.size(), (unsigned long long)(gx2vk::shader_disk_hits() - hits));
    cache_readers(dir);
    // A truncated file must be recompiled, without reserving its corrupt
    // claimed SPIR-V size. Exercise replacement of an existing cache too.
    const auto damaged = std::filesystem::directory_iterator(dir / "spirv")->path();
    {
        std::ofstream file(damaged, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(8);
        const uint32_t words = UINT32_MAX;
        file.write(reinterpret_cast<const char*>(&words), 4);
    }
    auto compiles = gx2vk::shader_compile_count();
    for (unsigned i = 0; i < sources.size(); ++i) {
        std::vector<uint32_t> out;
        require(gx2vk::compile_glsl(sources[i].stage, sources[i].text, out, &log) && out == compiled[i],
                "corrupt cache recovery failed");
    }
    require(gx2vk::shader_compile_count() == compiles + 1, "corrupt cache was not replaced");
    gx2vk::shader_set_cache_dir("");
    for (auto& entry : std::filesystem::directory_iterator(dir / "spirv")) std::filesystem::remove(entry.path());
    std::filesystem::remove(dir / "spirv");
    std::filesystem::remove(dir);
    compiler_correctness();
    workers.drain();
    memory();
    gx2vk::shader_shutdown();
}
void translation() {
    std::vector<std::vector<uint32_t>> programs;
    for (unsigned n : {4u, 32u, 128u})
        for (unsigned v : {0u, 1u}) programs.push_back(bytecode(n, false, v));
    uint64_t checksum = 0;
    std::vector<std::string> expected;
    for (unsigned i = 0; i < programs.size(); ++i) expected.push_back(translate(programs[i], false).text);
    R700TranslateInput zero_registers{reinterpret_cast<const uint8_t*>(programs[0].data()),
                                      uint32_t(programs[0].size() * 4), false, 0};
    auto* without_registers = r700_to_glsl(zero_registers);
    require(without_registers && expected[0] == without_registers, "absent register snapshot was not zeroed");
    std::free(without_registers);
    measure("gx2_to_glsl", 240, [&](unsigned i) {
        auto source = translate(programs[i % programs.size()], false);
        require(source.text == expected[i % programs.size()], "GX2 translation is nondeterministic");
        checksum += gx2vk::stable_hash(source.text.data(), source.text.size());
    });
    std::string vs = "#version 430\nout gl_PerVertex{vec4 gl_Position;};\n";
    for (unsigned i = 0; i < 32; ++i)
        vs += "layout(location = " + std::to_string(i) + ") " +
              (i % 3 == 0   ? "flat "
               : i % 3 == 1 ? "noperspective "
                            : "") +
              "out vec4 varying" + std::to_string(i) + ";\n";
    vs += "void main(){}\n";
    const auto rect = r700_rect_geometry(vs.c_str());
    measure("rectangle_geometry_generation", 160, [&](unsigned) {
        auto source = r700_rect_geometry(vs.c_str());
        require(source == rect, "rectangle geometry output changed");
        checksum += source.size();
    });
    std::printf("{\"metric\":\"translation_output\",\"checksum\":%llu}\n", (unsigned long long)checksum);
    memory();
    // Tolerated whitespace, repeated qualifiers, malformed declarations,
    // comment matches and empty/point outputs retain the exact old spelling.
    for (const std::string spacing : {" ", "\t", "\r\n", "\f\v"})
        for (const std::string qualifier : {"", "flat ", "noperspective ", "flat noperspective ", "flatflat "})
            for (bool quad : {false, true}) {
                auto text = vs + "\nlayout" + spacing + "(location" + spacing + "=" + spacing + "003)" + spacing +
                            qualifier + "out" + spacing + "vec4" + spacing + "_extra9 ;\n";
                text += "layout (location = 1) flat out vec3 wrong; layout(location = x) out vec4 wrong; out "
                        "gl_PerVertex{vec4 gl_Position;float gl_PointSize;};";
                require(r700_rect_geometry(text.c_str(), quad) == reference_rect_geometry(text.c_str(), quad),
                        "geometry scanner differs from regex oracle");
            }
    require(r700_rect_geometry(nullptr) == reference_rect_geometry(nullptr, false), "empty geometry scanner mismatch");
    auto check_format = [](std::string_view f, const auto&... args) {
        StringBuf buffer(32);
        buffer.add("prefix");
        buffer.addFmt(f, args...);
        require(std::string(buffer.c_str()) == "prefix" + shader_format(f, args...),
                "emitter formatting changed output");
    };
    for (int value : {0, 1, -1, INT_MIN, INT_MAX}) {
        check_format("R{}", value);
        check_format("[{}]", value);
        check_format("{:08x}", uint32_t(value));
        check_format("{{{}}}", value);
        check_format("{} + {}", value, value);
    }
    check_format("{}", UINT64_MAX);
    check_format("{}", INT64_MIN);
    check_format("{}", true);
    check_format("{}", 'x');
    check_format("name{}suffix", "hello");
    check_format("{}", std::string(128, 'a'));
    check_format("literal{{}}text");
}
void scheduling() {
    for (unsigned count : {2000u, 20000u}) {
        gx2vk::CompileQueue queue(1);
        std::promise<void> entered, release;
        auto gate = release.get_future().share();
        auto blocker = queue.post(
            [&] {
                entered.set_value();
                gate.wait();
                return 0u;
            },
            true);
        entered.get_future().wait();
        std::atomic<unsigned> completed = 0;
        std::vector<gx2vk::CompileQueue::Job<unsigned>> jobs;
        jobs.reserve(count);
        measure(count == 2000 ? "post_2000_pending" : "post_20000_pending", count, [&](unsigned i) {
            jobs.push_back(queue.post(
                [&, i] {
                    ++completed;
                    return i;
                },
                true));
        });
        // Ready jobs must not scan the still-blocked background queue. This
        // models a foreground owner resolving already completed shader work.
        std::promise<unsigned> ready;
        auto f = ready.get_future().share();
        ready.set_value(7);
        gx2vk::CompileQueue::Job<unsigned> done{f, {}};
        measure(count == 2000 ? "ready_resolve_2000_pending" : "ready_resolve_20000_pending", 1000,
                [&](unsigned) { require(queue.get(done) == 7, "completed job result changed"); });
        release.set_value();
        queue.drain();
        for (unsigned i = 0; i < count; ++i) require(queue.get(jobs[i]) == i, "compiler job result changed");
        require(completed == count, "compiler queue lost work");
    }
    gx2vk::CompileQueue queue(4);
    std::vector<gx2vk::CompileQueue::Job<void>> jobs;
    std::atomic<unsigned> n = 0;
    auto start = Clock::now();
    auto cpu = cpu_ms();
    for (unsigned i = 0; i < 50000; ++i) jobs.push_back(queue.post([&] { ++n; }, true));
    for (auto& job : jobs) job.result.get();
    metrics("worker_50000_tasks", {elapsed(start)}, elapsed(start), cpu_ms() - cpu, 50000);
    require(n == 50000, "worker lost work");
    auto failure = queue.post([]() -> unsigned { throw std::runtime_error("authored failure"); });
    bool caught = false;
    try {
        queue.get(failure);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    require(caught, "queue swallowed an exception");
    queue.drain();
    memory();
    // Concurrent consumers race dequeue/promotion and observe the same jobs.
    gx2vk::CompileQueue racing(4);
    std::atomic<bool> release = false;
    std::array<gx2vk::CompileQueue::Job<void>, 4> gates;
    for (auto& gate : gates) gate = racing.post([&] { release.wait(false); }, true);
    std::array<std::atomic<unsigned>, 4096> visits{};
    std::vector<gx2vk::CompileQueue::Job<unsigned>> pending;
    for (unsigned i = 0; i < visits.size(); ++i)
        pending.push_back(racing.post(
            [&, i] {
                ++visits[i];
                return i;
            },
            true));
    std::array<std::thread, 4> consumers;
    for (unsigned t = 0; t < consumers.size(); ++t)
        consumers[t] = std::thread([&, t] {
            for (unsigned i = 0; i < pending.size(); ++i) {
                unsigned at = (i * 4093 + t * 97) % pending.size();
                require(racing.get(pending[at]) == at, "promoted job changed value");
            }
        });
    release = true;
    release.notify_all();
    for (auto& consumer : consumers) consumer.join();
    racing.drain();
    for (auto& value : visits) require(value == 1, "promoted job executed more than once");
    gx2vk::CompileQueue types(1);
    unsigned referent = 42;
    auto ref = types.post([&]() -> unsigned& { return referent; });
    require(&types.get(ref) == &referent, "reference future lost identity");
    auto move = types.post([owned = std::make_unique<unsigned>(77)] { return *owned; });
    require(types.get(move) == 77, "move-only callable failed");
    types.drain();
    bool rejected = false;
    try {
        types.post([] {});
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "stopped compiler accepted work");
}
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void authored_host_function(CPUState* cpu) {
    ++cpu->r[3];
    cpu->r[4] = (cpu->r[4] * 1664525u) + 1013904223u;
}
void engine() {
    // Exercise the production flat native dispatch table with authored host
    // functions only. No guest RAM, game functions, loader or entry point.
    CPUState cpu{};
    std::array<HostFunc, 1024> table;
    table.fill(&authored_host_function);
    cpu.dispatch_table = table.data();
    cpu.dispatch_size = uint32_t(table.size());
    measure("direct_native_batches_100000", 64, [&](unsigned) {
        for (unsigned i = 0; i < 100000; ++i) authored_host_function(&cpu);
    });
    require(cpu.r[3] == 6400000, "direct call count mismatch");
    auto expected = cpu.r[4];
    cpu.r[3] = cpu.r[4] = 0;
    measure("recompiled_dispatch_batches_100000", 64, [&](unsigned) {
        for (unsigned i = 0; i < 100000; ++i) rbrew_dispatch(&cpu, (i % table.size()) * 4);
    });
    require(cpu.r[3] == 6400000 && cpu.r[4] == expected, "native dispatch changed ABI state");
    memory();
}
void gpu(bool telemetry = false) {
    if (!gx2vk::init(nullptr)) throw std::runtime_error("compatible Vulkan device unavailable");
    std::printf("Vulkan device: %s\n", gx2vk::caps().device_name.c_str());
    gx2vk::shader_set_cache_dir("");
    HeapProbe heap_probe(telemetry);
    const std::string vertex = "#version 450\nvoid main(){vec2 "
                               "p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);gl_Position=vec4(p*2.-1.,0.,1.);}\n";
    std::vector<VkShaderModule> fragments(32);
    auto vs = gx2vk::shader_module(VK_SHADER_STAGE_VERTEX_BIT, vertex);
    require(vs != VK_NULL_HANDLE, "synthetic vertex compilation failed");
    measure("cold_fragment_modules", unsigned(fragments.size()), [&](unsigned i) {
        auto source = translate(bytecode(4 + 4 * i, true, i), true);
        fragments[i] = gx2vk::shader_module(VK_SHADER_STAGE_FRAGMENT_BIT, source.text);
        require(fragments[i] != VK_NULL_HANDLE, "synthetic fragment module failed");
    });
    auto count = gx2vk::shader_compile_count();
    measure("warm_fragment_modules", unsigned(fragments.size()), [&](unsigned i) {
        auto source = translate(bytecode(4 + 4 * i, true, i), true);
        require(gx2vk::shader_module(VK_SHADER_STAGE_FRAGMENT_BIT, source.text) == fragments[i],
                "warm module mismatch");
    });
    require(count == gx2vk::shader_compile_count(), "warm module recompiled");
    VkPipelineLayout layout{};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    require(vkCreatePipelineLayout(gx2vk::device(), &li, nullptr, &layout) == VK_SUCCESS, "pipeline layout failed");
    VkPipelineCache cache{};
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    require(vkCreatePipelineCache(gx2vk::device(), &ci, nullptr, &cache) == VK_SUCCESS, "fresh pipeline cache failed");
    VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                              {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.lineWidth = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bs.attachmentCount = 1;
    bs.pAttachments = &ba;
    VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = states;
    const VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.layout = layout;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pColorBlendState = &bs;
    info.pDynamicState = &ds;
    std::vector<VkPipeline> pipelines(fragments.size());
    for (const char* phase : {"cold_graphics_pipeline_fresh_app_cache", "warm_graphics_pipeline_same_app_cache"}) {
        measure(phase, unsigned(fragments.size()), [&](unsigned i) {
            stages[1].module = fragments[i];
            VkPipeline p{};
            require(vkCreateGraphicsPipelines(gx2vk::device(), cache, 1, &info, nullptr, &p) == VK_SUCCESS,
                    "synthetic graphics pipeline failed");
            if (pipelines[i])
                vkDestroyPipeline(gx2vk::device(), p, nullptr);
            else
                pipelines[i] = p;
        });
    }
    heap_probe.checkpoint();
    gx2vk::Image image;
    gx2vk::ImageDesc desc;
    desc.width = desc.height = 8;
    desc.format = format;
    desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    require(gx2vk::create_image(image, desc), "test image failed");
    auto view = gx2vk::image_view(image, {});
    require(view != VK_NULL_HANDLE, "test image view failed");
    // Real GPU execution of every translated fragment shader. The expected
    // output is the exact representable sum in the authored R700 ALU chain.
    for (unsigned i = 0; i < pipelines.size(); ++i) {
        gx2vk::barrier_if_dirty();
        auto cmd = gx2vk::cmd();
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = view;
        color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea.extent = {8, 8};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[i]);
        VkViewport viewport{0, 0, 8, 8, 0, 1};
        VkRect2D scissor{{0, 0}, {8, 8}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        gx2vk::note_gpu_write(image);
        std::vector<uint8_t> bytes;
        require(gx2vk::read_image(image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 8, 8, bytes),
                "translated shader readback failed");
        const float expected = 1.0f + i * (i & 1 ? .5f : 1.0f);
        require(bytes.size() == 8 * 8 * 16, "shader output size mismatch");
        for (size_t at = 0; at < bytes.size(); at += 4) {
            float actual;
            std::memcpy(&actual, bytes.data() + at, 4);
            require(actual == expected, "translated shader produced incorrect GPU pixels");
        }
    }
    measure("draw_recording_batches_256", 32, [&](unsigned i) {
        auto cmd = gx2vk::cmd();
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = view;
        color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        gx2vk::barrier_if_dirty();
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea.extent = {8, 8};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[i]);
        VkViewport viewport{0, 0, 8, 8, 0, 1};
        VkRect2D scissor{{0, 0}, {8, 8}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        for (unsigned n = 0; n < 256; ++n) vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        gx2vk::note_gpu_write(image);
    });
    measure("submit_and_wait", 1, [&](unsigned) { require(gx2vk::finish(), "GPU finish failed"); });
    for (auto use : {gx2vk::MemoryUse::DeviceLocal, gx2vk::MemoryUse::Upload, gx2vk::MemoryUse::Readback}) {
        std::vector<gx2vk::Buffer> buffers(256);
        const char* name = use == gx2vk::MemoryUse::DeviceLocal ? "local_buffer_allocation"
                           : use == gx2vk::MemoryUse::Upload    ? "upload_buffer_allocation"
                                                                : "readback_buffer_allocation";
        measure(name, unsigned(buffers.size()), [&](unsigned i) {
            buffers[i] =
                gx2vk::create_buffer(65536, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, use);
            require(buffers[i].buffer != VK_NULL_HANDLE, "allocation stress failed");
        });
        heap_probe.checkpoint();
        for (auto& buffer : buffers) {
            vkDestroyBuffer(gx2vk::device(), buffer.buffer, nullptr);
            gx2vk::free_now(buffer.memory);
        }
    }
    auto streaming = gx2vk::create_buffer(256u * 65536, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gx2vk::MemoryUse::Readback);
    require(streaming.buffer && streaming.memory.mapped, "streaming destination failed");
    measure("upload_ring_record_64KiB", 256, [&](unsigned i) {
        auto span = gx2vk::upload(65536, 16);
        require(span.data != nullptr, "upload ring failed");
        std::memset(span.data, int(i), 65536);
        VkBufferCopy copy{span.offset, VkDeviceSize(i) * 65536, 65536};
        vkCmdCopyBuffer(gx2vk::cmd(), span.buffer, streaming.buffer, 1, &copy);
    });
    VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &host;
    vkCmdPipelineBarrier2(gx2vk::cmd(), &dependency);
    measure("upload_submit_and_wait", 1, [&](unsigned) { require(gx2vk::finish(), "streaming transfer failed"); });
    require(gx2vk::invalidate_mapped(streaming.memory), "streaming readback invalidation failed");
    for (size_t i = 0; i < 256u * 65536; ++i)
        require(streaming.memory.mapped[i] == uint8_t(i / 65536), "streaming transfer changed bytes");
    heap_probe.checkpoint();
    gx2vk::destroy_buffer(streaming);
    heap_probe.checkpoint();
    heap_probe.finish();
    memory();
    gx2vk::destroy_image(image);
    gx2vk::finish();
    for (auto p : pipelines) vkDestroyPipeline(gx2vk::device(), p, nullptr);
    vkDestroyPipelineCache(gx2vk::device(), cache, nullptr);
    vkDestroyPipelineLayout(gx2vk::device(), layout, nullptr);
    require(gx2vk::validation_error_count() == 0, "Vulkan validation error");
    gx2vk::shader_shutdown();
    gx2vk::shutdown();
}
} // namespace
int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected compiler, translation, scheduling, engine, gpu, or gpu_memory");
        const std::string mode = argv[1];
        if (mode == "compiler")
            compiler();
        else if (mode == "translation")
            translation();
        else if (mode == "scheduling")
            scheduling();
        else if (mode == "engine")
            engine();
        else if (mode == "gpu")
            gpu();
        else if (mode == "gpu_memory")
            gpu(true);
        else
            throw std::runtime_error("unknown benchmark");
        return 0;
    } catch (const std::exception& e) {
        RBREW_LOG_ERROR("test", "FAIL: %s", e.what());
        return 1;
    }
}

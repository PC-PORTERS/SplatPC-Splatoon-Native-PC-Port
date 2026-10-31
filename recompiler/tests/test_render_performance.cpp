// Only the runtime and authored synthetic inputs are linked. No game entry point.
#include "os/gx2/gx2_render.h"
#include "os/gx2/vk/gx2_vk.h"
#include "os/gx2/vk/gx2_imgui_overlay.h"
#include "runtime/rebrewu_runtime.h"
#include "rebrewu/logger.hpp"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <thread>
#include <cmath>
#include <string>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <optional>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
static LONG WINAPI benchmark_exception(EXCEPTION_POINTERS* e) {
    HMODULE module = nullptr;
    char path[MAX_PATH]{};
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<const char*>(e->ExceptionRecord->ExceptionAddress), &module);
    GetModuleFileNameA(module, path, MAX_PATH);
    RBREW_LOG_ERROR("render", "standalone exception %08lx in %s at module offset %llu",
                    e->ExceptionRecord->ExceptionCode, path,
                    (unsigned long long)(uintptr_t(e->ExceptionRecord->ExceptionAddress) - uintptr_t(module)));
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif
#ifdef RBREW_BENCH_SAMPLER
// Profiling copy only: samples the benchmark thread's instruction pointer
// every ~50 us and prints the hottest functions from the PDB at exit.
#include <dbghelp.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>
struct BenchSampler {
    std::atomic<bool> stop{false};
    std::vector<DWORD64> samples, callers;
    std::thread thread;
    HANDLE target = nullptr;
    DWORD64 exe_begin = 0, exe_end = 0;
    BenchSampler() {
        const char* on = std::getenv("RBREW_BENCH_SAMPLE");
        if (!on || on[0] != '1') return;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target, THREAD_ALL_ACCESS, FALSE,
                        0);
        samples.reserve(1 << 22);
        MODULEINFO info{};
        GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(nullptr), &info, sizeof(info));
        exe_begin = reinterpret_cast<DWORD64>(info.lpBaseOfDll);
        exe_end = exe_begin + info.SizeOfImage;
        thread = std::thread([this] {
            LARGE_INTEGER f, t0, t;
            QueryPerformanceFrequency(&f);
            while (!stop.load(std::memory_order_relaxed)) {
                QueryPerformanceCounter(&t0);
                do QueryPerformanceCounter(&t);
                while ((t.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart < 50);
                if (SuspendThread(target) == DWORD(-1)) break;
                CONTEXT c{};
                c.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(target, &c)) {
                    samples.push_back(c.Rip);
                    // One unwound frame attributes shared helpers (memcmp, hashing) to callers.
                    // System waits unwind further, to the first frame in this executable.
                    DWORD64 caller = 0;
                    for (unsigned depth = 0; depth < 12; ++depth) {
                        DWORD64 base = 0;
                        if (auto* fn = RtlLookupFunctionEntry(c.Rip, &base, nullptr)) {
                            void* handler = nullptr;
                            DWORD64 frame = 0;
                            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fn, &c, &handler, &frame, nullptr);
                        } else {
                            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
                            c.Rsp += 8;
                        }
                        caller = c.Rip;
                        if (!caller || (caller >= exe_begin && caller < exe_end)) break;
                    }
                    callers.push_back(caller);
                }
                ResumeThread(target);
            }
        });
    }
    ~BenchSampler() {
        if (!thread.joinable()) return;
        stop = true;
        thread.join();
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(process, nullptr, TRUE);
        std::map<std::string, size_t> by_function;
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
        for (DWORD64 rip : samples) {
            auto* s = reinterpret_cast<SYMBOL_INFO*>(buffer);
            s->SizeOfStruct = sizeof(SYMBOL_INFO);
            s->MaxNameLen = 511;
            DWORD64 off = 0;
            std::string name = SymFromAddr(process, rip, &off, s) ? std::string(s->Name) : "?";
            if (name == "?") {
                HMODULE m = nullptr;
                char path[MAX_PATH]{};
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCSTR>(rip), &m))
                    GetModuleFileNameA(m, path, MAX_PATH);
                name = std::string("? ") + path;
            }
            ++by_function[name];
        }
        // RBREW_BENCH_SAMPLE_LINES=<substring>: also rank source lines of matching functions.
        if (const char* match = std::getenv("RBREW_BENCH_SAMPLE_LINES")) {
            std::map<std::string, size_t> by_line;
            const bool by_caller = std::getenv("RBREW_BENCH_SAMPLE_CALLERS") != nullptr;
            for (size_t k = 0; k < samples.size(); ++k) {
                DWORD64 rip = samples[k];
                auto* s = reinterpret_cast<SYMBOL_INFO*>(buffer);
                s->SizeOfStruct = sizeof(SYMBOL_INFO);
                s->MaxNameLen = 511;
                DWORD64 off = 0;
                if (!SymFromAddr(process, rip, &off, s) || !std::strstr(s->Name, match)) continue;
                if (by_caller) {
                    rip = callers[k];
                    if (!SymFromAddr(process, rip, &off, s)) continue;
                }
                IMAGEHLP_LINE64 line{};
                line.SizeOfStruct = sizeof(line);
                DWORD disp = 0;
                if (SymGetLineFromAddr64(process, rip, &disp, &line))
                    ++by_line[std::string(s->Name).substr(0, 40) + " " + std::strrchr(line.FileName, '\\') + ":" +
                              std::to_string(line.LineNumber)];
            }
            std::vector<std::pair<size_t, std::string>> lines;
            for (auto& [n, c] : by_line) lines.push_back({c, n});
            std::sort(lines.rbegin(), lines.rend());
            for (size_t i = 0; i < lines.size() && i < 40; ++i)
                RBREW_LOG_DEBUG("sampler-line", "%6.2f%% %s", 100.0 * lines[i].first / samples.size(),
                                lines[i].second.c_str());
        }
        std::vector<std::pair<size_t, std::string>> top;
        for (auto& [n, c] : by_function) top.push_back({c, n});
        std::sort(top.rbegin(), top.rend());
        RBREW_LOG_DEBUG("sampler", "%zu samples", samples.size());
        for (size_t i = 0; i < top.size() && i < 45; ++i)
            RBREW_LOG_DEBUG("sampler", "%6.2f%% %s", 100.0 * top[i].first / samples.size(), top[i].second.c_str());
        SymCleanup(process);
        CloseHandle(target);
    }
};
#endif
void gx2_render_vk_test_stress(bool push, bool churn);
void gx2_render_vk_test_binding_contracts();
void gx2_render_vk_test_lifetime();
void gx2_render_vk_test_heavy(bool overdraw, bool push);
void gx2_render_vk_test_watch_ranges();
void gx2_render_vk_test_vertex_plans();
void gx2_render_vk_test_packet_batches();
void gx2_render_vk_test_plaza();
void gx2_render_vk_test_npc(unsigned characters, bool push);
void player_anim_register();
void player_anim_test_advance_idle(double seconds);
uint32_t player_anim_test_find_skeletal(CPUState* cpu, uint32_t controller, const char* name);
// Idle look-around hooks driven through the real dispatch path. Authored
// stand-ins play the game functions at their v16 addresses: the motion
// constructor registers WaitHold_Nrml in slot 57, the update writes a
// scripted current slot. Registration models the game's append semantics and
// parallel handle arrays; playback resolves the selected skeletal handle.
namespace idle_test {
constexpr uint32_t kCtor = 0x026227C8u, kUpdate = 0x0262304Cu, kRegister = 0x025CFB18u, kPlay = 0x025D06A4u;
constexpr uint32_t kMotion = 0x00600000u, kNames = 0x00620000u, kStorage = 0x00700000u, kStack = 0x00500000u;
struct Call {
    uint32_t function, controller, slot;
    std::string name;
    double blend;
};
std::vector<Call> calls;
uint32_t controller = 0x00610000u, current = 0xFFFFFFFFu;
bool missing = false;
bool dictionary_resources = true, dictionary_normal = true;
constexpr uint32_t kArrays = 0x00640000u, kHeap = 0x00650000u;
uint32_t heap = kHeap, active_clip = 0;
unsigned registrations = 0;
uint32_t entry(CPUState* c, unsigned field, unsigned slot = 57) {
    return rbrew_read32(c->mem, controller + field) + slot * 4;
}
std::string text(CPUState* c, uint32_t a) {
    return reinterpret_cast<const char*>(c->mem + a);
}
void wrapped(CPUState* c, uint32_t address, HostFunc original) {
    if (const RbrewGuestHook hook = rbrew_guest_hook(address))
        hook(c, address, original);
    else
        original(c);
}
void reg(CPUState* c) {
    calls.push_back({kRegister, c->r[3], c->r[4], text(c, c->r[5]), 0});
    ++registrations;
    // v16's single-clip registration allocates one handle, then appends at
    // the existing count. Re-registering a populated slot writes out of bounds.
    const uint32_t count = rbrew_read32(c->mem, entry(c, 0x10));
    if (count) throw std::runtime_error("registration appended beyond its one-clip allocation");
    const bool normal = text(c, c->r[5]) == "WaitHold_Nrml";
    if (!normal) throw std::runtime_error("idle hook called the constructor-only registrar");
    const uint32_t clip = 0x42u;
    for (unsigned field : {0xCu, 0x14u, 0x18u, 0x1Cu}) {
        rbrew_write32(c->mem, entry(c, field), heap);
        rbrew_write32(c->mem, heap, field == 0xC ? clip : 0xFFFFFFFFu);
        heap += 4;
    }
    rbrew_write32(c->mem, entry(c, 0x10), 1);
    // The return register is the LAST auxiliary lookup, not the skeletal clip.
    c->r[3] = 0xFFFFFFFFu;
    c->r[4] = 0xBAD;
    c->f[1] = -1;
    c->cr[0] = 7;
}
void play(CPUState* c) {
    calls.push_back({kPlay, c->r[3], c->r[4], "", c->f[1]});
    active_clip = rbrew_read32(c->mem, rbrew_read32(c->mem, entry(c, 0xC)));
    c->r[3] = 0xBAD;
    c->r[5] = 0xBAD;
}
void reg_entry(CPUState* c) {
    wrapped(c, kRegister, reg);
}
void play_entry(CPUState* c) {
    wrapped(c, kPlay, play);
}
void ctor(CPUState* c) {
    rbrew_write32(c->mem, kMotion + 0x20, 0xFFFFFFFFu);
    rbrew_write32(c->mem, kMotion + 0x24, 0xFFFFFFFFu);
    rbrew_write32(c->mem, kMotion + 0x28, controller);
    for (unsigned i = 0; i < 6; ++i) {
        rbrew_write32(c->mem, controller + 0xC + i * 4, kArrays + i * 0x400);
        rbrew_write32(c->mem, kArrays + i * 0x400 + 56 * 4, 0xABCD0000u + i);
        rbrew_write32(c->mem, kArrays + i * 0x400 + 57 * 4, 0);
        rbrew_write32(c->mem, kArrays + i * 0x400 + 58 * 4, 0xDCBA0000u + i);
    }
    rbrew_write32(c->mem, controller + 0x98, 3);
    rbrew_write32(c->mem, controller + 0x90, 0);
    if (dictionary_resources) {
        constexpr uint32_t model = 0x660000, resources = 0x660200, files = 0x660400;
        rbrew_write32(c->mem, controller + 0x90, model);
        rbrew_write32(c->mem, model + 0xA4, resources);
        rbrew_write32(c->mem, resources + 0x74, 5);
        rbrew_write32(c->mem, resources + 0x7C, files);
        for (unsigned i = 0; i < 5; ++i) {
            const uint32_t wrapper = 0x660500 + i * 0x20, fres = 0x661000 + i * 0x100;
            rbrew_write32(c->mem, files + i * 4, wrapper);
            rbrew_write32(c->mem, wrapper + 0x1C, fres);
            rbrew_write32(c->mem, fres, 0x46524553);
            rbrew_write16(c->mem, fres + 0x54, i == 0 ? 66 : i == 4 ? 2 : 0);
            const uint32_t dict = i == 0 ? 0x662000 : i == 4 ? 0x663000 : 0;
            rbrew_write32(c->mem, fres + 0x28, dict ? dict - (fres + 0x28) : 0);
            if (dict) rbrew_write32(c->mem, dict + 4, i == 0 ? 66 : 2);
        }
        std::memcpy(c->mem + 0x664000, "WaitHold_Nrml_LookAround", 24);
        for (unsigned i = 0; i < 2; ++i) {
            const uint32_t node = 0x663000 + 24 + i * 16, data = 0x665000 + i * 0x40;
            rbrew_write32(c->mem, node + 8,
                          (i == 0 ? (dictionary_normal ? kNames : 0x664080) : (missing ? 0x664080 : 0x664000)) -
                              (node + 8));
            rbrew_write32(c->mem, node + 12, data - (node + 12));
            rbrew_write32(c->mem, data, 0x46534B41);
        }
    }
    c->r[3] = controller;
    c->r[4] = 57;
    c->r[5] = kNames;
    c->r[6] = c->r[8] = 0;
    c->r[7] = 1;
    c->lr = 0x0262297Cu;
    reg_entry(c);
    c->r[3] = kMotion;
}
void update(CPUState* c) {
    rbrew_write32(c->mem, kMotion + 0x20, current);
    c->r[3] = 0x1234;
    c->r[14] = 0x5678;
}
} // namespace idle_test
static void idle_lookaround_test() {
    using namespace idle_test;
    auto require = [](bool ok, const char* text) {
        if (!ok) throw std::runtime_error(text);
    };
    std::vector<uint8_t> memory(8u << 20);
    std::vector<HostFunc> table((kCtor >> 2) + 1);
    CPUState cpu{};
    cpu.mem = memory.data();
    cpu.dispatch_table = table.data();
    cpu.dispatch_size = uint32_t(table.size());
    cpu.r[1] = kStack;
    table[kRegister >> 2] = reg_entry;
    table[kPlay >> 2] = play_entry;
    std::memcpy(cpu.mem + kNames, "WaitHold_Nrml", 14);
    player_anim_register();
    rbrew_write32(cpu.mem, 0, 0x13579BDFu);
    auto construct = [&] {
        wrapped(&cpu, kCtor, ctor);
        require(cpu.r[3] == kMotion, "constructor result changed");
    };
    auto snapshot = [&] {
        std::vector<uint32_t> result;
        for (unsigned field : {0xCu, 0x10u, 0x14u, 0x18u, 0x1Cu, 0x20u})
            result.push_back(rbrew_read32(cpu.mem, entry(&cpu, field)));
        return result;
    };
    auto frame = [&](uint32_t slot, uint32_t override_slot = 0xFFFFFFFFu) {
        current = slot;
        rbrew_write32(cpu.mem, kMotion + 0x24, override_slot);
        cpu.r[3] = kMotion;
        cpu.r[5] = 0x55;
        cpu.f[1] = 3;
        cpu.lr = 0x4242;
        wrapped(&cpu, kUpdate, update);
        require(cpu.r[3] == 0x1234 && cpu.r[14] == 0x5678 && cpu.r[5] == 0x55 && cpu.f[1] == 3 && cpu.lr == 0x4242,
                "hook changed the caller's registers");
    };
    auto wait_idle = [&] {
        frame(57);
        player_anim_test_advance_idle(5.01);
        frame(57);
    };
    construct();
    const auto normal = snapshot();
    calls.clear();
    for (unsigned i = 0; i < 4; ++i) frame(57);
    player_anim_test_advance_idle(4.9);
    frame(57);
    require(calls.empty(), "look-around started before the idle delay");
    player_anim_test_advance_idle(0.2);
    frame(57);
    require(calls.size() == 1 && calls[0].function == kPlay && calls[0].controller == controller &&
                calls[0].slot == 57 && calls[0].blend == 10,
            "idle look-around was not played without re-registering slot 57");
    require(active_clip == 0x43u, "playback did not resolve the look-around skeletal clip");
    require(snapshot() == normal, "look-around replaced game-owned allocations or auxiliary tracks");
    require(rbrew_read32(cpu.mem, entry(&cpu, 0x10)) == 1, "look-around changed the slot's variation count");
    for (unsigned i = 0; i < 6; ++i)
        require(rbrew_read32(cpu.mem, kArrays + i * 0x400 + 56 * 4) == 0xABCD0000u + i &&
                    rbrew_read32(cpu.mem, kArrays + i * 0x400 + 58 * 4) == 0xDCBA0000u + i,
                "look-around damaged a neighboring slot");
    calls.clear();
    for (unsigned i = 0; i < 329; ++i) frame(57);
    require(calls.empty(), "look-around ended before 330 frames");
    frame(57);
    require(calls.size() == 1 && calls[0].function == kPlay && calls[0].blend == 10 && snapshot() == normal &&
                active_clip == 0x42u,
            "WaitHold_Nrml was not restored after the look-around");
    calls.clear();
    wait_idle();
    require(calls.empty(), "look-around repeated within one idle period");
    frame(78);
    require(calls.empty(), "leaving idle after the look-around touched the controller");
    const uint32_t cached_heap = heap;
    const unsigned cached_registrations = registrations;
    for (uint32_t next : {73u, 78u, 103u, 116u, 57u}) {
        wait_idle();
        require(calls.size() == 1 && calls[0].function == kPlay && active_clip == 0x43u,
                "a new idle period did not reuse the look-around registration");
        calls.clear();
        frame(next, next == 57 ? 0 : 0xFFFFFFFFu);
        require(calls.empty() && snapshot() == normal,
                "movement, shooting, squid form or an override did not restore the slot without restarting playback");
    }
    require(heap == cached_heap && registrations == cached_registrations,
            "repeated idles allocated or registered more handles");
    frame(78);
    frame(57, 0);
    player_anim_test_advance_idle(10);
    frame(57, 0);
    require(calls.empty(), "look-around interrupted an active override");
    frame(57);
    player_anim_test_advance_idle(4.9);
    frame(57);
    require(calls.empty(), "override time counted toward the idle delay");
    calls.clear();
    controller = 0x00630000u;
    missing = true;
    construct();
    const auto missing_normal = snapshot();
    calls.clear();
    frame(78);
    wait_idle();
    require(calls.empty() && snapshot() == missing_normal &&
                rbrew_read32(cpu.mem, rbrew_read32(cpu.mem, entry(&cpu, 0xC))) == 0x42u,
            "a model without the clip did not keep its original animation");
    calls.clear();
    frame(78);
    wait_idle();
    require(calls.empty(), "an unavailable clip was retried");
    // A new motion at a reused controller address must discard the old cache.
    missing = false;
    construct();
    calls.clear();
    frame(78);
    wait_idle();
    require(calls.size() == 1 && calls[0].function == kPlay && active_clip == 0x43u,
            "reusing a controller address kept an unavailable or stale registration");
    calls.clear();
    frame(103);
    require(snapshot()[1] == 1 && rbrew_read32(cpu.mem, snapshot()[0]) == 0x42u,
            "dictionary fallback did not restore the original slot");
    dictionary_normal = false;
    construct();
    const auto mismatched_normal = snapshot();
    calls.clear();
    frame(78);
    wait_idle();
    require(calls.empty() && snapshot() == mismatched_normal,
            "dictionary fallback accepted an unverified handle convention");
    require(rbrew_read32(cpu.mem, 0) == 0x13579BDFu, "idle hook wrote through a null guest allocation");
    std::printf("{\"metric\":\"idle_lookaround\",\"passed\":true}\n");
}
// Optional private check against the owner's extracted BFRES. No assets are
// linked, bundled or required by the ordinary authored regression tests.
static void idle_asset_lookup_test(const char* path, uint32_t normal, uint32_t alternate) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open owned animation resource");
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>{});
    if (data.size() < 0x58 || data.size() > 8u << 20) throw std::runtime_error("unexpected BFRES size");
    std::vector<uint8_t> memory(12u << 20);
    CPUState cpu{};
    cpu.mem = memory.data();
    std::memcpy(cpu.mem + 0x1000, data.data(), data.size());
    constexpr uint32_t controller = 0x900800, model = 0x900000, resources = 0x900200, files = 0x900400;
    rbrew_write32(cpu.mem, controller + 0x90, model);
    rbrew_write32(cpu.mem, model + 0xA4, resources);
    rbrew_write32(cpu.mem, resources + 0x74, 5);
    rbrew_write32(cpu.mem, resources + 0x7C, files);
    for (unsigned i = 0; i < 5; ++i) {
        const uint32_t wrapper = 0x900500 + i * 0x20, fres = i == 4 ? 0x1000 : 0x901000 + i * 0x100;
        rbrew_write32(cpu.mem, files + i * 4, wrapper);
        rbrew_write32(cpu.mem, wrapper + 0x1C, fres);
        if (i != 4) rbrew_write32(cpu.mem, fres, 0x46524553);
    }
    if (player_anim_test_find_skeletal(&cpu, controller, "WaitHold_Nrml") != normal ||
        player_anim_test_find_skeletal(&cpu, controller, "WaitHold_Nrml_LookAround") != alternate ||
        player_anim_test_find_skeletal(&cpu, controller, "Missing_Authored_Animation") != 0xFFFFFFFFu)
        throw std::runtime_error("owned BFRES lookup returned the wrong animation handle");
    std::printf("{\"metric\":\"idle_asset_lookup\",\"normal\":%u,\"alternate\":%u,\"passed\":true}\n", normal,
                alternate);
}
// Captured presents in the real swapchain path: the authored frame reaches the
// readback untouched even with the ImGui overlay up, because captures skip it.
// 77 = no presentable window.
static int present_capture_test() {
    auto require = [](bool ok, const char* text) {
        if (!ok) throw std::runtime_error(text);
    };
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 77;
    SDL_Window* window = SDL_CreateWindow("present capture", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 360,
                                          SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!window) {
        SDL_Quit();
        return 77;
    }
    if (!gx2vk::init(window)) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 77;
    }
    gx2vk::Image frame;
    gx2vk::ImageDesc d;
    d.width = 640;
    d.height = 360;
    d.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    require(gx2vk::create_image(frame, d), "present capture frame allocation failed");
    gx2vk::barrier_if_dirty();
    VkClearColorValue color{};
    color.float32[0] = 0.25f;
    color.float32[1] = 0.5f;
    color.float32[2] = 0.75f;
    color.float32[3] = 1;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(gx2vk::cmd(), frame.image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
    gx2vk::note_gpu_write(frame);
    gx2_imgui::set_visible(true); // overlay up: captures must still omit it
    gx2vk::capture_presents(true);
    unsigned presented = 0;
    for (unsigned i = 0; i < 45; ++i) {
        presented += gx2vk::present(frame, 640, 360);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    if (!presented) {
        gx2vk::capture_presents(false);
        gx2vk::destroy_image(frame);
        gx2vk::shutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 77;
    }
    require(gx2vk::finish(), "captured presents did not complete");
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    require(gx2vk::last_present(rgba, w, h) && w && h, "captured present readback missing");
    // Authored colour (64,128,191) everywhere: no panel, no text, no dimming.
    size_t disturbed = 0;
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
        if (std::abs(int(rgba[i]) - 64) > 2 || std::abs(int(rgba[i + 1]) - 128) > 2 ||
            std::abs(int(rgba[i + 2]) - 191) > 2)
            ++disturbed;
    require(disturbed == 0, "the interface leaked into a captured present");
    gx2vk::capture_presents(false);
    require(gx2vk::validation_error_count() == 0, "Vulkan validation error");
    std::printf("{\"metric\":\"present_capture\",\"presents\":%u,\"width\":%u,\"height\":%u,\"passed\":true}\n",
                presented, w, h);
    gx2vk::destroy_image(frame);
    gx2vk::shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
// Guest hook registry, driven exactly as a recompiler --hooks wrapper does.
static void authored_original(CPUState* cpu) {
    cpu->r[3] = cpu->r[3] * 2 + 1;
}
static void authored_wrapper(CPUState* cpu) {
    if (const RbrewGuestHook hook = rbrew_guest_hook(0x02000010u)) {
        hook(cpu, 0x02000010u, authored_original);
        return;
    }
    authored_original(cpu);
}
static void guest_hook_test() {
    auto require = [](bool ok, const char* text) {
        if (!ok) throw std::runtime_error(text);
    };
    CPUState cpu{};
    cpu.r[3] = 5;
    authored_wrapper(&cpu);
    require(cpu.r[3] == 11 && !rbrew_guest_hook(0x02000010u), "unhooked wrapper changed the original");
    static uint32_t seen = 0;
    require(rbrew_set_guest_hook(0x02000010u,
                                 [](CPUState* c, uint32_t address, HostFunc original) {
                                     seen = address;
                                     original(c);
                                     c->r[4] = c->r[3];
                                 }),
            "hook registration failed");
    require(rbrew_guest_hook(0x02000014u) == nullptr, "hook matched another address");
    cpu.r[3] = 5;
    authored_wrapper(&cpu);
    require(cpu.r[3] == 11 && cpu.r[4] == 11 && seen == 0x02000010u, "hook did not wrap the original");
    require(rbrew_set_guest_hook(0x02000010u, nullptr), "hook clear failed");
    cpu.r[4] = 0;
    authored_wrapper(&cpu);
    require(cpu.r[3] == 23 && cpu.r[4] == 0, "cleared hook still ran");
    std::printf("{\"metric\":\"guest_hooks\",\"passed\":true}\n");
}
int main(int argc, char** argv) {
    try {
#ifdef _WIN32
        SetUnhandledExceptionFilter(benchmark_exception);
#endif
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        if (argc == 5 && std::string_view(argv[1]) == "idle_asset_lookup") {
            idle_asset_lookup_test(argv[2], uint32_t(std::stoul(argv[3])), uint32_t(std::stoul(argv[4])));
            return 0;
        }
        if (argc != 2) throw std::runtime_error("expected an authored benchmark mode");
        if (std::string_view(argv[1]) == "guest_hooks") {
            guest_hook_test();
            return 0;
        }
        if (std::string_view(argv[1]) == "present_capture") return present_capture_test();
        if (std::string_view(argv[1]) == "idle_lookaround") {
            idle_lookaround_test();
            return 0;
        }
        const std::string_view mode = argv[1];
        // npc_<count> / npc_fallback_<count>: skinned characters, 1-100.
        unsigned npc_characters = 0;
        const bool npc_fallback = mode.starts_with("npc_fallback_");
        if (mode.starts_with("npc_")) {
            const auto digits = mode.substr(npc_fallback ? 13 : 4);
            if (digits == "1" || digits == "10" || digits == "25" || digits == "50" || digits == "100")
                npc_characters = unsigned(std::stoul(std::string(digits)));
            else
                throw std::runtime_error("unknown authored benchmark");
        }
        if (!npc_characters && mode != "fallback" && mode != "push" && mode != "churn" && mode != "contracts" &&
            mode != "lifetime" && mode != "heavy" && mode != "heavy_push" && mode != "overdraw" && mode != "watch" &&
            mode != "geometry" && mode != "packets" && mode != "plaza")
            throw std::runtime_error("unknown authored benchmark");
        if (!gx2vk::init(nullptr)) return 77;
        gx2_render_init();
        std::printf("{\"device\":\"%s\",\"workload\":\"%s\",\"synthetic\":true}\n", gx2vk::caps().device_name.c_str(),
                    argv[1]);
#ifdef RBREW_BENCH_SAMPLER
        std::optional<BenchSampler> sampler;
        sampler.emplace();
#endif
        if (mode == "contracts")
            gx2_render_vk_test_binding_contracts();
        else if (mode == "watch")
            gx2_render_vk_test_watch_ranges();
        else if (mode == "geometry")
            gx2_render_vk_test_vertex_plans();
        else if (mode == "packets")
            gx2_render_vk_test_packet_batches();
        else if (mode == "plaza")
            gx2_render_vk_test_plaza();
        else if (npc_characters)
            gx2_render_vk_test_npc(npc_characters, !npc_fallback);
        else if (mode == "lifetime")
            gx2_render_vk_test_lifetime();
        else if (mode == "heavy" || mode == "heavy_push" || mode == "overdraw")
            gx2_render_vk_test_heavy(mode == "overdraw", mode == "heavy_push");
        else
            gx2_render_vk_test_stress(mode == "push", mode == "churn");
#ifdef RBREW_BENCH_SAMPLER
        sampler.reset();
#endif
        if (gx2vk::validation_error_count()) throw std::runtime_error("Vulkan validation error");
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX m{};
        m.cb = sizeof(m);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof(m));
        std::printf("{\"metric\":\"memory\",\"peak_commit_bytes\":%llu,\"peak_working_set_bytes\":%llu,\"vulkan_owned_"
                    "bytes\":%llu}\n",
                    (unsigned long long)m.PeakPagefileUsage, (unsigned long long)m.PeakWorkingSetSize,
                    (unsigned long long)gx2vk::allocated_bytes());
#endif
        gx2_render_shutdown();
        return 0;
    } catch (const std::exception& e) {
        RBREW_LOG_ERROR("render", "render stress failed: %s", e.what());
        return 1;
    }
}

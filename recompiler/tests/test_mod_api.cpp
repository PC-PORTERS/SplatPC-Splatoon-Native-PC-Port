#include "mod_loader.h"
#include "rebrewu_runtime.h"
#include "splat_mod_api.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "test_mod_api: CHECK failed at line %d: %s\n", __LINE__, #cond);                      \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

namespace {

volatile int g_side_effect_sink = 0;

// ---- Native test targets (kept unique to prevent MSVC /OPT:ICF merging) ----

volatile int g_native_bias = 10;

RBREW_GUEST_BODY int native_add_biased(int a, int b) {
    const int sum = a + b + g_native_bias;
    g_side_effect_sink += 1;
    return sum;
}

RBREW_GUEST_BODY int native_short_leaf(int x) {
    g_side_effect_sink += 7;
    return x * 3 + 7;
}

SPLAT_DEFINE_NATIVE_HOOK(NativeBiasedHookA, int, (int a, int b))
int NativeBiasedHookA::Callback(int a, int b) {
    return Orig(a + 1, b) + 100;
}

SPLAT_DEFINE_NATIVE_HOOK(NativeBiasedHookB, int, (int a, int b))
int NativeBiasedHookB::Callback(int a, int b) {
    return Orig(a, b) * 2;
}

SPLAT_DEFINE_NATIVE_HOOK(NativeBiasedHookC, int, (int a, int b))
int NativeBiasedHookC::Callback(int a, int b) {
    return Orig(a, b) + 5;
}

SPLAT_DEFINE_NATIVE_HOOK(NativeShortLeafHook, int, (int x))
int NativeShortLeafHook::Callback(int x) {
    return Orig(x) + 1000;
}

// ---- Guest test targets across Tiers 1, 2, and 3 ----

RBREW_GUEST_BODY void tier1_shared_body(CPUState* __restrict cpu, uint8_t* __restrict /*mem*/) {
    g_side_effect_sink += 11;
    cpu->r[3] += 1u;
}

RBREW_GUEST_BODY void tier1_stack_body(CPUState* __restrict cpu, uint8_t* __restrict /*mem*/) {
    g_side_effect_sink += 13;
    cpu->r[3] += 5u;
}

void tier1_stack_unhooked(CPUState* cpu) {
    tier1_stack_body(cpu, cpu->mem);
}

void tier1_stack_entry(CPUState* cpu) {
    if (const RbrewGuestHook hook = rbrew_guest_hook(0x00021000u)) {
        hook(cpu, 0x00021000u, &tier1_stack_unhooked);
        return;
    }
    tier1_stack_body(cpu, cpu->mem);
}

void builtin_manifest_hook(CPUState* cpu, uint32_t /*addr*/, HostFunc orig) {
    if (orig) orig(cpu);
    cpu->r[3] += 1000u;
}

SPLAT_DEFINE_GUEST_HOOK(Tier1ModHookA)
void Tier1ModHookA::Callback(CPUState* cpu, uint32_t /*guest_addr*/) {
    Orig(cpu);
    cpu->r[3] += 10u;
}

SPLAT_DEFINE_GUEST_HOOK(Tier1ModHookB)
void Tier1ModHookB::Callback(CPUState* cpu, uint32_t /*guest_addr*/) {
    Orig(cpu);
    cpu->r[3] *= 2u;
}

void tier1_noop_callback(CPUState* cpu, uint32_t /*guest_addr*/) {
    (void)cpu;
}

RBREW_GUEST_BODY void tier2_body(CPUState* __restrict cpu, uint8_t* __restrict /*mem*/) {
    g_side_effect_sink += 17;
    cpu->r[3] = cpu->r[3] * 10u + 3u;
}

void tier2_entry(CPUState* cpu) {
    tier2_body(cpu, cpu->mem);
}

SPLAT_DEFINE_GUEST_HOOK(Tier2ModHookA)
void Tier2ModHookA::Callback(CPUState* cpu, uint32_t /*guest_addr*/) {
    Orig(cpu);
    cpu->r[3] += 50u;
}

SPLAT_DEFINE_GUEST_HOOK(Tier2ModHookB)
void Tier2ModHookB::Callback(CPUState* cpu, uint32_t /*guest_addr*/) {
    Orig(cpu);
    cpu->r[3] += 7u;
}

void tier3_cafe_stub(CPUState* cpu) {
    g_side_effect_sink += 19;
    cpu->r[3] = cpu->r[3] + cpu->r[4] + 9u;
}

SPLAT_DEFINE_GUEST_HOOK(Tier3ModHook)
void Tier3ModHook::Callback(CPUState* cpu, uint32_t /*guest_addr*/) {
    Orig(cpu);
    cpu->r[3] *= 3u;
}

RBREW_GUEST_BODY void plugin_target_body(CPUState* __restrict cpu, uint8_t* __restrict /*mem*/) {
    g_side_effect_sink += 23;
    cpu->r[3] += 7u;
}

void plugin_target_entry(CPUState* cpu) {
    plugin_target_body(cpu, cpu->mem);
}

void test_native_trampolines(const SplatModHost* host) {
    CHECK(native_add_biased(2, 3) == 15);
    CHECK(native_short_leaf(4) == 19);

    CHECK(NativeBiasedHookA::InstallAtFn(host, &native_add_biased));
    CHECK(native_add_biased(2, 3) == 116); // Orig(3, 3) + 100

    CHECK(NativeBiasedHookB::InstallAtFn(host, &native_add_biased));
    CHECK(native_add_biased(2, 3) == 232); // 116 * 2

    CHECK(NativeBiasedHookC::InstallAtFn(host, &native_add_biased));
    CHECK(native_add_biased(2, 3) == 237); // 232 + 5

    // Unhook middle detour first, verifying C -> A -> Orig chain stays intact.
    NativeBiasedHookB::Uninstall(host);
    CHECK(native_add_biased(2, 3) == 121); // 116 + 5

    // Unhook top detour, leaving A -> Orig.
    NativeBiasedHookC::Uninstall(host);
    CHECK(native_add_biased(2, 3) == 116);

    // Unhook final detour, restoring original prologue.
    NativeBiasedHookA::Uninstall(host);
    CHECK(native_add_biased(2, 3) == 15);

    CHECK(NativeShortLeafHook::InstallAtFn(host, &native_short_leaf));
    CHECK(native_short_leaf(4) == 1019);
    NativeShortLeafHook::Uninstall(host);
    CHECK(native_short_leaf(4) == 19);
}

void test_tier1_manifest_hooks(const SplatModHost* host, CPUState* cpu, std::vector<HostFunc>& dispatch) {
    // Verify dynamic scaling past the legacy 32-hook limit.
    constexpr size_t kBulkHooks = 48;
    std::vector<SplatModHookHandle> handles;
    handles.reserve(kBulkHooks);
    for (size_t i = 0; i < kBulkHooks; ++i) {
        const uint32_t addr = 0x00020000u + uint32_t(i * 4u);
        rbrew_register_guest_body(addr, reinterpret_cast<void*>(&tier1_shared_body), true);
        SplatModGuestOrig orig = nullptr;
        const SplatModHookHandle h = host->hook_guest(addr, &tier1_noop_callback, &orig);
        CHECK(h != 0 && orig != nullptr);
        handles.push_back(h);
    }
    for (SplatModHookHandle h : handles) CHECK(host->unhook_guest(h));

    // Verify stacking mods on top of a pre-existing built-in C++ manifest hook.
    constexpr uint32_t kStackAddr = 0x00021000u;
    rbrew_register_guest_body(kStackAddr, reinterpret_cast<void*>(&tier1_stack_body), true);
    dispatch[kStackAddr >> 2] = &tier1_stack_entry;
    CHECK(rbrew_set_guest_hook(kStackAddr, &builtin_manifest_hook));

    CHECK(Tier1ModHookA::InstallAt(host, kStackAddr));
    CHECK(Tier1ModHookB::InstallAt(host, kStackAddr));

    cpu->r[3] = 1u;
    tier1_stack_entry(cpu);
    CHECK(cpu->r[3] == 2032u); // ((1 + 5 + 1000) + 10) * 2

    // Remove bottom mod hook while top mod + built-in hook stay active.
    Tier1ModHookA::Uninstall(host);
    cpu->r[3] = 1u;
    tier1_stack_entry(cpu);
    CHECK(cpu->r[3] == 2012u); // (1 + 5 + 1000) * 2

    // Remove last mod hook, restoring the built-in manifest hook directly.
    Tier1ModHookB::Uninstall(host);
    CHECK(rbrew_guest_hook(kStackAddr) == &builtin_manifest_hook);
    cpu->r[3] = 1u;
    tier1_stack_entry(cpu);
    CHECK(cpu->r[3] == 1006u);

    CHECK(rbrew_set_guest_hook(kStackAddr, nullptr));
}

void test_tier2_body_trampolines(const SplatModHost* host, CPUState* cpu, std::vector<HostFunc>& dispatch) {
    constexpr uint32_t kBodyAddr = 0x00022000u;
    rbrew_register_guest_body(kBodyAddr, reinterpret_cast<void*>(&tier2_body), false);
    dispatch[kBodyAddr >> 2] = &tier2_entry;

    cpu->r[3] = 2u;
    tier2_body(cpu, cpu->mem);
    CHECK(cpu->r[3] == 23u);

    CHECK(Tier2ModHookA::InstallAt(host, kBodyAddr));
    CHECK(Tier2ModHookB::InstallAt(host, kBodyAddr));

    // Direct call to compiled body hits trampoline -> ModB -> ModA -> orig.
    cpu->r[3] = 2u;
    tier2_body(cpu, cpu->mem);
    CHECK(cpu->r[3] == 80u); // (2 * 10 + 3) + 50 + 7

    // Indirect dispatch also hits the hooked body.
    cpu->r[3] = 2u;
    rbrew_dispatch(cpu, kBodyAddr);
    CHECK(cpu->r[3] == 80u);

    Tier2ModHookA::Uninstall(host);
    cpu->r[3] = 2u;
    tier2_body(cpu, cpu->mem);
    CHECK(cpu->r[3] == 30u); // 23 + 7

    Tier2ModHookB::Uninstall(host);
    cpu->r[3] = 2u;
    tier2_body(cpu, cpu->mem);
    CHECK(cpu->r[3] == 23u);
}

void test_tier3_dispatch_and_call_guest(const SplatModHost* host, CPUState* cpu, std::vector<HostFunc>& dispatch) {
    constexpr uint32_t kStubAddr = 0x00023000u;
    dispatch[kStubAddr >> 2] = &tier3_cafe_stub;

    cpu->lr = 0xDEADBEEFu;
    cpu->r[3] = 0x11111111u;
    cpu->r[4] = 0x22222222u;
    cpu->r[10] = 0xAAAAAAAAu;

    const uint32_t args[2] = {4u, 5u};
    uint32_t r3_out = 0u;
    CHECK(host->call_guest(cpu, kStubAddr, args, 2u, &r3_out));
    CHECK(r3_out == 18u);
    // Caller registers and LR must stay intact across call_guest.
    CHECK(cpu->lr == 0xDEADBEEFu);
    CHECK(cpu->r[3] == 0x11111111u);
    CHECK(cpu->r[4] == 0x22222222u);
    CHECK(cpu->r[10] == 0xAAAAAAAAu);

    CHECK(Tier3ModHook::InstallAt(host, kStubAddr));
    CHECK(host->call_guest(cpu, kStubAddr, args, 2u, &r3_out));
    CHECK(r3_out == 54u);

    Tier3ModHook::Uninstall(host);
    CHECK(host->call_guest(cpu, kStubAddr, args, 2u, &r3_out));
    CHECK(r3_out == 18u);

    // Unmapped or misaligned targets fail cleanly without aborting.
    CHECK(!host->call_guest(cpu, 0x0003FFFCu, args, 2u, &r3_out));
    CHECK(!host->call_guest(cpu, kStubAddr + 1u, args, 2u, &r3_out));
}

void test_host_extensions(const SplatModHost* host) {
    host->set_dual_view_active(true);
    CHECK(rbrew_mod::dual_view_active());
    host->set_dual_view_active(false);
    CHECK(!rbrew_mod::dual_view_active());

    host->set_suppress_cinema_hud(true);
    CHECK(rbrew_mod::suppress_cinema_hud());
    host->set_suppress_cinema_hud(false);
    CHECK(!rbrew_mod::suppress_cinema_hud());

    host->request_screen_mode(SPLATMOD_SCREEN_GAMEPAD);
    int req_mode = SPLATMOD_SCREEN_TV;
    CHECK(rbrew_mod::consume_screen_mode_request(&req_mode));
    CHECK(req_mode == SPLATMOD_SCREEN_GAMEPAD);
    CHECK(!rbrew_mod::consume_screen_mode_request(&req_mode));

    rbrew_mod::note_screen_mode(SPLATMOD_SCREEN_GAMEPAD);
    CHECK(host->current_screen_mode() == SPLATMOD_SCREEN_GAMEPAD);
    rbrew_mod::note_screen_mode(SPLATMOD_SCREEN_TV);
    CHECK(host->current_screen_mode() == SPLATMOD_SCREEN_TV);

    host->request_quit();
    CHECK(rbrew_mod::consume_quit_request());
    CHECK(!rbrew_mod::consume_quit_request());
}

void test_plugin_load_and_unload(const char* plugin_path, const SplatModHost* host, CPUState* cpu, uint8_t* arena,
                                 std::vector<HostFunc>& dispatch) {
    constexpr uint32_t kPluginAddr = 0x00010000u;
    rbrew_register_guest_body(kPluginAddr, reinterpret_cast<void*>(&plugin_target_body), false);
    dispatch[kPluginAddr >> 2] = &plugin_target_entry;

    const uint32_t args[1] = {10u};
    uint32_t r3_out = 0u;
    CHECK(host->call_guest(cpu, kPluginAddr, args, 1u, &r3_out));
    CHECK(r3_out == 17u);

    CHECK(!rbrew_mod::has_mods());
    CHECK(rbrew_mod::load_mod_from_path(plugin_path, 16u));
    CHECK(rbrew_mod::has_mods());

    const auto mods = rbrew_mod::list_mods();
    CHECK(mods.size() == 1 && mods[0].id == "splatpc.test_mod" && mods[0].initialized);

    const auto pages = rbrew_mod::list_menu_pages();
    CHECK(pages.size() == 1 && pages[0].title == "Test Mod Options");
    CHECK(pages[0].status && std::strcmp(pages[0].status(pages[0].user), "Ready") == 0);

    // Plugin's SPLAT_DEFINE_GUEST_HOOK adds +100 on top of plugin_target_body (+7).
    CHECK(host->call_guest(cpu, kPluginAddr, args, 1u, &r3_out));
    CHECK(r3_out == 117u);

    // Plugin's VPAD hook writes big-endian 0xCAFEBABE to 0x00001000.
    std::memset(arena + 0x00001000u, 0, 4);
    rbrew_mod::on_vpad_read(arena);
    CHECK(splatmod_read32(arena, 0x00001000u) == 0xCAFEBABEu);

    // Plugin's KPAD provider reports Player 2 Pro Controller on chan 0.
    SplatModProPad pad{};
    CHECK(rbrew_mod::poll_kpad_provider(0u, &pad));
    CHECK(pad.hold == 0x00000010u && pad.lx == 0.5f && pad.ly == -0.25f && pad.ry == 0.75f);
    CHECK(!rbrew_mod::poll_kpad_provider(1u, &pad));

    // Plugin's audio tap receives stereo frames without crashing.
    float stereo[32] = {};
    rbrew_mod::on_audio_frames(stereo, 16u);

    std::string title;
    CHECK(rbrew_mod::copy_window_title_override(title) && title == "SplatPC Test Mod Title");
    CHECK(rbrew_mod::relative_mouse_in_gamepad());

    // Shutdown & unload must cleanly remove all hooks and extensions.
    rbrew_mod::unload_mods_for_test();
    CHECK(!rbrew_mod::has_mods());
    CHECK(rbrew_mod::list_mods().empty());
    CHECK(rbrew_mod::list_menu_pages().empty());
    CHECK(!rbrew_mod::has_vpad_hooks());
    CHECK(!rbrew_mod::has_kpad_providers());
    CHECK(!rbrew_mod::relative_mouse_in_gamepad());
    CHECK(!rbrew_mod::copy_window_title_override(title));

    CHECK(host->call_guest(cpu, kPluginAddr, args, 1u, &r3_out));
    CHECK(r3_out == 17u);
}

} // namespace

int main(int argc, char** argv) {
    std::vector<uint8_t> arena(0x40000u, 0);
    std::vector<HostFunc> dispatch(0x40000u >> 2, nullptr);

    CPUState cpu{};
    cpu.mem = arena.data();
    cpu.dispatch_table = dispatch.data();
    cpu.dispatch_size = uint32_t(dispatch.size());

    rbrew_mod::set_dispatch_cpu_for_test(&cpu);
    const SplatModHost* host = rbrew_mod::host_api();
    CHECK(host && host->abi_version == SPLATMOD_ABI_VERSION);

    test_native_trampolines(host);
    test_tier1_manifest_hooks(host, &cpu, dispatch);
    test_tier2_body_trampolines(host, &cpu, dispatch);
    test_tier3_dispatch_and_call_guest(host, &cpu, dispatch);
    test_host_extensions(host);

    if (argc >= 2 && argv[1] && *argv[1]) {
        test_plugin_load_and_unload(argv[1], host, &cpu, arena.data(), dispatch);
    }

    rbrew_mod::unload_mods_for_test();
    std::printf("test_mod_api: all tests passed (%s)\n", (argc >= 2 && argv[1]) ? argv[1] : "no plugin arg");
    return 0;
}

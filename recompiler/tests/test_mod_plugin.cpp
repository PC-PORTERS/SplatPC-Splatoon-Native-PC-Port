// Standalone test plugin verifying the SplatPC Modding SDK ABI with no host linkage.

#include "splat_mod_api.h"

#include <cstring>

namespace {

constexpr uint32_t kPluginTestGuestAddr = 0x00010000u;

const SplatModHost* g_host = nullptr;
SplatModHookHandle g_menu_handle = 0;
SplatModHookHandle g_vpad_handle = 0;
SplatModHookHandle g_kpad_handle = 0;
SplatModHookHandle g_audio_handle = 0;
uint32_t g_audio_frames_seen = 0;

SPLAT_DEFINE_GUEST_HOOK(PluginGuestHook)
void PluginGuestHook::Callback(CPUState* cpu, uint32_t /*guest_addr*/) {
    Orig(cpu);
    cpu->r[3] += 100u;
}

void on_vpad_tick(uint8_t* mem, void* /*user*/) {
    if (g_host && mem && g_host->memory_accessible(mem, 0x00001000u, 4)) {
        splatmod_write32(mem, 0x00001000u, 0xCAFEBABEu);
    }
}

bool on_kpad_poll(uint32_t chan, SplatModProPad* out_pad, void* /*user*/) {
    if (chan != 0 || !out_pad) return false;
    out_pad->hold = 0x00000010u; // A button
    out_pad->lx = 0.5f;
    out_pad->ly = -0.25f;
    out_pad->rx = 0.0f;
    out_pad->ry = 0.75f;
    return true;
}

void on_audio_tap(const float* stereo_frames, uint32_t frame_count, void* /*user*/) {
    if (stereo_frames) g_audio_frames_seen += frame_count;
}

int draw_mod_page(const SplatModUi* ui, void* /*user*/) {
    if (ui && ui->text) ui->text(ui->user, "Test mod plugin page");
    return SPLATMOD_MENU_STAY;
}

const char* mod_page_status(void* /*user*/) {
    return "Ready";
}

const SplatModInfo kInfo = {
    SPLATMOD_ABI_VERSION, "splatpc.test_mod", "SplatPC Test Mod", "1.0.0", "SplatPC Tests",
};

} // namespace

SPLATMOD_EXPORT const SplatModInfo* splatmod_info(void) {
    return &kInfo;
}

SPLATMOD_EXPORT int splatmod_early_init(const SplatModEarlyHost* host, int argc, char** argv) {
    if (!host || host->abi_version != SPLATMOD_ABI_VERSION) return 1;
    if (argc >= 2 && argv && argv[1] && std::strcmp(argv[1], "--test-mod-early-exit") == 0) return 42;
    return -1;
}

SPLATMOD_EXPORT bool splatmod_init(const SplatModHost* host) {
    if (!host || host->abi_version != SPLATMOD_ABI_VERSION) return false;
    g_host = host;

    if (!PluginGuestHook::InstallAt(host, kPluginTestGuestAddr)) return false;

    g_menu_handle = host->add_menu_page("Test Mod Options", &draw_mod_page, &mod_page_status, nullptr);
    g_vpad_handle = host->add_vpad_hook(&on_vpad_tick, nullptr);
    g_kpad_handle = host->add_kpad_provider(&on_kpad_poll, nullptr);
    g_audio_handle = host->add_audio_tap(&on_audio_tap, nullptr);
    host->set_window_title("SplatPC Test Mod Title");
    host->set_relative_mouse_in_gamepad(true);
    return g_menu_handle != 0 && g_vpad_handle != 0 && g_kpad_handle != 0 && g_audio_handle != 0;
}

SPLATMOD_EXPORT void splatmod_shutdown(void) {
    if (!g_host) return;
    PluginGuestHook::Uninstall(g_host);
    if (g_menu_handle) g_host->unhook_extension(g_menu_handle);
    if (g_vpad_handle) g_host->unhook_extension(g_vpad_handle);
    if (g_kpad_handle) g_host->unhook_extension(g_kpad_handle);
    if (g_audio_handle) g_host->unhook_extension(g_audio_handle);
    g_host->set_window_title(nullptr);
    g_host->set_relative_mouse_in_gamepad(false);
    g_menu_handle = g_vpad_handle = g_kpad_handle = g_audio_handle = 0;
    g_host = nullptr;
}

// coop.cpp - two-player co-op (Splatoon v288): story levels and Octo Valley with a
// second player on the game's second screen, and Battle Dojo labels for online play.
//
// Player 1 (host) is view 0 (TV, GamePad controller); player 2 (online guest or second
// local controller) is view 1, the second-screen path the game uses for Battle Dojo.
#include "../runtime/rebrewu_runtime.h"
#include "../runtime/benchmark_input.h"
#include "../runtime/native_control_defaults.h"
#include "../runtime/coop_views.h"
#include "../runtime/netplay.h"
#include "../runtime/port_settings.h"
#include "../os/vpad/game_controllers.h"
#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

namespace {
std::mutex s_players_mutex;
std::vector<uint32_t> s_players; // player objects of the current scene

std::string read_name(uint8_t* mem, uint32_t address) {
    std::string name;
    for (uint32_t i = 0; i < 96 && address && rbrew_memory_accessible(mem, address + i, 1); ++i) {
        const char c = char(rbrew_read8(mem, address + i));
        if (!c) break;
        name += c;
    }
    return name;
}

std::atomic<bool> s_coop{false}; // the current scene was built for two players
} // namespace

#if RBREW_GAME_REVISION == 288
namespace {
constexpr uint32_t kControllerBindingName = 0x0265A730u;
constexpr uint32_t kSceneCtor = 0x023FE9C8u;   // scene base ctor (r4 = views, r5 = cameras, r6 = tag)
constexpr uint32_t kPlayerSetup = 0x023FED60u; // players of a scene ([this+0x2C8] = count)
constexpr uint32_t kCreate3DLayer = 0x0289F358u, kAttachLayer = 0x02A3F070u;
constexpr uint32_t kCtrlMgrPtr = 0x101DDF64u;
constexpr uint32_t kGetVpad = 0x028AAB78u, kGetKpad = 0x028AAB54u;
constexpr uint32_t kCtrlBind1P = 0x02037214u, kCtrlBind2P = 0x02037394u, kCtrlSetSettings = 0x0203717Cu;
constexpr uint32_t kDynResMgrPtr = 0x101E78B0u;
constexpr uint32_t kLayer3DCubemapInit = 0x02A96208u;
constexpr uint32_t kLytAddTarget = 0x02783D90u;
constexpr uint32_t kLytTvHud = 6, kLytSecondHud = 8, kLytTvOverlay = 18, kLytSecondOverlay = 20;

uint32_t s_tv_3d_layer = 0, s_drc_3d_layer = 0, s_tv_3d_object = 0;
bool s_injected_slot1 = false;
bool s_ctrl_bound = false;

struct CubemapConfig {
    uint32_t ccc = 0, cdc = 0, w109c = 0, w10ac = 0, w10bc = 0, w10cc = 0;
    uint8_t b7b0 = 0, b108c = 0, b10dc = 0;
};
CubemapConfig s_tv_cubemap;
bool s_tv_cubemap_valid = false;
uint32_t s_main_tv2 = 0;    // player 2's Msn_MainTV_00 in the current story level
float s_p2_hud_last = 0.0f; // player 2's last HUD gauge value (scene+0x244 for player 1)

bool coop() {
    return s_coop.load(std::memory_order_relaxed);
}
// Scenes where two players play together: story levels (kind 1) and Octo Valley (3).
bool coop_level(int kind) {
    return kind == 1 || kind == 3;
}
bool coop_scene(uint8_t* mem) {
    return coop() && coop_level(rbrew_control_defaults::active_scene_type(mem));
}

bool call_guest(CPUState* cpu, uint32_t addr, std::initializer_list<uint32_t> args, uint32_t* out_r3 = nullptr) {
    if (!cpu || !cpu->dispatch_table || (addr >> 2) >= cpu->dispatch_size || !cpu->dispatch_table[addr >> 2])
        return false;
    const uint32_t sp = (cpu->r[1] - 0x40u) & ~0xFu;
    if (!rbrew_memory_accessible(cpu->mem, sp, 0x40u)) return false;
    const CPUState saved = *cpu;
    rbrew_write32(cpu->mem, sp, saved.r[1]);
    cpu->r[1] = sp;
    uint32_t reg = 3;
    for (uint32_t arg : args)
        if (reg <= 9) cpu->r[reg++] = arg;
    cpu->lr = 0;
    cpu->dispatch_table[addr >> 2](cpu);
    const uint32_t ret = cpu->r[3];
    *cpu = saved;
    if (out_r3) *out_r3 = ret;
    return true;
}

// Collects every player object of the scene as it asks for its controller binding.
void controller_binding_name(CPUState* cpu, uint32_t, HostFunc original) {
    const uint32_t player = cpu->r[3];
    original(cpu);
    std::lock_guard lock(s_players_mutex);
    if (std::find(s_players.begin(), s_players.end(), player) == s_players.end() && s_players.size() < 16)
        s_players.push_back(player);
}
uint32_t find_player(uint8_t* mem, uint32_t index) {
    std::lock_guard lock(s_players_mutex);
    for (auto it = s_players.rbegin(); it != s_players.rend(); ++it)
        if (rbrew_memory_accessible(mem, *it, 0xA0) && rbrew_read32(mem, *it + 0x9C) == index) return *it;
    return 0;
}

// Cmn::ControllerMgr gets the GamePad and KPAD channel 0 (player 2) as in Battle Dojo
// (02037394), and the GamePad alone again when the scene is no longer two-player.
void bind_coop_controllers(CPUState* cpu, bool two_players) {
    uint8_t* mem = cpu->mem;
    const uint32_t ctrl_mgr = rbrew_read32(mem, kCtrlMgrPtr);
    if (!ctrl_mgr || !rbrew_memory_accessible(mem, ctrl_mgr, 0x190u)) return;
    uint32_t vpad = 0;
    if (!call_guest(cpu, kGetVpad, {}, &vpad) || !vpad) return;
    const uint32_t saved_r1 = cpu->r[1];
    const uint32_t scratch = (saved_r1 - 0x40u) & ~0xFu;
    if (!rbrew_memory_accessible(mem, scratch, 0x40u)) return;
    rbrew_write32(mem, scratch, saved_r1);
    cpu->r[1] = scratch;
    if (two_players) {
        uint32_t kpad0 = 0;
        if (call_guest(cpu, kGetKpad, {0u}, &kpad0) && kpad0) {
            rbrew_write32(mem, scratch + 0x10u, 0u);
            rbrew_write32(mem, scratch + 0x14u, 3u);
            rbrew_write32(mem, scratch + 0x18u, rbrew_read32(mem, ctrl_mgr + 0x17Cu));
            rbrew_write32(mem, scratch + 0x1Cu, 0u);
            call_guest(cpu, kCtrlSetSettings, {ctrl_mgr, 1u, scratch + 0x18u});
            call_guest(cpu, kCtrlBind2P, {ctrl_mgr, vpad, scratch + 0x10u, vpad, kpad0, scratch + 0x14u, 0u});
            s_ctrl_bound = true;
        }
    } else if (s_ctrl_bound) {
        rbrew_write32(mem, scratch + 0x10u, 0u);
        call_guest(cpu, kCtrlBind1P, {ctrl_mgr, vpad, scratch + 0x10u});
        s_ctrl_bound = false;
    }
    cpu->r[1] = saved_r1;
}

// A story level or Octo Valley asks for one view and one camera; with a second player
// it gets two of each, as Battle Dojo does.
void scene_ctor(CPUState* cpu, uint32_t, HostFunc original) {
    s_tv_3d_layer = s_drc_3d_layer = s_tv_3d_object = 0;
    s_tv_cubemap_valid = false;
    s_main_tv2 = 0;
    s_p2_hud_last = 0.0f;
    const int kind = rbrew_control_defaults::active_scene_type(cpu->mem);
    const uint32_t views = cpu->r[4], cams = cpu->r[5], tag = cpu->r[6];
    const bool story = kind == 1 && views == 1 && cams == 1 && tag == 0x11u;
    const bool hub = kind == 3 && views == 1 && cams == 1;
    if (!story && !hub && s_ctrl_bound && kind != 6 && kind != 7) bind_coop_controllers(cpu, false);
    const bool two = (story || hub) && rbrew_settings::g_coop_story.load(std::memory_order_relaxed) &&
                     rbrew_coop::second_player_present();
    s_coop.store(two, std::memory_order_relaxed);
    if (two) {
        cpu->r[4] = 2;
        cpu->r[5] = 2;
    }
    original(cpu);
}

// Display objects, 028B242C(i): *( *(*(0x101E7698)+0xF3C) + 4i ) while i < *(...+0xF38).
// [object(5)+0x9DC] / [object(7)+0x9DC] are the views of the TV and the second screen.
// A story level reports view 0 for both, so 023FED60 put player 2's camera into view 0's
// slot and 023FF448 bound the second controller only in the one-player branch. While
// those two run, the second screen reports view 1 (as in Battle Dojo).
uint32_t display_object(uint8_t* mem, uint32_t index) {
    const uint32_t mgr = rbrew_read32(mem, 0x101E7698u);
    if (!mgr || !rbrew_memory_accessible(mem, mgr + 0xF38u, 8)) return 0;
    const uint32_t count = rbrew_read32(mem, mgr + 0xF38u), list = rbrew_read32(mem, mgr + 0xF3Cu);
    if (!list || index >= count || !rbrew_memory_accessible(mem, list + 4 * index, 4)) return 0;
    const uint32_t object = rbrew_read32(mem, list + 4 * index);
    return object && rbrew_memory_accessible(mem, object + 0x9DCu, 4) ? object : 0;
}
uint32_t s_view_override = 0, s_view_saved = 0; // object whose +0x9DC is forced to 1, its old value
void force_second_view(uint8_t* mem) {
    const uint32_t second = display_object(mem, 7);
    if (!second || s_view_override) return;
    s_view_saved = rbrew_read32(mem, second + 0x9DCu);
    if (s_view_saved == 1) return;
    s_view_override = second;
    rbrew_write32(mem, second + 0x9DCu, 1);
}
void restore_second_view(uint8_t* mem) {
    if (!s_view_override) return;
    rbrew_write32(mem, s_view_override + 0x9DCu, s_view_saved);
    s_view_override = 0;
}

constexpr uint32_t kBindControllers = 0x023FF448u;
void bind_controllers(CPUState* cpu, uint32_t, HostFunc original) {
    if (coop_scene(cpu->mem)) {
        bind_coop_controllers(cpu, true);
        const uint32_t dyn = rbrew_read32(cpu->mem, kDynResMgrPtr);
        if (dyn && rbrew_memory_accessible(cpu->mem, dyn + 0x48u, 4))
            rbrew_write32(cpu->mem, dyn + 0x48u, rbrew_read32(cpu->mem, dyn + 0x48u) & ~1u);
        force_second_view(cpu->mem);
    }
    original(cpu);
    restore_second_view(cpu->mem);
}

// 02652E84(player) hands a player its controls once the level's start sequence is over.
// A story level calls it for its main player only (returning to 025FEAA8); the same call
// is repeated for player 2.
constexpr uint32_t kReleasePlayer = 0x02652E84u, kMissionStartReturn = 0x025FEAA8u;
void release_player(CPUState* cpu, uint32_t, HostFunc original) {
    const CPUState entry = *cpu;
    original(cpu);
    if (entry.lr != kMissionStartReturn || !coop() || rbrew_control_defaults::active_scene_type(cpu->mem) != 1) return;
    const uint32_t second = find_player(cpu->mem, 1);
    if (!second || second == entry.r[3]) return;
    const CPUState after = *cpu;
    *cpu = entry;
    cpu->r[3] = second;
    original(cpu);
    *cpu = after;
}

// The match's 8 player slots: *( *(0x101DD330) + 0x10 ) is an array of 8 pointers; a slot
// is in use when byte +4 of its struct is non-zero. Player 2 gets a copy of slot 0 (same
// team: shared ink, pads and checkpoints).
uint32_t player_slots(uint8_t* mem) {
    const uint32_t holder = rbrew_read32(mem, 0x101DD330u);
    if (!holder || !rbrew_memory_accessible(mem, holder + 0x10, 4)) return 0;
    const uint32_t slots = rbrew_read32(mem, holder + 0x10);
    return slots && rbrew_memory_accessible(mem, slots, 32) ? slots : 0;
}
void player_setup(CPUState* cpu, uint32_t, HostFunc original) {
    {
        std::lock_guard lock(s_players_mutex);
        s_players.clear();
    }
    const uint32_t self = cpu->r[3];
    const int kind = rbrew_control_defaults::active_scene_type(cpu->mem);
    const bool two = coop() && coop_level(kind);
    if (two && rbrew_memory_accessible(cpu->mem, self + 0x2C8, 4) && rbrew_read32(cpu->mem, self + 0x2C8) == 1)
        rbrew_write32(cpu->mem, self + 0x2C8, 2);
    if (two) force_second_view(cpu->mem); // restored by bind_controllers (023FF448) right after

    const uint32_t slots = player_slots(cpu->mem);
    const uint32_t first = slots ? rbrew_read32(cpu->mem, slots) : 0;
    const uint32_t second = slots ? rbrew_read32(cpu->mem, slots + 4) : 0;
    constexpr uint32_t kSlotSize = 0xFC;
    const bool slots_ok = first && second && rbrew_memory_accessible(cpu->mem, first, kSlotSize) &&
                          rbrew_memory_accessible(cpu->mem, second, kSlotSize);
    if (two) {
        if (slots_ok && !rbrew_read8(cpu->mem, second + 4)) {
            for (uint32_t i = 4; i < kSlotSize; ++i)
                rbrew_write8(cpu->mem, second + i, rbrew_read8(cpu->mem, first + i));
            rbrew_write32(cpu->mem, second, 1);
            s_injected_slot1 = true;
        }
    } else if (s_injected_slot1) {
        if (kind != 6 && slots_ok) rbrew_write8(cpu->mem, second + 4, 0);
        s_injected_slot1 = false;
    }
    original(cpu);
}

// Render layers per screen: 0289F358(layer manager, index, {name, config}*, display)
// creates one, 02A3F070(layer, object) attaches what it draws. With two players the
// second screen's "Map3D(DRC)" layer draws the TV's 3D object, as in Battle Dojo.
void create_3d_layer(CPUState* cpu, uint32_t, HostFunc original) {
    const uint32_t args = cpu->r[5], display = cpu->r[6];
    original(cpu);
    const std::string name = args ? read_name(cpu->mem, rbrew_read32(cpu->mem, args)) : std::string();
    if (name == "3D(TV)" && display == 0) s_tv_3d_layer = cpu->r[3];
    if (name == "Map3D(DRC)" && display == 1) s_drc_3d_layer = cpu->r[3];
}
void attach_layer(CPUState* cpu, uint32_t, HostFunc original) {
    const uint32_t layer = cpu->r[3];
    if (layer && layer == s_tv_3d_layer) s_tv_3d_object = cpu->r[4];
    if (layer && layer == s_drc_3d_layer && s_tv_3d_object && coop_scene(cpu->mem)) cpu->r[4] = s_tv_3d_object;
    original(cpu);
}

// The environment cube map of one view's render pipeline (`this` = pipeline, surface
// description at *(this+0x170)+0x208). A story level never fills it in for a second
// view: drawing it aborts on format 0, updating it records commands until memory runs
// out. Such cube maps are skipped.
bool cube_map_empty(CPUState* cpu) {
    if (!coop_scene(cpu->mem)) return false;
    uint8_t* mem = cpu->mem;
    const uint32_t pipeline = cpu->r[3];
    const uint32_t env = rbrew_read32(mem, pipeline + 0x170u);
    const uint32_t ctx = rbrew_read32(mem, pipeline + 0x140u);
    const uint32_t tex = ctx && rbrew_memory_accessible(mem, ctx + 4u, 4) ? rbrew_read32(mem, ctx + 4u) : 0;
    const bool env_ok = env && rbrew_memory_accessible(mem, env + 0x1F4u, 0x1Cu);
    const bool tex_ok = tex && rbrew_memory_accessible(mem, tex, 0x20u);
    const uint32_t env_size = env_ok ? rbrew_read32(mem, env + 0x1F4u) : 0;
    const uint32_t env_count = env_ok ? rbrew_read32(mem, env + 0x1FCu) : 0;
    const uint32_t env_format = env_ok ? rbrew_read32(mem, env + 0x208u) : 0;
    const uint32_t tex_width = tex_ok ? rbrew_read32(mem, tex + 0x08u) : 0;
    const uint32_t tex_format = tex_ok ? rbrew_read32(mem, tex + 0x18u) : 0;
    return !ctx || !env_size || !env_count || !env_format || env_format >= 0x3Bu || !tex_width || tex_width > 4096u ||
           !tex_format;
}
constexpr uint32_t kCubemapRender = 0x029E7B00u; // draws it, returns the command pointer (r5)
void cubemap_render(CPUState* cpu, uint32_t, HostFunc original) {
    if (cube_map_empty(cpu)) {
        cpu->r[3] = cpu->r[5];
        return;
    }
    original(cpu);
}
constexpr uint32_t kCubemapUpdate = 0x029E9608u; // renders its faces, returns the command pointer (r4)
void cubemap_update(CPUState* cpu, uint32_t, HostFunc original) {
    if (cube_map_empty(cpu)) {
        const uint32_t pipeline = cpu->r[3];
        const uint32_t ctx = rbrew_read32(cpu->mem, pipeline + 0x140u);
        if (ctx && rbrew_memory_accessible(cpu->mem, ctx + 0x1560u, 4)) {
            const uint32_t remaining = rbrew_read32(cpu->mem, ctx + 0x1560u);
            if (remaining > 0) rbrew_write32(cpu->mem, ctx + 0x1560u, remaining - 1);
        }
        rbrew_write8(cpu->mem, pipeline + 0x16Du, 1);
        cpu->r[3] = cpu->r[4];
        return;
    }
    original(cpu);
}

// 0240B07C fills the scene's start/respawn point tables: team 0 at +0x02C (8 x 0x30-byte
// mtx34, valid bytes at +0x384), team 1 at +0x1AC (+0x38C). Story maps only populate team
// 0 slot 0; player 2 gets a copy beside it (in Octo Valley beside the return manhole, out
// of player 1's way).
constexpr uint32_t kSpawnSetup = 0x0240B07Cu;
void spawn_setup(CPUState* cpu, uint32_t, HostFunc original) {
    const uint32_t self = cpu->r[3];
    original(cpu);
    if (!coop_scene(cpu->mem) || !self || !rbrew_memory_accessible(cpu->mem, self, 0x394u)) return;
    if (!rbrew_read8(cpu->mem, self + 0x384u)) return;
    const uint32_t src = self + 0x02Cu;
    const float offset = rbrew_control_defaults::active_scene_type(cpu->mem) == 3 ? 25.0f : 5.0f;
    struct Target {
        uint32_t mtx, valid;
    };
    const Target targets[] = {
        {self + 0x05Cu, self + 0x385u}, // team 0 slot 1
        {self + 0x1ACu, self + 0x38Cu}, // team 1 slot 0
        {self + 0x1DCu, self + 0x38Du}, // team 1 slot 1
    };
    for (const Target& t : targets) {
        if (rbrew_read8(cpu->mem, t.valid)) continue;
        for (uint32_t off = 0; off < 0x30u; off += 4)
            rbrew_write32(cpu->mem, t.mtx + off, rbrew_read32(cpu->mem, src + off));
        rbrew_write_f32(cpu->mem, t.mtx + 0x24u, rbrew_read_f32(cpu->mem, src + 0x24u) + offset);
        rbrew_write8(cpu->mem, t.valid, 1);
    }
}

// Battle Dojo HUD (Del_MainTV_00, one instance drawn on both screens, at [handler+0x38]):
// the score boxes are marked with device pictures (P_DRC_00 GamePad, P_TV_00 with text
// T_TV_00 "TV"). In online play the GamePad box is the host and the TV box the guest: the
// pictures are hidden and the boxes say "P1" and "P2" (a clone of T_TV_00 made with the
// game's 02104C28, appended to N_DRC_00).
uint32_t s_dojo_hud = 0, s_label_layout = 0, s_p1_label = 0;
bool pane_named(uint8_t* mem, uint32_t pane, const char* name) {
    for (uint32_t i = 0; i < 24; ++i) {
        const char c = char(rbrew_read8(mem, pane + 0x80 + i));
        if (c != name[i]) return false;
        if (!c) return true;
    }
    return false;
}
uint32_t find_pane(uint8_t* mem, uint32_t pane, const char* name, int depth = 0) {
    if (!pane || depth > 16 || !rbrew_memory_accessible(mem, pane, 0xD8)) return 0;
    if (pane_named(mem, pane, name)) return pane;
    const uint32_t end = pane + 0x14;
    uint32_t guard = 0;
    for (uint32_t child = rbrew_read32(mem, end); child && child != end && guard < 256;
         child = rbrew_read32(mem, child), ++guard)
        if (const uint32_t found = find_pane(mem, child, name, depth + 1)) return found;
    return 0;
}
void set_pane_xy(uint8_t* mem, uint32_t pane, float x, float y) {
    rbrew_write_f32(mem, pane + 0x1C, x);
    rbrew_write_f32(mem, pane + 0x20, y);
    rbrew_write8(mem, pane + 0x44, rbrew_read8(mem, pane + 0x44) | 0x10); // matrix dirty
}
void set_pane_visible(uint8_t* mem, uint32_t pane, bool visible) {
    rbrew_write8(mem, pane + 0x44, uint8_t((rbrew_read8(mem, pane + 0x44) & ~1u) | (visible ? 1u : 0u)));
}
void set_player_label(uint8_t* mem, uint32_t text_box, char digit) { // UTF-16BE "P1"/"P2"
    const uint32_t buffer = rbrew_read32(mem, text_box + 0xA4);
    if (!buffer || rbrew_read16(mem, text_box + 0xCC) < 3 || !rbrew_memory_accessible(mem, buffer, 6)) return;
    if (rbrew_read16(mem, buffer) == 'P' && rbrew_read16(mem, buffer + 2) == uint16_t(digit) &&
        rbrew_read16(mem, text_box + 0xCE) == 2)
        return;
    rbrew_write16(mem, buffer, 'P');
    rbrew_write16(mem, buffer + 2, uint16_t(digit));
    rbrew_write16(mem, buffer + 4, 0);
    rbrew_write16(mem, text_box + 0xCE, 2);
    rbrew_write8(mem, text_box + 0xD0, rbrew_read8(mem, text_box + 0xD0) | 0x20); // text changed
}
// build: clone and move panes (game calls that allocate), only where the game builds
// layouts itself; otherwise only memory writes (visibility, text), safe from any hook.
void dojo_player_labels(CPUState* cpu, bool build) {
    uint8_t* mem = cpu->mem;
    if (!s_dojo_hud || !rbrew_net::host_guest_connected() || !rbrew_memory_accessible(mem, s_dojo_hud + 0x38, 4))
        return;
    const uint32_t hud = rbrew_read32(mem, s_dojo_hud + 0x38);
    const uint32_t layout = hud && rbrew_memory_accessible(mem, hud + 0x30, 4) ? rbrew_read32(mem, hud + 0x30) : 0;
    const uint32_t root =
        layout && rbrew_memory_accessible(mem, layout + 0x1C, 4) ? rbrew_read32(mem, layout + 0x1C) : 0;
    if (!root) return;
    if (hud != s_label_layout) {
        s_label_layout = hud;
        s_p1_label = 0;
    }
    const uint32_t box1 = find_pane(mem, root, "N_DRC_00"), box2 = find_pane(mem, root, "N_TV_00");
    const uint32_t icon1 = find_pane(mem, root, "P_DRC_00"), icon2 = find_pane(mem, root, "P_TV_00");
    const uint32_t text2 = find_pane(mem, root, "T_TV_00");
    if (!box1 || !box2 || !icon1 || !icon2 || !text2 || !rbrew_read32(mem, text2 + 0xB4)) return; // not built yet
    if (!s_p1_label) {
        if (!build) return;
        uint32_t clone = 0;
        call_guest(cpu, 0x02104C28u, {0u, text2, box1}, &clone); // copy of T_TV_00 under N_DRC_00
        if (!clone) return;
        set_pane_xy(mem, clone, -46.0f, 6.0f);
        rbrew_write8(mem, clone + 0x80, 'X'); // keep the original the only "T_TV_00"
        s_p1_label = clone;
    }
    if (rbrew_read32(mem, text2 + 0x0C) == icon2) { // move "TV" out of its picture so the picture can hide
        if (!build) return;
        call_guest(cpu, 0x0292BC08u, {icon2, text2});
        call_guest(cpu, 0x0292BBB0u, {box2, text2});
        set_pane_xy(mem, text2, 46.0f, 6.0f);
    }
    set_pane_visible(mem, icon1, false);
    set_pane_visible(mem, icon2, false);
    set_player_label(mem, s_p1_label, '1');
    set_player_label(mem, text2, '2');
}

// 026BA97C and 026BA904 answer "is this a two-player (Dojo) match" (scene kind == 6);
// per-player cameras, controls and second-screen helpers hang off both.
constexpr uint32_t kIsDuel = 0x026BA97Cu, kIsDuel2 = 0x026BA904u;
void is_duel(CPUState* cpu, uint32_t, HostFunc original) {
    if (coop_scene(cpu->mem)) {
        cpu->r[3] = 1;
        return;
    }
    original(cpu);
}
void is_duel2(CPUState* cpu, uint32_t, HostFunc original) {
    is_duel(cpu, 0, original);
    static uint32_t labelled_frame = ~0u; // called many times per frame in a match
    const uint32_t frame = rbrew_benchmark::frame.load(std::memory_order_relaxed);
    if (frame != labelled_frame && rbrew_control_defaults::active_scene_type(cpu->mem) == 6) {
        labelled_frame = frame;
        dojo_player_labels(cpu, false);
    }
}

// A level's second 3D layer has no HDR cube map configuration of its own: it gets the TV's.
void layer3d_cubemap_init(CPUState* cpu, uint32_t, HostFunc original) {
    uint8_t* mem = cpu->mem;
    const uint32_t layer3d = cpu->r[3];
    const uint32_t cfg = layer3d && rbrew_memory_accessible(mem, layer3d + 4u, 4) ? rbrew_read32(mem, layer3d + 4u) : 0;
    if (!cfg || !rbrew_memory_accessible(mem, cfg, 0x10E0u)) return original(cpu);
    const CubemapConfig current{
        rbrew_read32(mem, cfg + 0xCCCu),  rbrew_read32(mem, cfg + 0xCDCu),  rbrew_read32(mem, cfg + 0x109Cu),
        rbrew_read32(mem, cfg + 0x10ACu), rbrew_read32(mem, cfg + 0x10BCu), rbrew_read32(mem, cfg + 0x10CCu),
        rbrew_read8(mem, cfg + 0x7B0u),   rbrew_read8(mem, cfg + 0x108Cu),  rbrew_read8(mem, cfg + 0x10DCu)};
    if (current.ccc && current.cdc) {
        s_tv_cubemap = current;
        s_tv_cubemap_valid = true;
    } else if (s_tv_cubemap_valid && coop_scene(mem)) {
        const CubemapConfig& v = s_tv_cubemap;
        rbrew_write32(mem, cfg + 0xCCCu, v.ccc);
        rbrew_write32(mem, cfg + 0xCDCu, v.cdc);
        rbrew_write32(mem, cfg + 0x109Cu, v.w109c);
        rbrew_write32(mem, cfg + 0x10ACu, v.w10ac);
        rbrew_write32(mem, cfg + 0x10BCu, v.w10bc);
        rbrew_write32(mem, cfg + 0x10CCu, v.w10cc);
        rbrew_write8(mem, cfg + 0x7B0u, v.b7b0);
        rbrew_write8(mem, cfg + 0x108Cu, v.b108c);
        rbrew_write8(mem, cfg + 0x10DCu, v.b10dc);
    }
    original(cpu);
}

// Story HUD per player. LytMissionHandler::init (02488B94, handler h in r31) fills layout
// parameter block A (r1+8) with the TV target [h+0x28], builds Msn_MainTV_00 (028CD9F0(0x244),
// ctor 02489BEC, init = vtable+0x34(obj, block), stored at h+0x3C), then the level-wide
// layouts with block A, and Msn_MainDRC_00 with block B (r1+0xB0, target [h+0x2C]). With two
// players, player 2's screen gets its own Msn_MainTV_00 built from block B, the level-wide
// layouts go to both screens, and Msn_MainDRC_00 gets no target.
constexpr uint32_t kMsnDrcBlockTarget = 0x02488D28u; // 02783D90 return address for block B
constexpr uint32_t kMsnDrcCourseClear = 0x02488D8Cu; // the second screen's CourseClear
constexpr uint32_t kLytTargetCount = 0x14u;          // targets in a parameter block
constexpr uint32_t kAlloc = 0x028CD9F0u, kMainTvCtor = 0x02489BECu, kMainTvSize = 0x244u;
constexpr uint32_t kAmiiboTimerCtor = 0x0248B5E8u, kAmiiboTimerReturn = 0x02488C20u;
constexpr uint32_t kHudFeed = 0x023FCBBCu;

void create_second_main_tv(CPUState* cpu, uint32_t block) {
    uint8_t* mem = cpu->mem;
    uint32_t obj = 0;
    if (!call_guest(cpu, kAlloc, {kMainTvSize}, &obj) || !obj || !rbrew_memory_accessible(mem, obj, kMainTvSize))
        return;
    for (uint32_t i = 0; i < kMainTvSize; i += 4) rbrew_write32(mem, obj + i, 0); // as player 1's (cleared heap)
    call_guest(cpu, kMainTvCtor, {obj});
    const uint32_t vtable = rbrew_read32(mem, obj);
    const uint32_t init =
        vtable && rbrew_memory_accessible(mem, vtable + 0x34, 4) ? rbrew_read32(mem, vtable + 0x34) : 0;
    if (init && call_guest(cpu, init, {obj, block})) s_main_tv2 = obj;
}

// Runs right after player 1's Msn_MainTV_00 init: from here on block A also targets the
// second screen, so the level-wide layouts show on both.
void amiibo_timer_ctor(CPUState* cpu, uint32_t, HostFunc original) {
    if (cpu->lr == kAmiiboTimerReturn && coop() && rbrew_control_defaults::active_scene_type(cpu->mem) == 1) {
        const uint32_t handler = cpu->r[31];
        call_guest(cpu, kLytAddTarget, {cpu->r[1] + 8, rbrew_read32(cpu->mem, handler + 0x2C), 1u});
    }
    original(cpu);
}

// Level-wide HUD state (sunken scroll +0x225, door keys +0x234, eggs and lives in the info
// object [tv+0x21C]) only reaches player 1's Msn_MainTV_00 through the handler's forwarders.
// It is copied to player 2's with the game's own setters, which cache the value and return
// early when it is unchanged, so each animation plays once per change.
void mirror_level_hud(CPUState* cpu, uint32_t handler, uint32_t tv1, uint32_t tv2, bool both_shown) {
    uint8_t* mem = cpu->mem;
    constexpr uint32_t kSetScrollAnimated = 0x0248A030u; // (tv, u8): pickup, plays GetOldDocuments
    constexpr uint32_t kSetScrollStatic = 0x024893C4u;   // (handler, u8) forwarder: level start / retry
    constexpr uint32_t kSetKeys = 0x0248A0F4u;           // (tv, n)
    constexpr uint32_t kSetEggs = 0x024C39A0u;           // (info, n), needs info+0x10 bit 0x10
    constexpr uint32_t kSetLivesTarget = 0x024C38DCu;    // (info, n), needs bit 0x4
    constexpr uint32_t kSetLivesStatic = 0x024C3784u;    // (info, n), level start
    constexpr uint32_t kPaneAnimClamp = 0x0248A268u, kPaneAnimStop = 0x0248A280u; // +0x240
    const uint8_t scroll = rbrew_read8(mem, tv1 + 0x225);
    if (rbrew_read8(mem, tv2 + 0x225) != scroll) {
        if (scroll && both_shown) {
            call_guest(cpu, kSetScrollAnimated, {tv2, scroll});
        } else {
            const uint32_t saved = rbrew_read32(mem, handler + 0x3C);
            rbrew_write32(mem, handler + 0x3C, tv2);
            call_guest(cpu, kSetScrollStatic, {handler, scroll});
            rbrew_write32(mem, handler + 0x3C, saved);
        }
    }
    const uint32_t keys = rbrew_read32(mem, tv1 + 0x234);
    if (rbrew_read32(mem, tv2 + 0x234) != keys) call_guest(cpu, kSetKeys, {tv2, keys});
    const uint32_t info1 = rbrew_read32(mem, tv1 + 0x21C), info2 = rbrew_read32(mem, tv2 + 0x21C);
    if (info1 && info2 && rbrew_memory_accessible(mem, info1, 0x48) && rbrew_memory_accessible(mem, info2, 0x48)) {
        const uint32_t flags = rbrew_read32(mem, info2 + 0x10);
        const uint32_t eggs = rbrew_read32(mem, info1 + 0x44);
        if ((flags & 0x10u) && rbrew_read32(mem, info2 + 0x44) != eggs) call_guest(cpu, kSetEggs, {info2, eggs});
        if (flags & 0x4u) {
            const uint32_t shown = rbrew_read32(mem, info1 + 0x34), target = rbrew_read32(mem, info1 + 0x38);
            if (!both_shown && rbrew_read32(mem, info2 + 0x34) != shown)
                call_guest(cpu, kSetLivesStatic, {info2, shown});
            if (rbrew_read32(mem, info2 + 0x38) != target) call_guest(cpu, kSetLivesTarget, {info2, target});
        }
    }
    const uint32_t anim1 = rbrew_read32(mem, tv1 + 0x240), anim2 = rbrew_read32(mem, tv2 + 0x240);
    if (anim1 == 0 && anim2 != 0)
        call_guest(cpu, kPaneAnimStop, {tv2});
    else if (anim1 == 1 && anim2 > 1)
        call_guest(cpu, kPaneAnimClamp, {tv2});
}

// The layout manager draws every layout that is active (+0x118 bit 0x2 clear) on the
// targets in its mask (+0x11C). At mission start the scene (025FDA78 -> 024890F8) attaches
// and shows the handler's Msn_MainTV_00 and Msn_MainDRC_00 directly; player 2's MainTV is
// not the handler's, so its state follows player 1's: appear (vtable +0x164, then 02489CC0
// starts its pane animations), fade out (+0x16C), sleep (+0x94). Msn_MainDRC_00 (GamePad
// frame and mission number) is detached from the second screen with 028B80BC.
constexpr uint32_t kLytFlags = 0x118u, kLytMask = 0x11Cu, kLytHidden = 0x2u, kLytState = 0x1E8u;
constexpr uint32_t kLytDetach = 0x028B80BCu, kMainTvStartAnims = 0x02489CC0u;
void mirror_hud_state(CPUState* cpu, uint32_t handler, uint32_t tv1) {
    uint8_t* mem = cpu->mem;
    const uint32_t tv2 = s_main_tv2;
    const auto ok = [&](uint32_t o) { return o && rbrew_memory_accessible(mem, o, kMainTvSize); };
    const auto hidden = [&](uint32_t o) { return (rbrew_read32(mem, o + kLytFlags) & kLytHidden) != 0; };
    const auto state = [&](uint32_t o) { return rbrew_read8(mem, o + kLytState); }; // 0 sleep, 1-3 in, 4 fade out
    const auto vcall = [&](uint32_t o, uint32_t off) {
        const uint32_t vt = rbrew_read32(mem, o);
        if (vt && rbrew_memory_accessible(mem, vt + off, 4)) call_guest(cpu, rbrew_read32(mem, vt + off), {o});
    };
    if (ok(tv1) && ok(tv2)) {
        const uint8_t s1 = state(tv1), s2 = state(tv2);
        if (!hidden(tv1) && hidden(tv2) && s1 >= 1 && s1 <= 3) {
            vcall(tv2, 0x164);
            if (rbrew_read32(mem, tv1 + 0x240) != 0) { // player 1's came through 024890F8 (pane animations running)
                const uint32_t info = rbrew_read32(mem, 0x101DD330u);
                if (info) call_guest(cpu, kMainTvStartAnims, {tv2, info + 0x1C});
            }
        } else if (s1 == 4 && !hidden(tv2) && s2 != 4 && s2 != 0) {
            vcall(tv2, 0x16C);
        } else if (hidden(tv1) && !hidden(tv2)) {
            vcall(tv2, 0x94);
        }
        mirror_level_hud(cpu, handler, tv1, tv2, !hidden(tv1) && !hidden(tv2));
    }
    const uint32_t second = rbrew_read32(mem, handler + 0x2C), drc = rbrew_read32(mem, handler + 0x40);
    if (second < 32 && ok(drc) && (rbrew_read32(mem, drc + kLytMask) & (1u << second)))
        call_guest(cpu, kLytDetach, {drc, second, 1u});
}

// 023FCBBC(scene) feeds the HUD each frame from the player of view 0 ([PlayerMgr+0x2B0] is
// a player index, PlayerMgr = [[0x101E2814]+0x268]) into the handler's Msn_MainTV_00
// ([[scene+0x238]+0x3C]); scene+0x244 keeps the last gauge value. A second pass feeds
// player 2's HUD: second MainTV, view 1's player, its own last value, and for the "press
// R" special prompt (02488DE8 reads [h+0x5C]) the controller bound to view 1's camera.
void hud_feed(CPUState* cpu, uint32_t, HostFunc original) {
    const CPUState entry = *cpu;
    original(cpu);
    if (!s_main_tv2 || !coop() || rbrew_control_defaults::active_scene_type(cpu->mem) != 1) return;
    uint8_t* mem = cpu->mem;
    const uint32_t scene = entry.r[3];
    const uint32_t handler = rbrew_read32(mem, scene + 0x238);
    const uint32_t mgr_holder = rbrew_read32(mem, 0x101E2814u);
    const uint32_t mgr = mgr_holder ? rbrew_read32(mem, mgr_holder + 0x268) : 0;
    if (!handler || !mgr || !rbrew_memory_accessible(mem, handler + 0x3C, 0x24) ||
        !rbrew_memory_accessible(mem, mgr + 0x2B0, 8))
        return;
    const CPUState after = *cpu;
    const uint32_t main_tv = rbrew_read32(mem, handler + 0x3C), view0 = rbrew_read32(mem, mgr + 0x2B0);
    const double last = rbrew_read_f32(mem, scene + 0x244);
    const auto bound = [&](uint32_t slot) {
        const uint32_t camera = rbrew_memory_accessible(mem, scene + slot, 4) ? rbrew_read32(mem, scene + slot) : 0;
        return camera && rbrew_memory_accessible(mem, camera + 8, 4) ? rbrew_read32(mem, camera + 8) : 0u;
    };
    const uint32_t pad = rbrew_read32(mem, handler + 0x5C), pad1 = bound(0x280), pad2 = bound(0x284);
    const bool swap_pad = pad && pad == pad1 && pad2 && pad2 != pad1;
    rbrew_write32(mem, handler + 0x3C, s_main_tv2);
    rbrew_write32(mem, mgr + 0x2B0, rbrew_read32(mem, mgr + 0x2B4));
    rbrew_write_f32(mem, scene + 0x244, s_p2_hud_last);
    if (swap_pad) rbrew_write32(mem, handler + 0x5C, pad2);
    *cpu = entry;
    original(cpu);
    if (swap_pad) rbrew_write32(mem, handler + 0x5C, pad);
    s_p2_hud_last = float(rbrew_read_f32(mem, scene + 0x244));
    rbrew_write_f32(mem, scene + 0x244, last);
    rbrew_write32(mem, mgr + 0x2B0, view0);
    rbrew_write32(mem, handler + 0x3C, main_tv);
    mirror_hud_state(cpu, handler, main_tv);
    *cpu = after;
}

// 02783D90(params, target_id, flag) registers a display target on a layout init parameter
// block. Targets: 6 TV HUD, 8 second-screen HUD, 18 TV overlay, 20 second-screen overlay.
void lyt_add_target(CPUState* cpu, uint32_t, HostFunc original) {
    const int kind = rbrew_control_defaults::active_scene_type(cpu->mem);
    if (kind == 6) {
        if (cpu->lr == 0x024827B4u) { // Dojo HUD handler init
            s_dojo_hud = cpu->r[27];
            s_label_layout = s_p1_label = 0;
        } else if (s_dojo_hud && !s_p1_label) {
            // A later layout of the Dojo init: the HUD is built and the game allocates
            // layouts itself here, so the label panes are made now.
            const CPUState saved = *cpu;
            dojo_player_labels(cpu, true);
            *cpu = saved;
        }
    }
    if (!coop() || !coop_level(kind)) return original(cpu);
    if (kind == 3) {
        // Octo Valley: player 2 sees the TV layouts on its screen, not the GamePad menu.
        const uint32_t target = cpu->r[4];
        if (target == kLytSecondHud || target == kLytSecondOverlay) return;
        const CPUState entry = *cpu;
        original(cpu);
        if (target == kLytTvHud || target == kLytTvOverlay) {
            const CPUState after = *cpu;
            *cpu = entry;
            cpu->r[4] = target == kLytTvHud ? kLytSecondHud : kLytSecondOverlay;
            original(cpu);
            *cpu = after;
        }
        return;
    }
    if (cpu->lr == kMsnDrcBlockTarget) {
        // Block B now holds the second screen's target: build player 2's own
        // Msn_MainTV_00 with it, then empty the block so Msn_MainDRC_00 is not drawn.
        const uint32_t params = cpu->r[3];
        original(cpu);
        create_second_main_tv(cpu, params);
        rbrew_write32(cpu->mem, params + kLytTargetCount, 0);
        return;
    }
    if (cpu->lr == kMsnDrcCourseClear) return;
    original(cpu);
}

void hook(uint32_t address, RbrewGuestHook fn) {
    if (!rbrew_set_guest_hook(address, fn)) RBREW_LOG_ERROR("coop", "hook %08X unavailable\n", address);
}
} // namespace

void coop_install() {
    hook(kControllerBindingName, controller_binding_name);
    hook(kSceneCtor, scene_ctor);
    hook(kPlayerSetup, player_setup);
    hook(kIsDuel, is_duel);
    hook(kIsDuel2, is_duel2);
    hook(kCreate3DLayer, create_3d_layer);
    hook(kAttachLayer, attach_layer);
    hook(kCubemapRender, cubemap_render);
    hook(kCubemapUpdate, cubemap_update);
    hook(kSpawnSetup, spawn_setup);
    hook(kBindControllers, bind_controllers);
    hook(kReleasePlayer, release_player);
    hook(kLayer3DCubemapInit, layer3d_cubemap_init);
    hook(kLytAddTarget, lyt_add_target);
    hook(kAmiiboTimerCtor, amiibo_timer_ctor);
    hook(kHudFeed, hud_feed);
}
#else
void coop_install() {}
#endif

namespace rbrew_coop {
namespace {
std::atomic<bool> s_two_views{false};
std::atomic<bool> s_dojo_match{false};

#if RBREW_GAME_REVISION == 288
// A story map has no start point for a second player, which then appears at the world
// origin and falls out of the level again and again. While it is there (x = z = 0), it is
// moved next to player 1. Position at +0x248, a second copy at +0x72C.
void place_player2(uint8_t* mem) {
    const uint32_t first = find_player(mem, 0), second = find_player(mem, 1);
    if (!first || !second || !rbrew_memory_accessible(mem, first, 0x740) ||
        !rbrew_memory_accessible(mem, second, 0x740))
        return;
    if (rbrew_read_f32(mem, second + 0x248) != 0.0f || rbrew_read_f32(mem, second + 0x250) != 0.0f) return;
    const float x = rbrew_read_f32(mem, first + 0x248), y = rbrew_read_f32(mem, first + 0x24C),
                z = rbrew_read_f32(mem, first + 0x250);
    if (x == 0.0f && z == 0.0f) return;
    for (uint32_t base : {0x248u, 0x72Cu}) {
        rbrew_write_f32(mem, second + base, x + 15.0f);
        rbrew_write_f32(mem, second + base + 4, y + 10.0f);
        rbrew_write_f32(mem, second + base + 8, z);
    }
}
#endif
} // namespace

void update_views(uint8_t* mem) {
    second_player_present(); // polls a local player 2's ZL+ZR join every frame
#if RBREW_GAME_REVISION == 288
    restore_second_view(mem); // in case 023FF448 did not run after the setup
    const int kind = rbrew_control_defaults::active_scene_type(mem);
    if (kind >= 0 && !coop_level(kind)) s_coop.store(false, std::memory_order_relaxed);
    const bool two = coop_scene(mem);
    s_two_views.store(two, std::memory_order_relaxed);
    s_dojo_match.store(kind == 6, std::memory_order_relaxed);
    // Battle Dojo draws the TV's and the second screen's 3D on alternate frames (0242A91C
    // writes interval 2 to [3D object+0x36FD]): 30 fps per player. Interval 1 draws both
    // every frame.
    if (kind == 6 && s_tv_3d_object && rbrew_memory_accessible(mem, s_tv_3d_object + 0x36F4, 12) &&
        rbrew_read32(mem, s_tv_3d_object + 0x36F4) >= 2 && rbrew_read8(mem, s_tv_3d_object + 0x36FD) > 1)
        rbrew_write8(mem, s_tv_3d_object + 0x36FD, 1);
    if (two) {
        const uint32_t dyn = rbrew_read32(mem, kDynResMgrPtr);
        if (dyn && rbrew_memory_accessible(mem, dyn + 0x48u, 4))
            rbrew_write32(mem, dyn + 0x48u, rbrew_read32(mem, dyn + 0x48u) & ~1u);
        place_player2(mem);
    }
#else
    (void)mem;
#endif
}
bool two_views() {
    return s_two_views.load(std::memory_order_relaxed);
}
bool dojo_match() {
    return s_dojo_match.load(std::memory_order_relaxed);
}
// An online guest joins by connecting. A second local controller only joins after
// pressing ZL+ZR (like Battle Dojo's join screen): a merely attached pad (Steam Input /
// DS4Windows virtual pads, wheels) must not turn solo play into co-op.
bool second_player_present() {
    if (rbrew_net::host_guest_connected()) return true;
#if defined(HAVE_SDL2)
    static std::atomic<SDL_GameController*> joined{nullptr};
    ControllerDevice* dev = get_controller(1);
    SDL_GameController* gc = dev && dev->gc && SDL_GameControllerGetAttached(dev->gc) ? dev->gc : nullptr;
    if (!gc) {
        joined.store(nullptr, std::memory_order_relaxed);
        return false;
    }
    if (joined.load(std::memory_order_relaxed) == gc) return true;
    constexpr Sint16 kPressed = 16384;
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > kPressed &&
        SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > kPressed) {
        joined.store(gc, std::memory_order_relaxed);
        return true;
    }
#endif
    return false;
}
} // namespace rbrew_coop

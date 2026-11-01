// player_anim.cpp - player animation hooks (Splatoon AGMP01 v16).
//
// Idle look-around (default on; RBREW_IDLE_LOOKAROUND=0 disables): the game
// ships WaitHold_Nrml_LookAround but never requests it. After a player has
// stood in WaitHold_Nrml for kIdleSeconds, slot 57 of its animation
// controller selects a cached look-around registration with the game's own
// 10-frame blend. After its 330 frames, or as soon as the game
// leaves slot 57, the original slot data is restored, so the
// game's own state machine is never changed. The controller holds exactly
// 193 slots (0x304-byte arrays), hence the temporary swap instead of a new
// slot. Only the existing slot's single skeletal handle is swapped; its
// allocation, count, variation and auxiliary tracks remain game-owned.
//
// Trace (RBREW_ANIM_LOG=<file>): logs registrations and animation switches;
// every hook runs the original function unchanged.
//
// Addresses and layouts were reverse-engineered from the v16 executable and
// one recorded trace (docs/player-anim-trace.md); names are descriptive.
#include "../../runtime/rebrewu_runtime.h"
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {
using Clock = std::chrono::steady_clock;
// Player animation table: 16-byte entries {name, ?, prefix, flags}; 193 slots.
constexpr uint32_t kAnimTable = 0x100D9AC0u, kAnimCount = 193;
constexpr uint32_t kMotionCtor = 0x026227C8u; // allocates/initializes the motion object
constexpr uint32_t kMotionUpdate =
    0x0262304Cu; // per-frame motion update; +0x20 current slot, +0x24 override, +0x28 controller
constexpr uint32_t kRegisterSlot = 0x025CFB18u;       // (controller, slot, name, r6, r7, r8); appends to slot
constexpr uint32_t kPlaySlot = 0x025D06A4u;           // (controller, slot, 1, 1, 0) with f1 = blend frames
constexpr uint32_t kCtorRegisterReturn = 0x0262297Cu; // the constructor's primary-controller registrations
constexpr uint32_t kControllerFuncs[] = {0x025CF3A0u, kRegisterSlot, 0x025D0BF8u, 0x025CFFC4u,
                                         kPlaySlot,   0x025D0750u,   0x025D07F8u, 0x025D08B0u};
constexpr uint32_t kWaitHoldSlot = 57;      // WaitHold_Nrml
constexpr unsigned kLookAroundFrames = 330; // FSKA frame count in Player00/01_anim
constexpr double kBlendFrames = 10.0, kIdleSeconds = 5.0;
constexpr char kLookAroundName[] = "WaitHold_Nrml_LookAround";
constexpr unsigned kMaxLines = 60000;

bool s_lookaround = false;
std::FILE* s_log = nullptr;
std::mutex s_log_mutex, s_idle_mutex;
unsigned s_lines = 0, s_ctor_calls = 0;
const auto s_start = Clock::now();
thread_local int t_in_ctor = 0, t_in_update = 0;
thread_local std::string t_update_calls; // controller calls of the current update, kept if the slot changed

void line(const char* format, ...) {
    std::lock_guard lock(s_log_mutex);
    if (!s_log || s_lines >= kMaxLines) return;
    ++s_lines;
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - s_start).count();
    std::fprintf(s_log, "%10.1f ", ms);
    va_list args;
    va_start(args, format);
    std::vfprintf(s_log, format, args);
    va_end(args);
    std::fputc('\n', s_log);
    if (s_lines % 64 == 0 || s_lines == kMaxLines) std::fflush(s_log);
}
uint32_t word(CPUState* cpu, uint32_t address) {
    return address && rbrew_memory_accessible(cpu->mem, address, 4) ? rbrew_read32(cpu->mem, address) : 0xDEADDEADu;
}
// Printable C string at a guest address, or empty.
std::string text(CPUState* cpu, uint32_t address) {
    std::string out;
    if (!address || !rbrew_memory_accessible(cpu->mem, address, 1)) return out;
    for (uint32_t i = 0; i < 64 && rbrew_memory_accessible(cpu->mem, address + i, 1); ++i) {
        const char c = char(cpu->mem[address + i]);
        if (!c) return out;
        if (c < 32 || c > 126) return {};
        out += c;
    }
    return out;
}
std::string anim_name(CPUState* cpu, uint32_t index) {
    if (index >= kAnimCount) return index == 0xFFFFFFFFu ? "-" : "?";
    return text(cpu, word(cpu, kAnimTable + index * 16));
}
std::string dump(CPUState* cpu, uint32_t address, unsigned bytes) {
    std::string out;
    char item[16];
    for (unsigned i = 0; i < bytes; i += 4) {
        std::snprintf(item, sizeof(item), "%08X ", word(cpu, address + i));
        out += item;
    }
    return out;
}
std::string registers(CPUState* cpu) {
    char out[400];
    std::snprintf(out, sizeof(out),
                  "lr=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X f1=%g f2=%g f3=%g s4='%s' s5='%s'", cpu->lr,
                  cpu->r[3], cpu->r[4], cpu->r[5], cpu->r[6], cpu->r[7], cpu->r[8], cpu->f[1], cpu->f[2], cpu->f[3],
                  text(cpu, cpu->r[4]).c_str(), text(cpu, cpu->r[5]).c_str());
    return out;
}

// --- Guest calls from a hook ------------------------------------------------
// Runs a guest function with the given integer arguments (r3...) and f1, then
// restores every register the interrupted caller can observe.
struct Registers {
    uint32_t r[32], cr[8], gqr[8], lr, ctr, xer, fpscr;
    double f[32], ps1[32];
};
uint32_t call_guest(CPUState* cpu, uint32_t address, std::initializer_list<uint32_t> args, double f1 = 0.0) {
    Registers saved;
    std::memcpy(saved.r, cpu->r, sizeof(saved.r)); std::memcpy(saved.cr, cpu->cr, sizeof(saved.cr));
    std::memcpy(saved.f, cpu->f, sizeof(saved.f)); std::memcpy(saved.ps1, cpu->ps1, sizeof(saved.ps1));
    std::memcpy(saved.gqr,cpu->gqr,sizeof saved.gqr);
    saved.lr = cpu->lr; saved.ctr = cpu->ctr; saved.xer = cpu->xer; saved.fpscr = cpu->fpscr;
    struct Restore { CPUState* cpu; const Registers& saved; ~Restore() {
    std::memcpy(cpu->r, saved.r, sizeof(saved.r)); std::memcpy(cpu->cr, saved.cr, sizeof(saved.cr));
    std::memcpy(cpu->f, saved.f, sizeof(saved.f)); std::memcpy(cpu->ps1, saved.ps1, sizeof(saved.ps1));
    std::memcpy(cpu->gqr,saved.gqr,sizeof saved.gqr);
    cpu->lr = saved.lr; cpu->ctr = saved.ctr; cpu->xer = saved.xer; cpu->fpscr = saved.fpscr;
    }} restore{cpu,saved};
    unsigned n = 3;
    for (uint32_t value : args) cpu->r[n++] = value;
    cpu->f[1] = f1;
    cpu->ps1[1] = f1;
    cpu->lr = 0; // never a constructor call site for the registration hook
    rbrew_call(cpu, address);
    const uint32_t result = cpu->r[3];
    return result;
}

// --- Idle look-around ---------------------------------------------------------
// Six parallel arrays: skeletal handle-list pointer, variation count, three
// auxiliary handle-list pointers, and the selected variation. Save the entry
// in every array, including absent auxiliary arrays, as one registration.
constexpr uint32_t kSlotFields[] = {0xCu, 0x10u, 0x14u, 0x18u, 0x1Cu, 0x20u};
using Slot = std::array<uint32_t, 6>;
struct WaitHoldRegistration {
    Slot normal{};
    uint32_t skeletal = 0xFFFFFFFFu, alternate = 0xFFFFFFFFu;
    bool prepared = false, available = false;
};
struct Idle {
    Clock::time_point since{};
    unsigned frames = 0;
    bool idle = false, playing = false, played = false;
};
std::unordered_map<uint32_t, WaitHoldRegistration> s_wait_hold; // primary controller -> its slot-57 registration
std::unordered_map<uint32_t, Idle> s_idle;                      // motion object

// Resolve an exact dictionary key in the model's already loaded resources.
// This is the same resource order and packed handle as 02983BC4, using a
// bounded dictionary walk instead of its Patricia-tree name search.
uint32_t find_skeletal_clip(CPUState* cpu, uint32_t controller, const char* name) {
    constexpr uint32_t missing = 0xFFFFFFFFu;
    const uint32_t model = word(cpu, controller + 0x90);
    line("lookaround lookup controller=%08X name=%s model=%08X", controller, name, model);
    if (!model || !rbrew_memory_accessible(cpu->mem, model, 0xA8)) return missing;
    const uint32_t resources = word(cpu, model + 0xA4);
    if (!resources || !rbrew_memory_accessible(cpu->mem, resources, 0x80)) return missing;
    const uint32_t count = word(cpu, resources + 0x74), files = word(cpu, resources + 0x7C);
    line("lookaround resources=%08X files=%08X count=%u", resources, files, count);
    if (!files || count > 64 || !rbrew_memory_accessible(cpu->mem, files, uint64_t(count) * 4)) return missing;
    uint32_t base_index = 0;
    for (uint32_t file = 0; file < count; ++file) {
        const uint32_t wrapper = word(cpu, files + file * 4);
        if (!wrapper || !rbrew_memory_accessible(cpu->mem, wrapper, 0x20)) return missing;
        const uint32_t fres = word(cpu, wrapper + 0x1C);
        line("lookaround file=%u wrapper=%08X fres=%08X magic=%08X", file, wrapper, fres, word(cpu, fres));
        if (!fres || !rbrew_memory_accessible(cpu->mem, fres, 0x58) || word(cpu, fres) != 0x46524553u) return missing;
        const uint32_t clips = rbrew_read16(cpu->mem, fres + 0x54);
        const uint32_t relative = word(cpu, fres + 0x28);
        if (relative) {
            const uint32_t dictionary = fres + 0x28 + relative;
            if (!rbrew_memory_accessible(cpu->mem, dictionary, uint64_t(clips) * 16 + 24) ||
                word(cpu, dictionary + 4) != clips)
                return missing;
            for (uint32_t clip = 0; clip < clips; ++clip) {
                const uint32_t node = dictionary + 24 + clip * 16;
                const uint32_t key_offset = word(cpu, node + 8);
                if (!key_offset || text(cpu, node + 8 + key_offset) != name) continue;
                const uint32_t data_offset = word(cpu, node + 12);
                if (!data_offset || word(cpu, node + 12 + data_offset) != 0x46534B41u || base_index + clip >= 0xFFFFu)
                    return missing;
                // Skeletal handles have type 0 in the upper half. The lower
                // half is cumulative across resource files, not file-local.
                return base_index + clip;
            }
        }
        base_index += clips;
        if (base_index >= 0xFFFFu) return missing;
    }
    return missing;
}

bool read_slot(CPUState* cpu, uint32_t controller, Slot& slot) {
    const uint32_t tracks = word(cpu, controller + 0x98);
    if (tracks < 1 || tracks > 3) return false;
    for (unsigned i = 0; i < slot.size(); ++i) {
        const uint32_t array = word(cpu, controller + kSlotFields[i]);
        if (!array) {
            if (i < 2 + tracks || i == 5) return false;
            slot[i] = 0;
            continue;
        }
        if (uint64_t(array) + kWaitHoldSlot * 4 + 4 > UINT32_MAX ||
            !rbrew_memory_accessible(cpu->mem, array + kWaitHoldSlot * 4, 4))
            return false;
        slot[i] = word(cpu, array + kWaitHoldSlot * 4);
    }
    return true;
}
void select_skeletal_clip(CPUState* cpu, const WaitHoldRegistration& r, bool alternate) {
    rbrew_write32(cpu->mem, r.normal[0], alternate ? r.alternate : r.skeletal);
}
bool prepare_lookaround(CPUState* cpu, uint32_t controller, WaitHoldRegistration& r) {
    if (r.prepared) return r.available;
    r.prepared = true;
    if (!read_slot(cpu, controller, r.normal) || r.normal[1] != 1 || !r.normal[0] ||
        !rbrew_memory_accessible(cpu->mem, r.normal[0], 4)) {
        line("lookaround controller=%08X invalid original slot count=%u handles=%08X", controller, r.normal[1],
             r.normal[0]);
        return false;
    }
    r.skeletal = word(cpu, r.normal[0]);
    const uint32_t normal = find_skeletal_clip(cpu, controller, "WaitHold_Nrml");
    r.alternate = find_skeletal_clip(cpu, controller, kLookAroundName);
    r.available = normal != 0xFFFFFFFFu && normal == r.skeletal && r.alternate != 0xFFFFFFFFu;
    line("lookaround controller=%08X direct normal=%08X registered=%08X alternate=%08X handles=%08X available=%u",
         controller, normal, r.skeletal, r.alternate, r.normal[0], unsigned(r.available));
    if (s_log) std::fflush(s_log);
    return r.available;
}
void play_wait_hold(CPUState* cpu, uint32_t controller) {
    call_guest(cpu, kPlaySlot, {controller, kWaitHoldSlot, 1, 1, 0}, kBlendFrames);
}

// After the game's motion update for `motion` (all game state already final).
void idle_lookaround(CPUState* cpu, uint32_t motion) {
    const uint32_t current = word(cpu, motion + 0x20), override_slot = word(cpu, motion + 0x24);
    const uint32_t controller = word(cpu, motion + 0x28);
    std::lock_guard lock(s_idle_mutex);
    const auto registration = s_wait_hold.find(controller);
    if (registration == s_wait_hold.end()) return;
    Idle& state = s_idle[motion];
    const auto now = Clock::now();
    if (current != kWaitHoldSlot || override_slot != 0xFFFFFFFFu) {
        // The game switched away: slot 57 must hold WaitHold_Nrml again.
        if (state.playing) {
            select_skeletal_clip(cpu, registration->second, false);
            line("lookaround motion=%08X interrupted current=%u override=%d", motion, current, int32_t(override_slot));
        }
        state = Idle{};
        return;
    }
    if (!state.idle) {
        state.idle = true;
        state.since = now;
    }
    if (state.playing) {
        if (++state.frames < kLookAroundFrames) return;
        select_skeletal_clip(cpu, registration->second, false);
        play_wait_hold(cpu, controller);
        state.playing = false;
        line("lookaround motion=%08X finished", motion);
        return;
    }
    if (state.played || std::chrono::duration<double>(now - state.since).count() < kIdleSeconds) return;
    state.played = true;
    if (!prepare_lookaround(cpu, controller, registration->second)) {
        // This model has no such clip: keep its original animation.
        line("lookaround motion=%08X controller=%08X: clip unavailable", motion, controller);
        return;
    }
    select_skeletal_clip(cpu, registration->second, true);
    play_wait_hold(cpu, controller);
    state.playing = true;
    state.frames = 0;
    line("lookaround motion=%08X controller=%08X started", motion, controller);
}

// --- Hooks --------------------------------------------------------------------
void motion_ctor(CPUState* cpu, uint32_t address, HostFunc original) {
    const unsigned call = ++s_ctor_calls;
    const uint32_t owner = cpu->r[4];
    if (s_log && call <= 8) line("ctor#%u enter %s", call, registers(cpu).c_str());
    ++t_in_ctor;
    original(cpu);
    --t_in_ctor;
    const uint32_t motion = cpu->r[3];
    if (s_lookaround) {
        std::lock_guard lock(s_idle_mutex);
        s_idle.erase(motion);
        const uint32_t primary = word(cpu, motion + 0x28);
        if (auto registration = s_wait_hold.find(primary); registration != s_wait_hold.end())
            prepare_lookaround(cpu, primary, registration->second);
    }
    if (!s_log || call > 8) return;
    line("ctor#%u motion=%08X owner=%08X motion[0..0x40]=%s", call, motion, owner, dump(cpu, motion, 0x40).c_str());
    line("ctor#%u owner+6A8=%08X owner+A0=%08X owner+5C=%08X", call, word(cpu, owner + 0x6A8), word(cpu, owner + 0xA0),
         word(cpu, owner + 0x5C));
    for (uint32_t field : {0x28u, 0x2Cu}) {
        const uint32_t controller = word(cpu, motion + field);
        line("ctor#%u controller+%X=%08X [0..0x80]=%s", call, field, controller, dump(cpu, controller, 0x80).c_str());
    }
}

void motion_update(CPUState* cpu, uint32_t address, HostFunc original) {
    const uint32_t motion = cpu->r[3];
    const uint32_t before[2] = {word(cpu, motion + 0x20), word(cpu, motion + 0x24)};
    ++t_in_update;
    t_update_calls.clear();
    original(cpu);
    --t_in_update;
    if (s_lookaround) idle_lookaround(cpu, motion);
    if (!s_log) return;
    const uint32_t after[2] = {word(cpu, motion + 0x20), word(cpu, motion + 0x24)};
    if (before[0] == after[0] && before[1] == after[1]) return;
    line("anim motion=%08X current %u(%s)->%u(%s) override %d(%s)->%d(%s)", motion, before[0],
         anim_name(cpu, before[0]).c_str(), after[0], anim_name(cpu, after[0]).c_str(), int32_t(before[1]),
         anim_name(cpu, before[1]).c_str(), int32_t(after[1]), anim_name(cpu, after[1]).c_str());
    if (!t_update_calls.empty()) line("calls %s", t_update_calls.c_str());
}

void controller(CPUState* cpu, uint32_t address, HostFunc original) {
    // The constructor's own WaitHold_Nrml registration, remembered per controller.
    if (s_lookaround && address == kRegisterSlot && t_in_ctor && cpu->lr == kCtorRegisterReturn &&
        cpu->r[4] == kWaitHoldSlot && text(cpu, cpu->r[5]) == "WaitHold_Nrml") {
        std::lock_guard lock(s_idle_mutex);
        s_wait_hold[cpu->r[3]] = {};
    }
    if (!s_log || (!t_in_ctor && !t_in_update)) {
        original(cpu);
        return;
    }
    const std::string args = registers(cpu);
    original(cpu);
    if (t_in_ctor && s_ctor_calls <= 2)
        line("  ctor %08X(%s) -> r3=%08X f1=%g", address, args.c_str(), cpu->r[3], cpu->f[1]);
    else if (t_in_update && t_update_calls.size() < 4000) {
        char head[64];
        std::snprintf(head, sizeof(head), "\n    %08X(", address);
        char tail[48];
        std::snprintf(tail, sizeof(tail), ") -> r3=%08X", cpu->r[3]);
        t_update_calls += head + args + tail;
    }
}
} // namespace

#ifdef RBREW_ADVANCED_TESTS
// Advance test idle timers without sleeping or changing the production clock.
uint32_t player_anim_test_find_skeletal(CPUState* cpu, uint32_t controller, const char* name) {
    return find_skeletal_clip(cpu, controller, name);
}
void player_anim_test_advance_idle(double seconds) {
    std::lock_guard lock(s_idle_mutex);
    for (auto& [motion, state] : s_idle)
        state.since -= std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}
#endif

void player_anim_register() {
#if RBREW_GAME_REVISION == 288
    // These optional hooks have only been validated for the base RPX.
    return;
#endif
    const char* lookaround = std::getenv("RBREW_IDLE_LOOKAROUND");
    s_lookaround = !(lookaround && lookaround[0] == '0');
    if (const char* path = std::getenv("RBREW_ANIM_LOG"); path && *path) s_log = std::fopen(path, "w");
    if (!s_lookaround && !s_log) return;
    bool ok = rbrew_set_guest_hook(kMotionCtor, motion_ctor) && rbrew_set_guest_hook(kMotionUpdate, motion_update);
    if (s_log)
        for (uint32_t address : kControllerFuncs) ok = rbrew_set_guest_hook(address, controller) && ok;
    else
        ok = rbrew_set_guest_hook(kRegisterSlot, controller) && ok;
    line("player animation hooks %s; idle look-around %s (direct handles v4); table %08X count %u",
         ok ? "armed" : "INCOMPLETE", s_lookaround ? "on" : "off", kAnimTable, kAnimCount);
    if (s_log) std::fflush(s_log);
}


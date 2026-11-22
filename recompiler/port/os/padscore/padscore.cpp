// padscore — Wii U Pro Controller / Classic Controller / Wii Remote implementation
// KPADInitEx, KPADReadEx, WPADProbe, etc. for Local Multiplayer / Battle Dojo

#include "../os_common.h"
#include "pro_pad.h"
#include "../../runtime/native_control_defaults.h"
#include "../../runtime/coop_views.h"
#include <algorithm>

static void KPADInit(CPUState* cpu) {
    (void)cpu;
}
static void KPADInitEx(CPUState* cpu) {
    (void)cpu;
}
static void KPADShutdown(CPUState* cpu) {
    (void)cpu;
}

// Player 2: the online guest while one is connected, else the second local controller.
// The plaza and Octo Valley (scene kinds 2 and 3) without a second view have no
// second player, but the game still reads this channel there: the guest's stick
// would steer the host's Inkling.
static bool read_player2(uint8_t* mem, rbrew_net::ProPad& pad) {
    const int kind = rbrew_control_defaults::active_scene_type(mem);
    const bool hub = (kind == 2 || kind == 3) && !rbrew_coop::two_views();
    if (rbrew_net::host_remote_pad(pad)) {
        if (!hub) return true;
        pad = rbrew_net::ProPad{};
        return false;
    }
#if defined(HAVE_SDL2)
    ControllerDevice* dev = get_controller(1);
    if (dev && dev->gc && SDL_GameControllerGetAttached(dev->gc)) {
        pad = pro_pad_from_controller(dev->gc);
        return true;
    }
#endif
    return false;
}

// WPADProbe(chan, type*) → 0 (connected) or -1 (not connected)
static void WPADProbe(CPUState* cpu) {
    uint32_t chan = ARG0;
    uint32_t type_ptr = ARG1;
    rbrew_net::ProPad pad;
    if (chan == 0 && read_player2(MEM, pad)) {
        if (type_ptr && rbrew_memory_accessible(MEM, type_ptr, 4)) {
            rbrew_write32(MEM, type_ptr, 31u); // WPAD_EXT_PRO_CONTROLLER
        }
        RET = 0; // WPAD_ERR_NONE (connected!)
        return;
    }
    if (type_ptr && rbrew_memory_accessible(MEM, type_ptr, 4)) {
        rbrew_write32(MEM, type_ptr, 0u);
    }
    RET = (uint32_t)-1; // WPAD_ERR_NOT_READY / WPAD_ERR_NO_CONTROLLER
}

// KPADReadEx(chan, buffer, count) → samplesRead
// KPADError, written through KPADReadEx's fourth argument.
constexpr int32_t kKpadOk = 0, kKpadInvalidController = -2;

static void KPADReadEx(CPUState* cpu) {
    uint32_t chan = ARG0;
    uint32_t buf = ARG1;
    uint32_t count = ARG2;
    const uint32_t error_ptr = ARG3;
    const auto set_error = [&](int32_t error) {
        if (error_ptr && rbrew_memory_accessible(MEM, error_ptr, 4)) rbrew_write32(MEM, error_ptr, uint32_t(error));
    };

    if (!buf || count == 0 || !rbrew_memory_accessible(MEM, buf, 0xF0)) {
        set_error(kKpadInvalidController);
        RET = 0;
        return;
    }

    if (chan != 0) {
        // Battle Dojo uses WPAD / KPAD channel 0 for Player 2
        for (uint32_t i = 0; i < 0xF0; i += 4) rbrew_write32(MEM, buf + i, 0u);
        set_error(kKpadInvalidController);
        RET = 0;
        return;
    }

    rbrew_net::ProPad pad;
    if (read_player2(MEM, pad)) {
        for (uint32_t i = 0; i < 0xF0; i += 4) rbrew_write32(MEM, buf + i, 0u);
        const uint32_t pro_hold = pro_pad_hold_with_sticks(pad);
        const float lx = pad.lx, ly = pad.ly, rx = pad.rx, ry = pad.ry;

        static uint32_t s_p2_prev_hold = 0;
        uint32_t pro_trigger = pro_hold & ~s_p2_prev_hold;
        uint32_t pro_release = s_p2_prev_hold & ~pro_hold;
        s_p2_prev_hold = pro_hold;

        // Base fields
        rbrew_write32(MEM, buf + 0x00, pro_hold);
        rbrew_write32(MEM, buf + 0x04, pro_trigger);
        rbrew_write32(MEM, buf + 0x08, pro_release);

        // Extension fields
        rbrew_write8(MEM, buf + 0x5C, 31u); // extensionType = WPAD_EXT_PRO_CONTROLLER
        rbrew_write8(MEM, buf + 0x5D, 0u);  // error = 0
        rbrew_write8(MEM, buf + 0x5E, 1u);  // posValid = 1
        rbrew_write8(MEM, buf + 0x5F, 22u); // format = WPAD_FMT_PRO_CONTROLLER

        // Pro controller structure at offset +0x60
        rbrew_write32(MEM, buf + 0x60, pro_hold);
        rbrew_write32(MEM, buf + 0x64, pro_trigger);
        rbrew_write32(MEM, buf + 0x68, pro_release);
        write_guest_float(MEM, buf + 0x6C, lx);
        write_guest_float(MEM, buf + 0x70, ly);
        write_guest_float(MEM, buf + 0x74, rx);
        write_guest_float(MEM, buf + 0x78, ry);
        // KPADProStatus ends with two int32 fields, charging (+0x7C) and
        // wired (+0x80); ZL/ZR are digital buttons on the Pro Controller.
        // Writing trigger floats here reported a charging, wired pad.
        rbrew_write32(MEM, buf + 0x7C, 0u);
        rbrew_write32(MEM, buf + 0x80, 0u);

        set_error(kKpadOk);
        RET = 1; // 1 sample read
        return;
    }

    // Not connected: clear first entry
    for (uint32_t i = 0; i < 0xF0; i += 4) rbrew_write32(MEM, buf + i, 0u);
    set_error(kKpadInvalidController);
    RET = 0;
}

// KPADRead — same
static void KPADRead(CPUState* cpu) {
    KPADReadEx(cpu);
}

static void WPADIsEnabled(CPUState* cpu) {
    RET = 1;
}
static void WPADGetBatteryLevel(CPUState* cpu) {
    RET = 4;
} // 4 = full
static void WPADControlMotor(CPUState* cpu) {
    uint32_t chan = ARG0;
    uint32_t command = ARG1; // 0 = off, 1 = on
#if defined(HAVE_SDL2)
    ControllerDevice* dev = get_controller(chan == 0 ? 1 : 0);
    if (dev && dev->gc) {
        uint16_t strength = command ? 0xFFFF : 0;
        SDL_GameControllerRumble(dev->gc, strength, strength, 200);
    }
#endif
    (void)cpu;
}
static void WPADGetInfoAsync(CPUState* cpu) {
    (void)cpu;
}
static void WPADIsMplsAttached(CPUState* cpu) {
    RET = 0;
}

static void KPADGetUnifiedWpadStatus(CPUState* cpu) {
    RET = 0;
}
static void KPADSetPosPlayMode(CPUState* cpu) {
    (void)cpu;
}
static void KPADEnableDPD(CPUState* cpu) {
    (void)cpu;
}
static void KPADDisableDPD(CPUState* cpu) {
    (void)cpu;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
#include "padscore_addrs.h"

void padscore_register(CPUState* cpu) {
#define REG(name) rbrew_register_func(cpu, ADDR_##name, name)
    REG(KPADInitEx);
    REG(KPADReadEx);
    REG(WPADProbe);
    REG(WPADGetBatteryLevel);
    REG(WPADControlMotor);
    REG(WPADIsMplsAttached);
    REG(KPADEnableDPD);
    REG(KPADDisableDPD);
#undef REG
}

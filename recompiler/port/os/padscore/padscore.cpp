// padscore — Wii U Pro Controller / Classic Controller / Wii Remote implementation
// KPADInitEx, KPADReadEx, WPADProbe, etc. for Local Multiplayer / Battle Dojo

#include "../os_common.h"
#include "../vpad/game_controllers.h"
#include "../../runtime/mod_loader.h"
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

static void write_kpad_pro_status(uint8_t* mem, uint32_t buf, uint32_t pro_hold, float lx, float ly, float rx, float ry,
                                  uint32_t& prev_hold) {
    for (uint32_t i = 0; i < 0xF0; i += 4) rbrew_write32(mem, buf + i, 0u);

    constexpr float kThreshold = 0.5f;
    if (rx <= -kThreshold) pro_hold |= 0x00020000u;
    if (rx >= kThreshold) pro_hold |= 0x00080000u;
    if (ry >= kThreshold) pro_hold |= 0x00010000u;
    if (ry <= -kThreshold) pro_hold |= 0x00040000u;
    if (lx <= -kThreshold) pro_hold |= 0x00200000u;
    if (lx >= kThreshold) pro_hold |= 0x00800000u;
    if (ly >= kThreshold) pro_hold |= 0x00100000u;
    if (ly <= -kThreshold) pro_hold |= 0x00400000u;

    const uint32_t pro_trigger = pro_hold & ~prev_hold;
    const uint32_t pro_release = prev_hold & ~pro_hold;
    prev_hold = pro_hold;

    rbrew_write32(mem, buf + 0x00, pro_hold);
    rbrew_write32(mem, buf + 0x04, pro_trigger);
    rbrew_write32(mem, buf + 0x08, pro_release);

    rbrew_write8(mem, buf + 0x5C, 31u); // extensionType = WPAD_EXT_PRO_CONTROLLER
    rbrew_write8(mem, buf + 0x5D, 0u);  // error = 0
    rbrew_write8(mem, buf + 0x5E, 1u);  // posValid = 1
    rbrew_write8(mem, buf + 0x5F, 22u); // format = WPAD_FMT_PRO_CONTROLLER

    rbrew_write32(mem, buf + 0x60, pro_hold);
    rbrew_write32(mem, buf + 0x64, pro_trigger);
    rbrew_write32(mem, buf + 0x68, pro_release);
    write_guest_float(mem, buf + 0x6C, lx);
    write_guest_float(mem, buf + 0x70, ly);
    write_guest_float(mem, buf + 0x74, rx);
    write_guest_float(mem, buf + 0x78, ry);
    // KPADProStatus ends with two int32 fields, charging (+0x7C) and
    // wired (+0x80); ZL/ZR are digital buttons on the Pro Controller.
    rbrew_write32(mem, buf + 0x7C, 0u);
    rbrew_write32(mem, buf + 0x80, 0u);
}

// WPADProbe(chan, type*) → 0 (connected) or -1 (not connected)
static void WPADProbe(CPUState* cpu) {
    uint32_t chan = ARG0;
    uint32_t type_ptr = ARG1;
    if (rbrew_mod::has_kpad_providers()) {
        SplatModProPad mod_pad{};
        if (rbrew_mod::poll_kpad_provider(chan, &mod_pad)) {
            if (type_ptr && rbrew_memory_accessible(MEM, type_ptr, 4)) {
                rbrew_write32(MEM, type_ptr, 31u); // WPAD_EXT_PRO_CONTROLLER
            }
            RET = 0;
            return;
        }
    }
    if (chan == 0) {
#if defined(HAVE_SDL2)
        ControllerDevice* dev = get_controller(1); // Player 2 Pro Controller
        if (dev && dev->gc && SDL_GameControllerGetAttached(dev->gc)) {
            if (type_ptr && rbrew_memory_accessible(MEM, type_ptr, 4)) {
                rbrew_write32(MEM, type_ptr, 31u); // WPAD_EXT_PRO_CONTROLLER
            }
            RET = 0; // WPAD_ERR_NONE (connected!)
            return;
        }
#endif
    }
    if (type_ptr && rbrew_memory_accessible(MEM, type_ptr, 4)) {
        rbrew_write32(MEM, type_ptr, 0u);
    }
    RET = (uint32_t)-1; // WPAD_ERR_NOT_READY / WPAD_ERR_NO_CONTROLLER
}

// KPADReadEx(chan, buffer, count, error) → samplesRead
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

    if (rbrew_mod::has_kpad_providers()) {
        SplatModProPad mod_pad{};
        if (rbrew_mod::poll_kpad_provider(chan, &mod_pad)) {
            static uint32_t s_mod_prev_hold[4] = {};
            uint32_t dummy_prev = 0;
            uint32_t& prev_hold = (chan < 4) ? s_mod_prev_hold[chan] : dummy_prev;
            write_kpad_pro_status(MEM, buf, mod_pad.hold, mod_pad.lx, mod_pad.ly, mod_pad.rx, mod_pad.ry, prev_hold);
            set_error(kKpadOk);
            RET = 1;
            return;
        }
    }

    if (chan != 0) {
        // Battle Dojo uses WPAD / KPAD channel 0 for Player 2
        for (uint32_t i = 0; i < 0xF0; i += 4) rbrew_write32(MEM, buf + i, 0u);
        set_error(kKpadInvalidController);
        RET = 0;
        return;
    }

#if defined(HAVE_SDL2)
    ControllerDevice* dev = get_controller(1); // Player 2 Pro Controller
    if (dev && dev->gc && SDL_GameControllerGetAttached(dev->gc)) {
        SDL_GameController* gc = dev->gc;
        auto btn = [&](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(gc, b) != 0; };

        uint32_t pro_hold = 0;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_UP)) pro_hold |= 0x00000001u;       // UP
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) pro_hold |= 0x00000002u;     // LEFT
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) pro_hold |= 0x00004000u;     // DOWN
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) pro_hold |= 0x00008000u;    // RIGHT
        if (btn(SDL_CONTROLLER_BUTTON_Y)) pro_hold |= 0x00000008u;             // X
        if (btn(SDL_CONTROLLER_BUTTON_B)) pro_hold |= 0x00000010u;             // A
        if (btn(SDL_CONTROLLER_BUTTON_X)) pro_hold |= 0x00000020u;             // Y
        if (btn(SDL_CONTROLLER_BUTTON_A)) pro_hold |= 0x00000040u;             // B
        if (btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) pro_hold |= 0x00000200u; // R
        if (btn(SDL_CONTROLLER_BUTTON_START)) pro_hold |= 0x00000400u;         // PLUS
        if (btn(SDL_CONTROLLER_BUTTON_GUIDE)) pro_hold |= 0x00000800u;         // HOME
        if (btn(SDL_CONTROLLER_BUTTON_BACK)) pro_hold |= 0x00001000u;          // MINUS
        if (btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) pro_hold |= 0x00002000u;  // L
        if (btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK)) pro_hold |= 0x01000000u;    // STICK_R
        if (btn(SDL_CONTROLLER_BUTTON_LEFTSTICK)) pro_hold |= 0x02000000u;     // STICK_L

        int16_t lt_raw = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int16_t rt_raw = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (lt_raw > 16384) pro_hold |= 0x00000080u; // ZL
        if (rt_raw > 16384) pro_hold |= 0x00000004u; // ZR

        float lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
        float ly = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
        float rx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
        float ry = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
        shape_stick(lx, ly);
        shape_stick(rx, ry);

        static uint32_t s_p2_prev_hold = 0;
        write_kpad_pro_status(MEM, buf, pro_hold, lx, ly, rx, ry, s_p2_prev_hold);

        set_error(kKpadOk);
        RET = 1; // 1 sample read
        return;
    }
#endif

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

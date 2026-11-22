#pragma once
// Player 2 Pro Controller: SDL controller -> KPAD state, shared by the local
// controller path and the online guest.
#include "../vpad/game_controllers.h"
#include "../../runtime/netplay.h"
#include "../../runtime/port_settings.h"

#if defined(HAVE_SDL2)
inline rbrew_net::ProPad pro_pad_from_controller(SDL_GameController* gc) {
    auto btn = [&](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(gc, b) != 0; };
    uint32_t hold = 0;
    if (btn(SDL_CONTROLLER_BUTTON_DPAD_UP)) hold |= 0x00000001u;
    if (btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) hold |= 0x00000002u;
    if (btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) hold |= 0x00004000u;
    if (btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) hold |= 0x00008000u;
    if (btn(SDL_CONTROLLER_BUTTON_Y)) hold |= 0x00000008u;             // X
    if (btn(SDL_CONTROLLER_BUTTON_B)) hold |= 0x00000010u;             // A
    if (btn(SDL_CONTROLLER_BUTTON_X)) hold |= 0x00000020u;             // Y
    if (btn(SDL_CONTROLLER_BUTTON_A)) hold |= 0x00000040u;             // B
    if (btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) hold |= 0x00000200u; // R
    if (btn(SDL_CONTROLLER_BUTTON_START)) hold |= 0x00000400u;         // PLUS
    if (btn(SDL_CONTROLLER_BUTTON_GUIDE)) hold |= 0x00000800u;         // HOME
    if (btn(SDL_CONTROLLER_BUTTON_BACK)) hold |= 0x00001000u;          // MINUS
    if (btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) hold |= 0x00002000u;  // L
    if (btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK)) hold |= 0x01000000u;
    if (btn(SDL_CONTROLLER_BUTTON_LEFTSTICK)) hold |= 0x02000000u;
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16384) hold |= 0x00000080u;  // ZL
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16384) hold |= 0x00000004u; // ZR

    rbrew_net::ProPad pad;
    pad.hold = hold;
    pad.lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
    pad.ly = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
    pad.rx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
    pad.ry = -SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
    shape_stick(pad.lx, pad.ly);
    shape_stick(pad.rx, pad.ry);
    return pad;
}
#endif

// Pro Controller bits for the ESC menu's actions (port_settings.h order).
constexpr uint32_t kProActionButtons[rbrew_settings::kFirstMoveAction] = {
    0x00000010u, 0x00000040u, 0x00000008u, 0x00000020u, 0x00002000u, 0x00000200u, // A B X Y L R
    0x00000080u, 0x00000004u, 0x00000400u, 0x00001000u, 0x00000800u,              // ZL ZR + - HOME
    0x00000001u, 0x00004000u, 0x00000002u, 0x00008000u,                           // up down left right
    0x02000000u, 0x01000000u};                                                    // stick clicks
// Mouse counts per second for a full right stick, like the GamePad's
// keyboard/mouse path (0.005 per count each 60 Hz frame).
constexpr float kMouseStickPerCount = 0.005f / 60.0f;

// Keyboard and mouse as a Pro Controller, with the player's key bindings
// (scancodes, or kMouseBase + SDL mouse button): bound keys press buttons, the
// move keys push the left stick, mouse movement pushes the right stick.
inline rbrew_net::ProPad pro_pad_from_keyboard(const uint8_t* keys, int key_count, uint32_t mouse_buttons,
                                               float mouse_x_per_s, float mouse_y_per_s, const int* bindings,
                                               float mouse_scale) {
    auto bound = [&](int action) {
        const int binding = bindings[action];
        if (binding >= rbrew_settings::kMouseBase) {
            const int button = binding - rbrew_settings::kMouseBase;
            return button >= 1 && button <= 32 && (mouse_buttons & (1u << (button - 1))) != 0;
        }
        return binding >= 0 && binding < key_count && keys[binding] != 0;
    };
    rbrew_net::ProPad pad;
    for (int action = 0; action < rbrew_settings::kFirstMoveAction; ++action)
        if (bound(action)) pad.hold |= kProActionButtons[action];
    pad.lx = float(bound(rbrew_settings::kActMoveRight)) - float(bound(rbrew_settings::kActMoveLeft));
    pad.ly = float(bound(rbrew_settings::kActMoveUp)) - float(bound(rbrew_settings::kActMoveDown));
    if (pad.lx != 0.0f && pad.ly != 0.0f) {
        pad.lx *= 0.7071f;
        pad.ly *= 0.7071f;
    }
    const auto clamp = [](float v) { return v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v; };
    pad.rx = clamp(mouse_x_per_s * kMouseStickPerCount * mouse_scale);
    pad.ry = clamp(-mouse_y_per_s * kMouseStickPerCount * mouse_scale); // mouse up looks up
    return pad;
}

// Controller and keyboard together: buttons add up, moving by key wins over
// the left stick, the mouse adds to the right stick.
inline rbrew_net::ProPad merge_pads(const rbrew_net::ProPad& controller, const rbrew_net::ProPad& keyboard) {
    const auto clamp = [](float v) { return v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v; };
    rbrew_net::ProPad pad = controller;
    pad.hold |= keyboard.hold;
    if (keyboard.lx != 0.0f || keyboard.ly != 0.0f) {
        pad.lx = keyboard.lx;
        pad.ly = keyboard.ly;
    }
    pad.rx = clamp(pad.rx + keyboard.rx);
    pad.ry = clamp(pad.ry + keyboard.ry);
    return pad;
}

// Stick directions as buttons, as the Pro Controller reports them.
inline uint32_t pro_pad_hold_with_sticks(const rbrew_net::ProPad& pad) {
    constexpr float kThreshold = 0.5f;
    uint32_t hold = pad.hold;
    if (pad.rx <= -kThreshold) hold |= 0x00020000u;
    if (pad.rx >= kThreshold) hold |= 0x00080000u;
    if (pad.ry >= kThreshold) hold |= 0x00010000u;
    if (pad.ry <= -kThreshold) hold |= 0x00040000u;
    if (pad.lx <= -kThreshold) hold |= 0x00200000u;
    if (pad.lx >= kThreshold) hold |= 0x00800000u;
    if (pad.ly >= kThreshold) hold |= 0x00100000u;
    if (pad.ly <= -kThreshold) hold |= 0x00400000u;
    return hold;
}

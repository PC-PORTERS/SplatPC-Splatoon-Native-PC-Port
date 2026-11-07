// screen_mode.h — shared state for TV / GamePad dual-screen emulation
//
// The Wii U has two display outputs:
//   TV        — 1080p HDMI, full game rendering, mouse captured for camera
//   GamePad   — 854×480 touch screen, DRC rendering, mouse free for touch sim
//
// Toggle with Tab inside the SDL window.
#pragma once
#include <stdint.h>
#include <atomic>

enum class ScreenMode { TV, Gamepad };

// Current display mode — written by gx2_context, read by vpad
extern ScreenMode g_screen_mode;

// Simulated touch state — written by gx2_context on mouse click in Gamepad mode,
// read by VPADRead to populate VPADStatus.tpNormal / tpFiltered1 / tpFiltered2
struct TouchState {
    bool touched;
    uint16_t x; // 0..853  (GamePad screen width  – 1)
    uint16_t y; // 0..479  (GamePad screen height – 1)
};
extern TouchState g_touch_state;

// Touch from the phone page (pad_remote.cpp): bit 31 touched, x in bits 0..15, y in bits 16..30.
inline std::atomic<uint32_t> g_phone_touch{0};
// The touch VPADRead reports: the mouse first, else the phone.
inline TouchState current_touch() {
    if (g_touch_state.touched) return g_touch_state;
    const uint32_t p = g_phone_touch.load(std::memory_order_relaxed);
    if (p >> 31) return {true, uint16_t(p & 0xFFFF), uint16_t((p >> 16) & 0x7FFF)};
    return {false, 0, 0};
}

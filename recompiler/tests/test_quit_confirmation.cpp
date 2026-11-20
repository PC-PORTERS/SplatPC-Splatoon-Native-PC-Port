#include "os/gx2/vk/gx2_imgui_overlay.h"
#include "runtime/port_settings.h"
#include "rebrewu/logger.hpp"

#include <cstdlib>

#define SDL_MAIN_HANDLED
#include <SDL.h>

namespace {

void require(bool ok, const char* what) {
    if (ok) return;
    RBREW_LOG_ERROR("quit_confirm", "FAIL: %s\n", what);
    std::exit(1);
}

void test_initial_state() {
    require(!gx2_imgui::menu_open(), "menu starts closed");
    require(!gx2_imgui::is_quit_confirmation_open(), "quit popup starts closed");
    require(!gx2_imgui::consume_quit_request(), "no quit request initially");
    require(rbrew_settings::g_confirm_quit.load(), "confirm_quit defaults to true");
}

void test_open_confirmation() {
    gx2_imgui::open_quit_confirmation();
    require(gx2_imgui::menu_open(), "opening quit confirmation opens menu");
    require(gx2_imgui::is_quit_confirmation_open(), "is_quit_confirmation_open is true after open");
    require(gx2_imgui::consume_menu_changed(), "menu changed notification set");
    require(!gx2_imgui::consume_quit_request(), "quit request not triggered solely by opening dialog");
}

void test_escape_event_consumes() {
    SDL_Event esc_ev{};
    esc_ev.type = SDL_KEYDOWN;
    esc_ev.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
    const bool consumed = gx2_imgui::menu_event(esc_ev);
    require(consumed, "escape key event is consumed while confirmation is open");
}

void test_touchpad_event_consumes() {
    SDL_Event pad_ev{};
    pad_ev.type = SDL_CONTROLLERBUTTONDOWN;
    pad_ev.cbutton.button = SDL_CONTROLLER_BUTTON_TOUCHPAD;
    const bool consumed = gx2_imgui::menu_event(pad_ev);
    require(consumed, "touchpad button event is consumed while confirmation is open");
}

void test_disabled_confirm_quits_immediately() {
    rbrew_settings::g_confirm_quit.store(false);
    gx2_imgui::set_menu_open(false);
    gx2_imgui::open_quit_confirmation();
    require(gx2_imgui::consume_quit_request(), "quits immediately when confirmation is disabled");

    // Restore setting
    rbrew_settings::g_confirm_quit.store(true);
}

} // namespace

int main() {
    test_initial_state();
    test_open_confirmation();
    test_escape_event_consumes();
    test_touchpad_event_consumes();
    test_disabled_confirm_quits_immediately();
    RBREW_LOG_INFO("quit_confirm", "PASS: all quit confirmation tests passed\n");
    return 0;
}

// gx2_graphics_menu.h - the beginner graphics, weather, camera and cinema
// pages of the ImGui settings menu (gx2_imgui_overlay.cpp), with the
// graphics presets. Render thread only (every ImGui call runs there); game
// actions such as spawning go through gx2env::request().
#pragma once
#include "../../../runtime/port_settings.h"

namespace gx2_graphics_menu {

enum class Page { None, Graphics, Weather, Camera, Cinema };

// Rows for the main settings page: graphics preset, frame rate, G-Sync and
// the buttons that open the pages. Returns the page to open, or Page::None.
Page build_main_rows(rbrew_settings::Values& v, bool& dirty);

const char* title(Page page);

// The page's widgets; returns true when "Back" was pressed.
bool build_page(Page page);

} // namespace gx2_graphics_menu

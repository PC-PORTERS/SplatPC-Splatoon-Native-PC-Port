// gx2_graphics_menu.h - the graphics, camera, weather and cinema pages of
// the ImGui settings menu (gx2_imgui_overlay.cpp), with the graphics
// presets, and the rows its other pages take from here. Render thread only
// (every ImGui call runs there); game actions such as spawning go through
// gx2env::request().
#pragma once
#include "../../../runtime/port_settings.h"

namespace gx2_graphics_menu {

enum class Page { None, Graphics, Camera, Weather, Cinema };

// Frame rate and G-Sync, for the Display page; true when a value changed.
bool build_frame_rows(rbrew_settings::Values& v);

// Mouse aiming and sensitivity / controller gyro, above the key and button bindings.
void build_mouse_rows();
void build_gyro_rows();

const char* title(Page page);

// The page's widgets without a Back button (for tabbed/sidebar navigation).
void build_page_contents(Page page);

// The page's widgets; returns true when "Back" was pressed.
bool build_page(Page page);

} // namespace gx2_graphics_menu

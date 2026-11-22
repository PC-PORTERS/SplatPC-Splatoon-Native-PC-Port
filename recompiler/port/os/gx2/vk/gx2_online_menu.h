// gx2_online_menu.h - the "Online co-op" page of the settings menu (Esc):
// host a game (with automatic router port forwarding) or join a friend's.
// Render thread only.
#pragma once

namespace gx2_online_menu {

enum class Result { Stay, Back, Quit }; // Quit: the guest window started, this game closes

Result build_page();

// One line for the main settings page ("" when not hosting).
const char* status_line();

} // namespace gx2_online_menu

// gx2_imgui_overlay.h - Dear ImGui interface (docking branch): performance
// overlay (F3), debug window, the simple FPS readout and the port's settings
// menu (Esc). Both overlays start hidden: F3 or the settings menu shows the
// debug overlay, the settings menu or its own checkbox shows the simple one.
//
// Threading: the window event loop (GX2 thread) calls process_event(),
// menu_event() and the small atomic accessors below. The render thread, from
// inside gx2vk::present, calls note_fullscreen() state only through the
// mirror, plus ensure_ready(), begin_frame() and render_draw_data(). Every
// ImGui call runs on the render thread; input crosses threads through a
// small queued copy of SDL events, and render-thread requests (fullscreen,
// quit) cross back through a mailbox consumed by the GX2 thread.
#pragma once
#include <cstdint>

#define VK_NO_PROTOTYPES
#include <volk.h>

union SDL_Event;
struct SDL_Window;
struct ImDrawData;

namespace gx2_imgui {

// Queues one SDL event for the render thread (any thread, non-blocking).
void process_event(const SDL_Event& event);

// Settings menu (Esc / controller touchpad). GX2 thread: toggles the menu
// and consumes every key/button press while it is open (the game keeps
// running but gets no input). The menu itself is built on the render thread.
bool menu_event(const SDL_Event& event);
void set_menu_open(bool open);
bool menu_open();
// Opens the menu on the amiibo picker (GX2 thread, when the game starts NFC
// detection). Esc/B/Back returns to the game's own "touch amiibo" screen.
void open_amiibo_picker();
bool consume_menu_changed(); // true once after the flag changed (GX2 syncs the mouse cursor)
// True while the menu is open and until every key/button held when it
// closed is released (the confirming press must not reach the game).
bool menu_blocks_input(bool any_input_held);

// Mailbox: requested on the render thread (menu widgets), applied on the GX2
// thread in GX2SwapScanBuffers.
bool consume_fullscreen_request(bool& fullscreen);
bool consume_quit_request();
// Mirror of the live window state, refreshed by the GX2 thread every swap.
void note_fullscreen(bool fullscreen);

// Performance / debug overlay visibility (F3), persisted as `fps_overlay` in
// settings.ini (RBREW_FPS_OVERLAY=1 starts visible; hidden by default). Any
// thread.
void set_visible(bool visible);
bool visible();
bool consume_visibility_changed(); // true once after the flag changed

// Simple overlay: FPS, frame time and the best FPS of the session, drawn
// without window chrome and without taking input. Persisted as
// `simple_overlay` in settings.ini (RBREW_SIMPLE_OVERLAY=1 starts visible).
void set_simple_overlay(bool visible);
bool simple_overlay();
bool consume_simple_overlay_changed(); // true once after the flag changed

// Render thread, called from gx2vk::present when the overlay or the menu is
// visible and the present is not a test capture. Creates the context and
// backends on first use; returns false while headless (no window) or on
// failure.
bool ensure_ready(SDL_Window* window, VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                  uint32_t queue_family, VkQueue queue, VkFormat format, uint32_t image_count);
// Builds the frame (drains queued input, NewFrame, overlay/menu windows) and
// returns the draw data, or null when nothing should be drawn.
ImDrawData* begin_frame(VkExtent2D extent);
// Records the draw data over `target` (dynamic rendering). The target is
// left in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL.
void render_draw_data(ImDrawData* draw_data, VkCommandBuffer cb, VkImage target, VkImageView target_view,
                      VkExtent2D extent);
// Tears the context and backends down (device idle, single-threaded).
void shutdown();

} // namespace gx2_imgui

// gx2_env_light.h - the plaza's own lights follow the time of day and the
// weather.
//
// The game keeps its lighting in agl::env parameter objects (DirectionalLight,
// HemisphereLight, Fog, PointLightRig, BloomObj; see plazaday.baglenv in
// Env.pack). frame() finds them in guest memory once per plaza visit and
// writes sun direction, colours and intensities every frame, so the game's
// own shaders, light pre-pass and shadow maps do the lighting:
//   - Main is blended each frame from the area lights (MainPlaza, MainNews):
//     direction and intensity are written to those sources, colours to Main;
//   - the static depth shadow is rebuilt when the sun direction changes (the
//     game only does that when its +0x47C flag is set).
// Every write re-checks the parameter header, so a freed or moved object is
// never touched; objects are searched again after a scene change.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace gx2env {
struct SceneGraphics {
    bool environment = false, post_only = false;
};
inline SceneGraphics scene_graphics(int type, bool anywhere = true, bool levels = true, bool every = false) {
    if (type == 1) return {false, levels};
    if (type <= 0 || type == 16) return {};
    return {type == 2 || (anywhere && (every || type == 3 || type == 5 || type == 6)), false};
}
inline std::array<float, 3> backlight_color(const std::array<float, 3>& original, const std::array<float, 3>& light,
                                            bool moonlit, float moon, float rim) {
    std::array<float, 3> out{};
    rim = std::clamp(rim, 0.f, 1.f);
    for (unsigned i = 0; i < 3; ++i)
        out[i] =
            moonlit ? light[i] * .025f * std::clamp(moon, 0.f, 1.f) : light[i] * (original[i] * (1 - rim) + .12f * rim);
    return out;
}
inline std::array<float, 3> ambient_from_environment(const std::array<float, 3>& original,
                                                     const std::array<float, 3>& reference,
                                                     const std::array<float, 3>& environment) {
    auto y = [](const auto& c) { return c[0] * .2126f + c[1] * .7152f + c[2] * .0722f; };
    const float scale = y(original) / std::max(y(reference), .001f);
    return {environment[0] * scale, environment[1] * scale, environment[2] * scale};
}
inline float night_lamp_intensity(float original, bool enabled, float night, float ceiling) {
    const float n = std::clamp(night, 0.f, 1.f);
    return (enabled ? original : std::min(original, ceiling) * n) * (1 + .6f * n);
}
// The plaza uses two LookAtCamera objects. Collision/pitch can move their eye
// away from the player camera's stored eye after they have been identified.
struct MainCameraIdentity {
    uint32_t field = 0;
    std::array<uint32_t, 2> cameras{};
    bool matches(uint32_t current_field, uint32_t camera, bool at_player_eye) {
        if (current_field != field) {
            field = current_field;
            cameras = {};
        }
        if (!field || !camera) return false;
        for (auto id : cameras)
            if (id == camera) return true;
        if (!at_player_eye) return false;
        for (auto& id : cameras)
            if (!id) {
                id = camera;
                return true;
            }
        return false;
    }
};
// Game thread, once per GX2SwapScanBuffers.
void frame(uint8_t* mem, uint64_t swap_count);
// Before guest code runs: the camera field of view hook.
void register_hooks();
// Free fly camera (testing; F7 or the ESC menu's Tools & hacks page). Never in online
// mode (online_server= set): no free camera or spawn hacks next to other players.
void set_fly_camera(bool on);
bool fly_camera();
bool online_mode();
// Fly spots (this session): save the current view, fly back to it later.
// save returns false outside the plaza; go turns the fly camera on.
// Game thread (VPADRead): PC controls of the fly camera while it is on -
// mouse counts turn it, move/side/up in -1..1 (WASD, Space/Ctrl), fast = Shift.
// The game meanwhile gets a neutral pad (the character stands still).
void fly_input(int mouse_dx, int mouse_dy, float move, float side, float up, bool fast);
// Fly camera field of view (zoom for photos): mouse wheel / Q, E / menu.
void fly_zoom(float steps);
float fly_fov();
void set_fly_fov(float degrees);
// Spawn hack (plaza): remember the character's position in one of three
// spots, put the character back there, or where the free camera is.
bool save_spawn(int slot);
bool spawn_at(int slot);
bool spawn_at_camera();
bool spawn_saved(int slot);
// The main camera as the game built its view this frame (look-at camera
// hook): for scenes whose shader data has another layout (Octo Valley).
struct MainView {
    float pos[3], at[3], up[3], fovy_deg, near_z, far_z;
    uint64_t frame;
};
bool main_view(MainView& out);
void want_spawn();
// Menu actions from the render thread (ImGui), run on the game thread in
// frame(): the free camera, fly spots, spawning and photos.
enum class Request { FlyCamera, FlyZoom, SaveFlySpot, GoFlySpot, SaveSpawn, SpawnAt, SpawnAtCamera, Photo };
void request(Request what, int value); // menu: the camera search starts (the hack needs it)
bool save_fly_spot(int slot);
bool go_fly_spot(int slot);
bool fly_spot_saved(int slot);

// Sky state for the renderer (any thread).
struct SkyState {
    int scene_type = -1, scene_id = -1;
    bool active = false;      // a 3D scene with lighting shaders on
    bool plaza = false;       // the plaza (its lights follow the clock)
    bool post_only = false;   // story level: picture effects only, the game's own sky and lights
    float sun[3] = {0, 1, 0}; // unit vectors towards sun / moon / celestial pole (world)
    float moon[3] = {0, -1, 0};
    float pole[3] = {0, 1, 0};
    float sun_elevation = 50, moon_elevation = -50; // degrees
    float star_angle = 0;                           // sidereal rotation, radians
    float hour = 14.5f;
    float cloud_cover = 0.3f; // 0..1
    float rain = 0;           // 0..1
    float wetness = 0;        // 0..1, follows the rain slowly
    float lamps = 0;          // 0 day .. 1 night lights
    double seconds = 0;       // simulation clock (wind)
    float lightning = 0;      // 0..1 flash of the current strike
    float bolt_azimuth = 0, bolt_seed = 0;
};
SkyState sky_state();
} // namespace gx2env

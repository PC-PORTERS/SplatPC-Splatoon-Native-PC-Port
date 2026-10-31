// gx2_camera_hooks.h - the game's camera hooks: field of view, look-at camera,
// free fly camera, fly spots and the spawn hack.
#pragma once
#include <array>
#include <cstdint>

namespace rbrew_settings {
struct Values;
}

namespace gx2env {
// The plaza uses two LookAtCamera objects. Collision/pitch can move their eye
// away from the player camera's stored eye after they have been identified.
// The main camera as the game built its view this frame (look-at camera hook):
// for scenes whose shader data has another layout (Octo Valley).
struct MainView {
    float pos[3], at[3], up[3], fovy_deg, near_z, far_z;
    uint64_t frame;
};
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

// Before guest code runs: install the camera hooks.
void register_hooks();
// Menu actions from the render thread (ImGui), run on the game thread in the
// lighting frame: the free camera, fly spots, spawning and photos.
enum class Request { FlyCamera, FlyZoom, SaveFlySpot, GoFlySpot, SaveSpawn, SpawnAt, SpawnAtCamera, Photo };
void request(Request what, int value); // menu: the camera search starts (the hack needs it)
void run_requests();

// Free fly camera (testing; F7 or the ESC menu's Tools & hacks page).
void set_fly_camera(bool on);
bool fly_camera();
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
// Spawn hack (plaza): remember the character's position in one of three spots,
// put the character back there, or where the free camera is.
bool save_spawn(int slot);
bool spawn_at(int slot);
bool spawn_at_camera();
bool spawn_saved(int slot);
bool main_view(MainView& out);
void want_spawn();
bool save_fly_spot(int slot);
bool go_fly_spot(int slot);
bool fly_spot_saved(int slot);

// Per-frame data the lighting driver gives the camera hooks. The camera field
// of view, fly camera and spawn tracking run after the lights are driven.
struct CameraFrameContext {
    uint8_t* mem = nullptr;
    uint64_t swap_count = 0;
    uint64_t world_frames = 0;
    bool camera_scene = false;
    bool shaders_on = false;
    bool world = false;
    bool plaza = false;
    const float* sun = nullptr;
    const rbrew_settings::Values* settings = nullptr;
};
void camera_frame(const CameraFrameContext& context);
} // namespace gx2env

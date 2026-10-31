// gx2_camera_hooks.cpp - the game's camera hooks: field of view, look-at
// camera, free fly camera, fly spots and the spawn hack.
#include "gx2_camera_hooks.h"
#include "gx2_guest_memory.h"
#include "../os/gx2/gx2_render.h"
#include "../runtime/port_settings.h"
#include "../runtime/rebrewu_runtime.h"
#include "rebrewu/logger.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <future>
#include <mutex>
#include <utility>
#include <vector>
#if defined(HAVE_SDL2)
#include <SDL.h>
#endif

namespace gx2env {
namespace {
// --- Camera field of view ---------------------------------------------------
// The player camera keeps its field of view in degrees directly before the
// near and far plane (found in the plaza: 55, 2, 20000), and the game builds
// its sead::PerspectiveProjection (near, far, fovy in radians, sin, cos, tan,
// aspect) from it every frame. Search: degree fields whose radians, near and
// far match a projection object; then write the wanted degrees each frame.
struct FovSearch {
    uint32_t field = 0;
    float original = 0;
};
FovSearch find_fov(const uint8_t* mem) {
    struct Proj {
        float near_, far_, fovy;
    };
    std::vector<Proj> projections;
    for_each_heap_region(mem, [&](uint32_t lo, uint32_t hi) {
        for (uint32_t a = lo; a + 28 <= hi; a += 4) {
            const float n = rf(mem, a), f = rf(mem, a + 4), y = rf(mem, a + 8);
            if (!(n > 0.05f && n < 100.f && f > n * 100.f && f < 1e6f && y > 0.4f && y < 2.0f)) continue;
            if (std::abs(rf(mem, a + 12) - std::sin(y * 0.5f)) > 1e-3f &&
                std::abs(rf(mem, a + 12) - std::sin(y)) > 1e-3f)
                continue;
            projections.push_back({n, f, y});
        }
    });
    FovSearch best;
    if (projections.empty()) return best;
    for_each_heap_region(mem, [&](uint32_t lo, uint32_t hi) {
        for (uint32_t a = lo; a + 12 <= hi && !best.field; a += 4) {
            const float d = rf(mem, a);
            if (!(d >= 30.f && d <= 100.f)) continue;
            const float n = rf(mem, a + 4), f = rf(mem, a + 8);
            for (const auto& pr : projections)
                if (n == pr.near_ && f == pr.far_ && std::abs(d * 0.0174533f - pr.fovy) < 1e-3f) {
                    best = {a, d};
                    break;
                }
        }
    });
    return best;
}
// Spawn hack. The player camera keeps the character's position at
// eye - 0x1C; the player object holds it several times (current,
// previous, matrix columns) within ~1.5 KB, and only moving all of them
// moves the character (one copy alone is restored the next frame).
struct Spawn {
    std::future<uint32_t> job; // finds the player object's copy
    uint32_t base = 0;         // one copy in the player object
    float now[3] = {0, 0, 0};  // the character, this frame
    bool now_valid = false;
    float spots[3][3] = {};
    bool saved[3] = {};
    bool pending = false; // move the character to `target`
    float target[3] = {0, 0, 0};
};
Spawn s_spawn;
std::atomic<bool> s_spawn_wanted{false}; // the hacks page was opened: find the camera
uint32_t find_player_copy(const uint8_t* mem, std::array<float, 3> p, uint32_t camera_eye) {
    std::vector<uint32_t> hits;
    for_each_heap_region(mem, [&](uint32_t lo, uint32_t hi) {
        for (uint32_t a = lo; a + 12 <= hi; a += 4)
            if (rf(mem, a) == p[0] && rf(mem, a + 4) == p[1] && rf(mem, a + 8) == p[2] &&
                (a + 0x200 < camera_eye || a > camera_eye + 0x300))
                hits.push_back(a);
    });
    uint32_t best = 0;
    size_t most = 0;
    for (size_t i = 0; i < hits.size(); ++i) {
        size_t nearby = 0;
        for (uint32_t h : hits) nearby += h + 0x400 >= hits[i] && h < hits[i] + 0x600;
        if (nearby > most) {
            most = nearby;
            best = hits[i];
        }
    }
    return most >= 4 ? best : 0;
}
// Copies of `p` around the player object's copy (empty when it moved).
std::vector<uint32_t> player_copies(uint8_t* mem, uint32_t base, const float* p) {
    std::vector<uint32_t> out;
    if (!base || !rbrew_memory_accessible(mem, base - 0x400, 0xA00)) return out;
    for (uint32_t a = base - 0x400; a < base + 0x600; a += 4)
        if (std::abs(rf(mem, a) - p[0]) < 0.01f && std::abs(rf(mem, a + 4) - p[1]) < 0.01f &&
            std::abs(rf(mem, a + 8) - p[2]) < 0.01f)
            out.push_back(a);
    return out;
}

struct FovDriver {
    std::future<FovSearch> job;
    FovSearch found;
    bool ready = false;
    float written = 0;
    void reset() {
        job = {};
        found = {};
        ready = false;
        written = 0;
    }
    bool valid(const uint8_t* mem) const {
        const float d = rf(mem, found.field), n = rf(mem, found.field + 4), f = rf(mem, found.field + 8);
        return d >= 30.f && d <= 100.f && n > 0.05f && f > n * 100.f;
    }
};
FovDriver s_fov;

// The wanted field of view as a factor of the game's (55 degrees); applied
// in the hook of the camera's set-fov call (guest 0200F24C, degrees in f1)
// to the main camera only: the call whose angle equals the player camera's
// own field. The game's smoothing and run-widening stay untouched.
std::atomic<float> s_fov_scale{1.0f};
std::atomic<uint32_t> s_fov_field{0};
std::atomic<bool> s_fly{false};
std::atomic<float> s_fly_fov{55.0f}; // fly camera zoom (degrees)
void fov_hook(CPUState* cpu, uint32_t, HostFunc original) {
    const uint32_t field = s_fov_field.load(std::memory_order_relaxed);
    const float scale = s_fly.load(std::memory_order_relaxed) ? s_fly_fov.load(std::memory_order_relaxed) / 55.f
                                                              : s_fov_scale.load(std::memory_order_relaxed);
    if (field && scale != 1.0f) {
        const float deg = float(cpu->f[1]);
        if (deg >= 20.f && deg <= 120.f && std::abs(deg - rf(cpu->mem, field)) < 0.01f) {
            cpu->f[1] = std::min(double(deg * scale), 120.0);
            static float logged = 0.f;
            if (std::abs(float(cpu->f[1]) - logged) > 2.f) {
                logged = float(cpu->f[1]);
                RBREW_LOG_INFO("camera", "field of view %.1f -> %.1f", deg, logged);
            }
        }
    }
    original(cpu);
}

// The look-at camera (guest 0281ADB4, sead::LookAtCamera::doUpdateMatrix;
// r3 = camera, position +0x34, target +0x40, up +0x4C) runs right after the
// game set the position for this frame. The moved position, target and up
// only go into the view matrix: they are restored afterwards, because the
// game builds the next frame's camera from the stored ones (leaving them
// moved fed back each frame and the camera ran away until it looked
// straight down at the ground).
// Only the plaza's main camera: the one whose position is the player
// camera's eye (player camera = field of view field - 0xCC).
// The plaza uses two LookAtCamera objects. Collision/pitch can move their eye
// away from the player camera's stored eye after they have been identified.
std::atomic<float> s_cam_distance{1.0f}, s_cam_height{0.0f};
std::mutex s_view_mutex;
MainView s_view{}; // the main camera's final view (camera hook)
MainCameraIdentity s_main_cameras;
std::atomic<uint64_t> s_view_clock{0}; // swap count, for the view's age
std::atomic<float> s_wiggle{0.0f};     // camera wiggle strength (radians scale), 0 = off
std::atomic<double> s_wiggle_time{0.0};
struct Fly {
    float pos[3] = {0, 0, 0}, yaw = 0, pitch = 0;
    bool started = false;
};
Fly s_fly_state;
struct FlyInput {
    float yaw = 0, pitch = 0, move = 0, side = 0, up = 0;
    bool fast = false;
};
FlyInput s_fly_input; // from VPADRead, used by camera_frame() (both game thread)
Fly s_fly_spots[3];   // saved views (started = saved)
Fly s_last_view;      // the main camera's view of the last frame
void camera_hook(CPUState* cpu, uint32_t, HostFunc original) {
    const uint32_t field = s_fov_field.load(std::memory_order_relaxed);
    const uint32_t cam = cpu->r[3];
    uint8_t* mem = cpu->mem;
    float saved[9];
    bool moved = false;
    bool main_camera = false;
    if (!field) s_main_cameras = {};
    if (field && cam >= kHeapBase && cam < kHeapEnd - 0x60) {
        const uint32_t eye = field - 0xCC;
        float pos[3], at[3];
        for (int i = 0; i < 3; ++i) {
            pos[i] = rf(mem, cam + 0x34 + 4 * i);
            at[i] = rf(mem, cam + 0x40 + 4 * i);
        }
        const bool at_player_eye = std::abs(pos[0] - rf(mem, eye)) < 0.01f &&
                                   std::abs(pos[1] - rf(mem, eye + 4)) < 0.01f &&
                                   std::abs(pos[2] - rf(mem, eye + 8)) < 0.01f;
        main_camera = s_main_cameras.matches(field, cam, at_player_eye);
        if (std::getenv("RBREW_WEATHER_LOG")) {
            static uint64_t logged = ~0ull;
            static unsigned calls = 0;
            const auto now = s_view_clock.load();
            if (logged == ~0ull || now - logged >= 120) {
                logged = now;
                calls = 0;
            }
            if (now == logged && calls++ < 8)
                RBREW_LOG_DEBUG("camera", "match frame %llu camera %08x main %d delta %.3f %.3f %.3f at %.3f %.3f %.3f",
                                (unsigned long long)now, cam, int(main_camera), pos[0] - rf(mem, eye),
                                pos[1] - rf(mem, eye + 4), pos[2] - rf(mem, eye + 8), at[0] - pos[0], at[1] - pos[1],
                                at[2] - pos[2]);
        }
        if (main_camera) {
            for (int i = 0; i < 9; ++i) saved[i] = rf(mem, cam + 0x34 + 4 * i);
            { // remembered for "save this view" (the game's view, or the fly camera's)
                const float dx = at[0] - pos[0], dy = at[1] - pos[1], dz = at[2] - pos[2];
                s_last_view = {
                    {pos[0], pos[1], pos[2]}, std::atan2(dx, -dz), std::atan2(dy, std::sqrt(dx * dx + dz * dz)), true};
            }
            Fly& f = s_fly_state;
            if (s_fly.load(std::memory_order_relaxed)) {
                if (!f.started) {
                    for (int i = 0; i < 3; ++i) f.pos[i] = pos[i];
                    const float dx = at[0] - pos[0], dy = at[1] - pos[1], dz = at[2] - pos[2];
                    f.yaw = std::atan2(dx, -dz);
                    f.pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));
                    f.started = true;
                }
                const float fwd[3] = {std::sin(f.yaw) * std::cos(f.pitch), std::sin(f.pitch),
                                      -std::cos(f.yaw) * std::cos(f.pitch)};
                for (int i = 0; i < 3; ++i) {
                    wf(mem, cam + 0x34 + 4 * i, f.pos[i]);
                    wf(mem, cam + 0x40 + 4 * i, f.pos[i] + fwd[i] * 10.f);
                }
                wf(mem, cam + 0x4C, 0.f);
                wf(mem, cam + 0x50, 1.f);
                wf(mem, cam + 0x54, 0.f);
                moved = true;
            } else {
                f.started = false;
                const float k = s_cam_distance.load(std::memory_order_relaxed),
                            h = s_cam_height.load(std::memory_order_relaxed);
                // The look-at target is only a point one unit ahead; the
                // character the camera orbits is the player camera's focus
                // point (eye + 0x0C, its distance at eye + 0xE8). The camera
                // moves along focus->eye and keeps aiming relative to it.
                float focus[3];
                for (int i = 0; i < 3; ++i) focus[i] = rf(mem, eye + 0x0C + 4 * i);
                float back[3], length = 0.f;
                for (int i = 0; i < 3; ++i) {
                    back[i] = pos[i] - focus[i];
                    length += back[i] * back[i];
                }
                length = std::sqrt(length);
                if ((k != 1.0f || h != 0.0f) && length > 5.f && length < 400.f) {
                    float moved_pos[3], to_old[3], to_new[3], dir[3], n_old = 0.f, n_new = 0.f, n_dir = 0.f,
                                                                      ahead = 0.f;
                    for (int i = 0; i < 3; ++i) {
                        moved_pos[i] = focus[i] + back[i] * k + (i == 1 ? h : 0.f);
                        to_old[i] = focus[i] - pos[i];
                        to_new[i] = focus[i] - moved_pos[i];
                        n_old += to_old[i] * to_old[i];
                        n_new += to_new[i] * to_new[i];
                        ahead += (at[i] - pos[i]) * (at[i] - pos[i]);
                    }
                    n_old = std::sqrt(n_old);
                    n_new = std::sqrt(n_new);
                    ahead = std::sqrt(ahead);
                    // Same aim offset from the character as the game's view.
                    for (int i = 0; i < 3; ++i) {
                        dir[i] = (at[i] - pos[i]) / std::max(ahead, 1e-4f) + to_new[i] / n_new - to_old[i] / n_old;
                        n_dir += dir[i] * dir[i];
                    }
                    n_dir = std::sqrt(n_dir);
                    if (n_dir > 1e-3f) {
                        for (int i = 0; i < 3; ++i) {
                            wf(mem, cam + 0x34 + 4 * i, moved_pos[i]);
                            wf(mem, cam + 0x40 + 4 * i, moved_pos[i] + dir[i] / n_dir * std::max(ahead, 1.f));
                        }
                        moved = true;
                    }
                }
            }
        }
    }
    // Camera wiggle: a slow hand-held drift of the real camera (yaw, pitch,
    // roll from a few incommensurate sines), not a shift of the picture.
    const float wiggle = s_wiggle.load(std::memory_order_relaxed);
    if (wiggle > 0.f && field && cam >= kHeapBase && cam < kHeapEnd - 0x60) {
        float p[3], a[3], u[3];
        for (int i = 0; i < 3; ++i) {
            p[i] = rf(mem, cam + 0x34 + 4 * i);
            a[i] = rf(mem, cam + 0x40 + 4 * i);
            u[i] = rf(mem, cam + 0x4C + 4 * i);
        }
        if (main_camera) {
            if (!moved)
                for (int i = 0; i < 9; ++i) saved[i] = rf(mem, cam + 0x34 + 4 * i);
            const double t = s_wiggle_time.load(std::memory_order_relaxed);
            const float yaw = wiggle * float(0.55 * std::sin(t * 0.83) + 0.30 * std::sin(t * 1.91 + 1.3) +
                                             0.15 * std::sin(t * 3.7 + 0.4));
            const float pitch = wiggle * 0.75f * float(0.6 * std::sin(t * 0.67 + 0.7) + 0.4 * std::sin(t * 2.3 + 2.1));
            const float roll = wiggle * 0.6f * float(std::sin(t * 0.51 + 0.2) * 0.7 + std::sin(t * 1.6) * 0.3);
            float d[3] = {a[0] - p[0], a[1] - p[1], a[2] - p[2]};
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len > 1e-4f) {
                for (float& c : d) c /= len;
                // yaw about world up
                const float cy = std::cos(yaw), sy = std::sin(yaw);
                float d1[3] = {d[0] * cy - d[2] * sy, d[1], d[0] * sy + d[2] * cy};
                // pitch: tilt towards up
                d1[1] += pitch;
                const float l1 = std::sqrt(d1[0] * d1[0] + d1[1] * d1[1] + d1[2] * d1[2]);
                for (float& c : d1) c /= l1;
                // roll: tilt the up vector sideways (right = d x up)
                const float r[3] = {d1[1] * u[2] - d1[2] * u[1], d1[2] * u[0] - d1[0] * u[2],
                                    d1[0] * u[1] - d1[1] * u[0]};
                float u1[3] = {u[0] + r[0] * roll, u[1] + r[1] * roll, u[2] + r[2] * roll};
                for (int i = 0; i < 3; ++i) {
                    wf(mem, cam + 0x40 + 4 * i, p[i] + d1[i] * len);
                    wf(mem, cam + 0x4C + 4 * i, u1[i]);
                }
                moved = true;
            }
        }
    }
    // Remember the main camera's final view (after our changes) for the
    // effects in scenes whose shader data has another layout.
    if (field && cam >= kHeapBase && cam < kHeapEnd - 0x60) {
        float p[3];
        for (int i = 0; i < 3; ++i) p[i] = rf(mem, cam + 0x34 + 4 * i);
        if (main_camera) {
            std::lock_guard<std::mutex> lock(s_view_mutex);
            for (int i = 0; i < 3; ++i) {
                s_view.pos[i] = p[i];
                s_view.at[i] = rf(mem, cam + 0x40 + 4 * i);
                s_view.up[i] = rf(mem, cam + 0x4C + 4 * i);
            }
            const float deg = rf(mem, field);
            const float scale = s_fly.load(std::memory_order_relaxed) ? s_fly_fov.load() / 55.f
                                                                      : s_fov_scale.load(std::memory_order_relaxed);
            s_view.fovy_deg = std::min(deg * scale, 120.f);
            s_view.near_z = rf(mem, field + 4);
            s_view.far_z = rf(mem, field + 8);
            s_view.frame = s_view_clock.load(std::memory_order_relaxed);
        }
    }
    original(cpu);
    if (moved)
        for (int i = 0; i < 9; ++i) wf(mem, cam + 0x34 + 4 * i, saved[i]);
}
} // namespace

void register_hooks() {
    static const bool off = [] {
        const char* v = std::getenv("RBREW_CAMERA_HOOK");
        return v && v[0] == '0';
    }();
    if (!off && !rbrew_set_guest_hook(0x0281ADB4u, camera_hook)) RBREW_LOG_WARN("camera", "look-at hook unavailable");
    if (!off && !rbrew_set_guest_hook(0x0200F24Cu, fov_hook))
        RBREW_LOG_WARN("camera", "field of view hook unavailable");
}

void set_fly_camera(bool on) {
    s_fly.store(on);
}
bool fly_camera() {
    return s_fly.load();
}
void fly_zoom(float steps) {
    s_fly_fov.store(std::clamp(s_fly_fov.load() - steps * 3.f, 15.f, 110.f));
}
float fly_fov() {
    return s_fly_fov.load();
}
void set_fly_fov(float degrees) {
    s_fly_fov.store(std::clamp(degrees, 15.f, 110.f));
}
void fly_input(int mouse_dx, int mouse_dy, float move, float side, float up, bool fast) {
    const float scale = rbrew_settings::g_mouse_scale.load(std::memory_order_relaxed);
    s_fly_input.yaw += float(mouse_dx) * 0.0022f * scale;
    s_fly_input.pitch -= float(mouse_dy) * 0.0022f * scale;
    s_fly_input.move = move;
    s_fly_input.side = side;
    s_fly_input.up = up;
    s_fly_input.fast = fast;
}
bool save_spawn(int slot) {
    s_spawn_wanted.store(true);
    if (slot < 0 || slot > 2 || !s_spawn.now_valid) return false;
    std::copy(s_spawn.now, s_spawn.now + 3, s_spawn.spots[slot]);
    s_spawn.saved[slot] = true;
    return true;
}
bool spawn_at(int slot) {
    if (slot < 0 || slot > 2 || !s_spawn.saved[slot] || !s_spawn.now_valid) return false;
    std::copy(s_spawn.spots[slot], s_spawn.spots[slot] + 3, s_spawn.target);
    s_spawn.pending = true;
    return true;
}
bool spawn_at_camera() {
    // Where the free camera is (or the game camera): the character drops
    // from there to the ground below.
    if (!s_spawn.now_valid || !s_last_view.started) return false;
    const Fly& v = s_fly.load() && s_fly_state.started ? s_fly_state : s_last_view;
    s_spawn.target[0] = v.pos[0] + std::sin(v.yaw) * 20.f;
    s_spawn.target[1] = v.pos[1];
    s_spawn.target[2] = v.pos[2] - std::cos(v.yaw) * 20.f;
    s_spawn.pending = true;
    return true;
}
std::mutex s_request_mutex;
std::vector<std::pair<Request, int>> s_requests;
void request(Request what, int value) {
    std::lock_guard<std::mutex> lock(s_request_mutex);
    if (s_requests.size() < 32) s_requests.emplace_back(what, value);
}
void run_requests() {
    std::vector<std::pair<Request, int>> pending;
    {
        std::lock_guard<std::mutex> lock(s_request_mutex);
        pending.swap(s_requests);
    }
    for (const auto& [what, value] : pending) {
        switch (what) {
        case Request::FlyCamera: set_fly_camera(value != 0); break;
        case Request::FlyZoom: set_fly_fov(float(value)); break;
        case Request::SaveFlySpot: save_fly_spot(value); break;
        case Request::GoFlySpot: go_fly_spot(value); break;
        case Request::SaveSpawn: save_spawn(value); break;
        case Request::SpawnAt: spawn_at(value); break;
        case Request::SpawnAtCamera: spawn_at_camera(); break;
        case Request::Photo: gx2_render_request_photo(); break;
        }
    }
}
void want_spawn() {
    s_spawn_wanted.store(true);
}
bool main_view(MainView& out) {
    std::lock_guard<std::mutex> lock(s_view_mutex);
    if (!s_fov_field.load() || s_view_clock.load() - s_view.frame > 2 || !(s_view.near_z > 0.f) ||
        !(s_view.far_z > s_view.near_z)) {
        if (std::getenv("RBREW_WEATHER_LOG")) {
            static uint64_t logged = ~0ull;
            const auto now = s_view_clock.load();
            if (logged == ~0ull || now - logged >= 120) {
                logged = now;
                RBREW_LOG_DEBUG("camera", "missing view frame %llu age %llu field %08x near %.2f far %.1f",
                                (unsigned long long)now, (unsigned long long)(now - s_view.frame), s_fov_field.load(),
                                s_view.near_z, s_view.far_z);
            }
        }
        return false;
    }
    out = s_view;
    return true;
}
bool spawn_saved(int slot) {
    return slot >= 0 && slot < 3 && s_spawn.saved[slot];
}
bool save_fly_spot(int slot) {
    if (slot < 0 || slot > 2 || !s_fov_field.load()) return false;
    s_fly_spots[slot] = s_fly.load() && s_fly_state.started ? s_fly_state : s_last_view;
    return s_fly_spots[slot].started;
}
bool go_fly_spot(int slot) {
    if (slot < 0 || slot > 2 || !s_fly_spots[slot].started) return false;
    s_fly_state = s_fly_spots[slot];
    s_fly.store(true);
    return true;
}
bool fly_spot_saved(int slot) {
    return slot >= 0 && slot < 3 && s_fly_spots[slot].started;
}

void camera_frame(const CameraFrameContext& context) {
    if (!context.mem || !context.settings) return;
    uint8_t* mem = context.mem;
    const auto& settings = *context.settings;
    run_requests();
    // Camera field of view (menu / RBREW_CAMERA_FOV; 55 is the game's own).
    {
        FovDriver& c = s_fov;
        const bool want = context.camera_scene && settings.camera_fov != 55;
        if (!context.camera_scene || context.world_frames < 2) {
            c.reset();
            s_main_cameras = {};
        } else {
            if (c.ready && !c.valid(mem)) c.reset();
            const bool need = want || settings.camera_distance != 100 || settings.camera_height != 0 || s_fly.load() ||
                              s_spawn_wanted.load() || settings.camera_motion || context.shaders_on;
            static uint64_t retry_at = 0; // a failed search waits 5 s before the next one
            if (context.world_frames < 2) retry_at = 0;
            if (need && !c.ready && !c.job.valid() && context.world_frames > 15 && context.world_frames >= retry_at)
                c.job = std::async(std::launch::async, find_fov, static_cast<const uint8_t*>(mem));
            if (c.job.valid() && c.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                c.found = c.job.get();
                c.job = {};
                c.ready = c.found.field != 0;
                if (!c.ready) retry_at = context.world_frames + 90;
                RBREW_LOG_INFO("camera", "field of view %s at %08X (%.1f degrees)", c.ready ? "found" : "not found",
                               c.found.field, c.found.original);
            }
        }
        s_fov_field.store(c.ready ? c.found.field : 0, std::memory_order_relaxed);
        // Spawn hack: track the character, move it on request.
        // RBREW_SPAWN_TEST=<frame> (diagnostics): save the spot there, spawn
        // back 300 frames later, log the character's position after.
        static const uint64_t spawn_test = [] {
            const char* v = std::getenv("RBREW_SPAWN_TEST");
            return v ? std::strtoull(v, nullptr, 10) : 0ull;
        }();
        if (spawn_test) {
            s_spawn_wanted.store(true);
            if (context.swap_count == spawn_test) {
                const bool saved = save_spawn(0);
                RBREW_LOG_DEBUG("spawn-test", "save %d at %.1f %.1f %.1f", saved, s_spawn.now[0], s_spawn.now[1],
                                s_spawn.now[2]);
            }
            if (context.swap_count == spawn_test + 300) {
                const bool spawned = spawn_at(0);
                RBREW_LOG_DEBUG("spawn-test", "spawn %d from %.1f %.1f %.1f", spawned, s_spawn.now[0], s_spawn.now[1],
                                s_spawn.now[2]);
            }
            if (context.swap_count > spawn_test + 300 && (context.swap_count - spawn_test - 300) % 30 == 0 &&
                context.swap_count < spawn_test + 480)
                RBREW_LOG_DEBUG("spawn-test", "+%llu: %.1f %.1f %.1f",
                                (unsigned long long)(context.swap_count - spawn_test - 300), s_spawn.now[0],
                                s_spawn.now[1], s_spawn.now[2]);
        }
        Spawn& sp = s_spawn;
        sp.now_valid = false;
        if (c.ready) {
            const uint32_t eye = c.found.field - 0xCC;
            for (int i = 0; i < 3; ++i) sp.now[i] = rf(mem, eye - 0x1C + 4 * i);
            const float fx = rf(mem, eye + 0x0C) - sp.now[0], fz = rf(mem, eye + 0x14) - sp.now[2];
            sp.now_valid = std::isfinite(sp.now[0]) && std::isfinite(sp.now[1]) && std::isfinite(sp.now[2]) &&
                           fx * fx + fz * fz < 60.f * 60.f;
            if (sp.pending && sp.now_valid) {
                auto copies = player_copies(mem, sp.base, sp.now);
                if (copies.size() >= 3) {
                    for (uint32_t a : copies)
                        for (int i = 0; i < 3; ++i) wf(mem, a + 4 * i, sp.target[i]);
                    RBREW_LOG_INFO("spawn", "character to %.1f %.1f %.1f (%zu copies)", sp.target[0], sp.target[1],
                                   sp.target[2], copies.size());
                    sp.pending = false;
                } else if (!sp.job.valid()) {
                    sp.base = 0;
                    sp.job = std::async(std::launch::async, find_player_copy, static_cast<const uint8_t*>(mem),
                                        std::array<float, 3>{sp.now[0], sp.now[1], sp.now[2]}, eye);
                }
            }
        } else
            sp.pending = false;
        if (sp.job.valid() && sp.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            sp.base = sp.job.get();
            sp.job = {};
            RBREW_LOG_INFO("spawn", "player object %s %08X", sp.base ? "at" : "not found", sp.base);
            if (!sp.base) sp.pending = false;
        }
        s_fov_scale.store(want ? settings.camera_fov / 55.f : 1.f, std::memory_order_relaxed);
    }
    s_cam_distance.store(context.world ? settings.camera_distance / 100.f : 1.f, std::memory_order_relaxed);
    s_cam_height.store(context.world ? float(settings.camera_height) : 0.f, std::memory_order_relaxed);
    // Wiggle (menu "Camera wiggle", strength): about 0.7 degrees at 100 %.
    s_wiggle.store(context.world && settings.camera_motion && !s_fly.load() ? 0.012f * settings.sway_strength / 100.f
                                                                            : 0.f,
                   std::memory_order_relaxed);
    s_wiggle_time.store(double(context.swap_count) / 60.0, std::memory_order_relaxed);
    s_view_clock.store(context.swap_count, std::memory_order_relaxed);
    // Fly camera (testing): F7 toggles; arrows move, PageUp/PageDown up and
    // down, keypad 4/6/8/2 turn, Shift faster.
#if defined(HAVE_SDL2)
    {
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        static bool f7_was = false;
        const bool f7 = keys && keys[SDL_SCANCODE_F7];
        if (f7 && !f7_was) {
            s_fly.store(!s_fly.load());
            RBREW_LOG_INFO("camera", "fly camera %s", s_fly.load() ? "on" : "off");
        }
        f7_was = f7;
        // RBREW_FLY_TEST=1 (diagnostics): the fly camera turns on in the
        // plaza and drifts up and back by itself, for replays.
        static const bool fly_test = [] {
            const char* v = std::getenv("RBREW_FLY_TEST");
            return v && (v[0] == '1' || v[0] == '2');
        }();
        if (fly_test && context.plaza && !s_fly.load()) s_fly.store(true);
        // RBREW_FLY_TEST=2: the fly camera turns towards the sun and rises
        // slowly (light shafts, silver linings).
        static const bool look_sun = [] {
            const char* v = std::getenv("RBREW_FLY_TEST");
            return v && v[0] == '2';
        }();
        if (fly_test && s_fly.load() && s_fly_state.started) {
            if (look_sun && context.sun) {
                s_fly_state.yaw =
                    std::atan2(context.sun[0], -context.sun[2]) + 0.25f * std::sin(float(context.swap_count) * 0.004f);
                s_fly_state.pitch = std::asin(std::clamp(context.sun[1], -1.f, 1.f)) - 0.12f;
                s_fly_input.up = 0.08f;
            } else {
                s_fly_input.move = -0.4f;
                s_fly_input.up = 0.25f;
                s_fly_input.yaw += 0.004f;
            }
        }
        if (keys && s_fly.load() && s_fly_state.started) {
            Fly& f = s_fly_state;
            const float speed =
                            (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT] || s_fly_input.fast) ? 6.f : 1.5f,
                        turn = 0.03f;
            // Mouse look and WASD (vpad.cpp fly_input), like a PC free camera;
            // Q/E zoom (the pad is neutral meanwhile, so they are free).
            if (keys[SDL_SCANCODE_E]) fly_zoom(0.25f);
            if (keys[SDL_SCANCODE_Q]) fly_zoom(-0.25f);
            f.yaw += s_fly_input.yaw;
            f.pitch = std::clamp(f.pitch + s_fly_input.pitch, -1.5f, 1.5f);
            s_fly_input.yaw = s_fly_input.pitch = 0.f;
            {
                const float cy = std::cos(f.yaw), sy = std::sin(f.yaw), cp = std::cos(f.pitch), sp = std::sin(f.pitch);
                const float m = s_fly_input.move * speed, sd = s_fly_input.side * speed;
                f.pos[0] += sy * cp * m + cy * sd;
                f.pos[1] += sp * m + s_fly_input.up * speed;
                f.pos[2] += -cy * cp * m + sy * sd;
            }
            if (keys[SDL_SCANCODE_KP_4]) f.yaw -= turn;
            if (keys[SDL_SCANCODE_KP_6]) f.yaw += turn;
            if (keys[SDL_SCANCODE_KP_8]) f.pitch = std::min(f.pitch + turn, 1.5f);
            if (keys[SDL_SCANCODE_KP_2]) f.pitch = std::max(f.pitch - turn, -1.5f);
            const float fx = std::sin(f.yaw), fz = -std::cos(f.yaw);
            const float move = (keys[SDL_SCANCODE_UP] ? 1.f : 0.f) - (keys[SDL_SCANCODE_DOWN] ? 1.f : 0.f);
            const float side = (keys[SDL_SCANCODE_RIGHT] ? 1.f : 0.f) - (keys[SDL_SCANCODE_LEFT] ? 1.f : 0.f);
            f.pos[0] += (fx * move - fz * side) * speed;
            f.pos[2] += (fz * move + fx * side) * speed;
            f.pos[1] += ((keys[SDL_SCANCODE_PAGEUP] ? 1.f : 0.f) - (keys[SDL_SCANCODE_PAGEDOWN] ? 1.f : 0.f)) * speed;
        }
    }
#else
    (void)context;
#endif
}
} // namespace gx2env

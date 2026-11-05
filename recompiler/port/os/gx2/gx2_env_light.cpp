// gx2_env_light.cpp - the plaza's own lights follow the time of day and the
// weather (gx2_env_light.h).
#include "gx2_env_light.h"
#include "gx2_sky_model.h"
#include "../../hooks/gx2_guest_memory.h"
#include "../../runtime/port_settings.h"
#include "../../runtime/rebrewu_runtime.h"
#include "../../runtime/native_control_defaults.h"
#include "rebrewu/logger.hpp"
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace gx2env {
namespace {
using gx2sky::smoothstep;
using gx2sky::Vec3;

// agl::utl::Parameter vtables in AGMP v16 (image addresses, fixed).
constexpr uint32_t kVtBool = 0x10142660u, kVtFloat = 0x10143468u;
constexpr uint32_t kVtableLo = 0x10000000u, kVtableHi = 0x10200000u;

constexpr uint32_t crc32(const char* text) {
    uint32_t c = 0xFFFFFFFFu;
    for (; *text; ++text) {
        c ^= uint8_t(*text);
        for (int k = 0; k < 8; ++k) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

// Parameters are [hash][next][vtable][value...], chained by `next`.
struct Param {
    uint32_t at = 0, hash = 0, vtable = 0;
};
struct Object {
    std::string name, group;
    std::vector<Param> params;
};

std::string guest_string(const uint8_t* mem, uint32_t at) {
    // SafeString value: [char*][vtable][capacity][buffer]
    const uint32_t text = r32(mem, at + 12);
    std::string out;
    if (text < kHeapBase || text >= kHeapEnd) return out;
    for (int i = 0; i < 64 && mem[text + i]; ++i) out += char(mem[text + i]);
    return out;
}

struct Found {
    std::vector<Object> objects;       // named objects that start with 'enable'
    std::vector<Param> bake_intensity; // model_scene_light.bake_light_intensity
    uint32_t shadow = 0;               // static depth shadow object (0 = not found)
};

const Param* find(const Object& o, uint32_t hash) {
    for (const auto& q : o.params)
        if (q.hash == hash) return &q;
    return nullptr;
}
bool has(const Object& o, const char* a, const char* b) {
    return find(o, crc32(a)) && find(o, crc32(b));
}

// Background job: the whole application heap, read-only.
Found search(const uint8_t* mem, const char* group) {
    Found f;
    constexpr uint32_t enable = crc32("enable"), name = crc32("name"), grp = crc32("group"),
                       bake = crc32("bake_light_intensity");
    for_each_heap_region(mem, [&](uint32_t lo, uint32_t hi) {
        for (uint32_t a = lo; a + 32 <= hi; a += 4) {
            const uint32_t h = r32(mem, a);
            if (h == bake && r32(mem, a + 8) == kVtFloat) {
                f.bake_intensity.push_back({a, h, kVtFloat});
                continue;
            }
            if (h != enable || r32(mem, a + 8) != kVtBool) continue;
            Object o;
            for (uint32_t at = a, n = 0; at >= lo && at + 32 <= hi && n < 64; at = r32(mem, at + 4), ++n) {
                const Param q{at, r32(mem, at), r32(mem, at + 8)};
                if (q.vtable < kVtableLo || q.vtable >= kVtableHi) break;
                o.params.push_back(q);
                if (q.hash == name)
                    o.name = guest_string(mem, at);
                else if (q.hash == grp)
                    o.group = guest_string(mem, at);
                if (!q.at || r32(mem, at + 4) == 0) break;
            }
            const bool character_lighting = group && !std::strcmp(group, "PlazaDay") && o.group == "DefaultDay" &&
                                            (o.name == "Main" || o.name == "Amb" || o.name == "Chara_Amb");
            if (!o.name.empty() && (character_lighting || (group ? o.group == group : !o.group.empty())))
                f.objects.push_back(std::move(o));
        }
    });
    // No group given (scenes other than the plaza): the group that holds
    // the main directional light ("Main" with a direction) is the scene's.
    if (!group) {
        std::string chosen;
        for (const auto& o : f.objects)
            if (o.name == "Main" && has(o, "Direction", "BacksideColor")) {
                chosen = o.group;
                break;
            }
        std::vector<Object> kept;
        for (auto& o : f.objects)
            if (!chosen.empty() && o.group == chosen) kept.push_back(std::move(o));
        f.objects = std::move(kept);
        RBREW_LOG_INFO("env", "scene light group: %s", chosen.empty() ? "(none)" : chosen.c_str());
    }
    // The static depth shadow points at the Main light object (base = its
    // Direction value - 0x11C) from +0x1198 and caches the direction at +0xF10.
    for (const auto& o : f.objects) {
        if (o.name != "Main" || !has(o, "Direction", "BacksideColor")) continue;
        const uint32_t dir = find(o, crc32("Direction"))->at + 12, base = dir - 0x11C;
        for_each_heap_region(mem, [&](uint32_t lo, uint32_t hi) {
            for (uint32_t a = lo + 0x1198; a + 4 <= hi && !f.shadow; a += 4) {
                if (r32(mem, a) != base) continue;
                const uint32_t s = a - 0x1198;
                if (s + 0x1200 > hi) continue;
                float d = 0;
                for (int i = 0; i < 3; ++i) d += std::fabs(rf(mem, s + 0xF10 + 4 * i) - rf(mem, dir + 4 * i));
                if (d < 0.02f && (mem[s + 0x391] & 1)) f.shadow = s;
            }
        });
    }
    return f;
}

Vec3 mix(const Vec3& a, const Vec3& b, float t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}
Vec3 mul(const Vec3& a, const Vec3& b) {
    return {a[0] * b[0], a[1] * b[1], a[2] * b[2]};
}
Vec3 scale(const Vec3& a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}
float luma(const Vec3& c) {
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}
Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float dot(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
// Rotation taking unit vector a onto unit vector b, applied to v (Rodrigues).
Vec3 rotate(const Vec3& a, const Vec3& b, const Vec3& v) {
    const Vec3 k = cross(a, b);
    const float s = std::sqrt(dot(k, k)), c = dot(a, b);
    if (s < 1e-6f) return c > 0 ? v : scale(v, -1);
    const Vec3 u = scale(k, 1 / s), uxv = cross(u, v);
    const float uv = dot(u, v);
    return {v[0] * c + uxv[0] * s + u[0] * uv * (1 - c), v[1] * c + uxv[1] * s + u[1] * uv * (1 - c),
            v[2] * c + uxv[2] * s + u[2] * uv * (1 - c)};
}

// Ambient and fog through the day, keyed by sun elevation (degrees). The
// first key is replaced by the game's own values at discovery.
struct Key {
    float elevation;
    Vec3 sky, ground;
    float intensity;
    Vec3 fog;
    float fog_alpha, fog_start, fog_end;
};
Key s_keys[] = {
    {25, {}, {}, 0, {}, 0, 0, 0},
    {10, {0.36f, 0.44f, 0.64f}, {0.46f, 0.40f, 0.32f}, 0.50f, {0.60f, 0.60f, 0.64f}, 0.7f, 260, 3400},
    {3, {0.42f, 0.44f, 0.54f}, {0.36f, 0.30f, 0.24f}, 0.40f, {0.66f, 0.64f, 0.66f}, 0.6f, 400, 4000},
    {-1, {0.38f, 0.36f, 0.46f}, {0.28f, 0.21f, 0.16f}, 0.32f, {0.56f, 0.52f, 0.56f}, 0.66f, 350, 3600},
    {-6, {0.14f, 0.17f, 0.38f}, {0.06f, 0.06f, 0.11f}, 0.34f, {0.12f, 0.13f, 0.30f}, 0.82f, 150, 2600},
    {-12, {0.055f, 0.08f, 0.20f}, {0.022f, 0.028f, 0.06f}, 0.36f, {0.022f, 0.03f, 0.07f}, 0.86f, 120, 2400},
};
Key key_at(float e) {
    constexpr int n = int(sizeof s_keys / sizeof s_keys[0]);
    if (e >= s_keys[0].elevation) return s_keys[0];
    if (e <= s_keys[n - 1].elevation) return s_keys[n - 1];
    int i = 0;
    while (e < s_keys[i + 1].elevation) ++i;
    const Key &a = s_keys[i], &b = s_keys[i + 1];
    const float t = smoothstep(a.elevation, b.elevation, e);
    auto l = [t](float x, float y) { return x + (y - x) * t; };
    return {e,
            mix(a.sky, b.sky, t),
            mix(a.ground, b.ground, t),
            l(a.intensity, b.intensity),
            mix(a.fog, b.fog, t),
            l(a.fog_alpha, b.fog_alpha),
            l(a.fog_start, b.fog_start),
            l(a.fog_end, b.fog_end)};
}

// One plaza visit's objects with the game's original values.
struct Light {
    const Object* o;
    Param dir, intensity, diffuse, specular, backside;
    Vec3 d0;
    float i0;
    Vec3 diffuse0, specular0, backside0;
};
struct Hemi {
    const Object* o;
    Param sky, ground, intensity;
    Vec3 sky0, ground0;
    float i0;
};
struct Fog {
    const Object* o;
    Param color, start, end;
    Vec3 c0;
    float a0, s0, e0;
};
struct Lamp {
    Param intensity, enable;
    float i0;
    bool e0;
};
struct Bloom {
    Param enable, intensity;
    bool e0;
    float i0;
};

struct Driver {
    std::future<Found> job;
    Found found;
    bool ready = false;
    std::vector<Light> lights;
    Light* main = nullptr;
    float fade = 0; // 0 = the scene's own lights .. 1 = driven (blends in after a scene change)
    std::vector<Hemi> hemis;
    std::vector<Fog> fogs;
    std::vector<Lamp> lamps;
    std::vector<Param> bakes;
    std::vector<float> bake0;
    Bloom bloom{};
    bool has_bloom = false;
    std::vector<std::pair<Param, bool>> flares; // lens flares (enable) follow the sun only
    bool applied = false;
    Vec3 applied_light{0, 0, 0};
    uint64_t plaza_frames = 0, retry_at = 0;

    std::vector<const Param*> written; // headers re-checked every frame

    bool valid(const uint8_t* mem, const Param& q) const {
        return q.at >= kHeapBase && q.at < kHeapEnd && r32(mem, q.at) == q.hash && r32(mem, q.at + 8) == q.vtable;
    }
    bool all_valid(const uint8_t* mem) const {
        for (const Param* q : written)
            if (!valid(mem, *q)) return false;
        return true;
    }
    Vec3 get3(const uint8_t* mem, const Param& q) const {
        return {rf(mem, q.at + 12), rf(mem, q.at + 16), rf(mem, q.at + 20)};
    }
    void put3(uint8_t* mem, const Param& q, const Vec3& v) {
        for (int i = 0; i < 3; ++i) wf(mem, q.at + 12 + 4 * i, v[i]);
    }

    void reset() {
        job = {};
        found = {};
        ready = false;
        lights.clear();
        main = nullptr;
        hemis.clear();
        fogs.clear();
        lamps.clear();
        bakes.clear();
        bake0.clear();
        has_bloom = false;
        flares.clear();
        applied = false;
        written.clear();
        applied_light = {0, 0, 0};
        fade = 0;
    }

    void adopt(const uint8_t* mem) {
        auto p = [](const Object& o, const char* n) {
            const Param* q = find(o, crc32(n));
            return q ? *q : Param{};
        };
        for (const auto& o : found.objects) {
            if (has(o, "Direction", "BacksideColor")) {
                Light l{&o,
                        p(o, "Direction"),
                        p(o, "Intensity"),
                        p(o, "DiffuseColor"),
                        p(o, "SpecularColor"),
                        p(o, "BacksideColor")};
                if (!l.dir.at || !l.intensity.at || !l.diffuse.at || !l.specular.at) continue;
                l.d0 = get3(mem, l.dir);
                l.i0 = rf(mem, l.intensity.at + 12);
                l.diffuse0 = get3(mem, l.diffuse);
                l.specular0 = get3(mem, l.specular);
                l.backside0 = l.backside.at ? get3(mem, l.backside) : Vec3{0, 0, 0};
                lights.push_back(l);
            } else if (has(o, "SkyColor", "GroundColor")) {
                Hemi h{&o, p(o, "SkyColor"), p(o, "GroundColor"), p(o, "Intensity")};
                if (!h.sky.at || !h.ground.at || !h.intensity.at) continue;
                h.sky0 = get3(mem, h.sky);
                h.ground0 = get3(mem, h.ground);
                h.i0 = rf(mem, h.intensity.at + 12);
                hemis.push_back(h);
            } else if (has(o, "Start", "End") && find(o, crc32("Color"))) {
                Fog f{&o, p(o, "Color"), p(o, "Start"), p(o, "End")};
                f.c0 = get3(mem, f.color);
                f.a0 = rf(mem, f.color.at + 24);
                f.s0 = rf(mem, f.start.at + 12);
                f.e0 = rf(mem, f.end.at + 12);
                fogs.push_back(f);
            } else if (has(o, "Radius", "BonePrefix")) {
                const Param q = p(o, "Intensity"), en = p(o, "enable");
                if (q.at && en.at) {
                    lamps.push_back({q, en, rf(mem, q.at + 12), mem[en.at + 12] != 0});
                    if (std::getenv("RBREW_WEATHER_LOG"))
                        RBREW_LOG_DEBUG("env-lamp", "%s enabled %d intensity %.3f", o.name.c_str(),
                                        int(mem[en.at + 12] != 0), rf(mem, q.at + 12));
                }
            } else if (o.name.rfind("OfxLargeLensFlare", 0) == 0 || o.name.rfind("OfxLensFlare", 0) == 0) {
                const Param q = p(o, "enable");
                if (q.at) flares.push_back({q, mem[q.at + 12] != 0});
            } else if (o.name == "BloomTPS" && find(o, crc32("intensity"))) {
                bloom = {p(o, "enable"), p(o, "intensity"), mem[p(o, "enable").at + 12] != 0,
                         rf(mem, p(o, "intensity").at + 12)};
                has_bloom = true;
            }
        }
        for (auto& l : lights)
            if (l.o->name == "Main" && (!main || l.o->group == "PlazaDay")) main = &l;
        for (const auto& q : found.bake_intensity) {
            const float v = rf(mem, q.at + 12);
            if (v >= 0 && v <= 10) {
                bakes.push_back(q);
                bake0.push_back(v);
            }
        }
        for (const auto& h : hemis)
            if (h.o->name == "Amb" && (!main || h.o->group == main->o->group)) {
                s_keys[0].sky = h.sky0;
                s_keys[0].ground = h.ground0;
                s_keys[0].intensity = h.i0;
            }
        for (const auto& f : fogs)
            if (f.o->name == "FogTPS") {
                s_keys[0].fog = f.c0;
                s_keys[0].fog_alpha = f.a0;
                s_keys[0].fog_start = f.s0;
                s_keys[0].fog_end = f.e0;
            }
        for (auto& l : lights) {
            written.push_back(&l.dir);
            written.push_back(&l.intensity);
            written.push_back(&l.diffuse);
            written.push_back(&l.specular);
            if (l.backside.at) written.push_back(&l.backside);
        }
        for (auto& h : hemis) {
            written.push_back(&h.sky);
            written.push_back(&h.ground);
            written.push_back(&h.intensity);
        }
        for (auto& f : fogs) {
            written.push_back(&f.color);
            written.push_back(&f.start);
            written.push_back(&f.end);
        }
        for (auto& l : lamps) {
            written.push_back(&l.intensity);
            written.push_back(&l.enable);
        }
        for (auto& b : bakes) written.push_back(&b);
        if (has_bloom) {
            written.push_back(&bloom.enable);
            written.push_back(&bloom.intensity);
        }
        for (auto& f : flares) written.push_back(&f.first);
        RBREW_LOG_INFO(
            "env", "plaza lights: %zu directional, %zu ambient, %zu fog, %zu lamps, bloom %d, bake %zu, shadow %08X",
            lights.size(), hemis.size(), fogs.size(), lamps.size(), has_bloom, bakes.size(), found.shadow);
    }

    void restore(uint8_t* mem) {
        if (!applied || !all_valid(mem)) {
            applied = false;
            return;
        }
        for (auto& l : lights) {
            put3(mem, l.dir, l.d0);
            wf(mem, l.intensity.at + 12, l.i0);
            put3(mem, l.diffuse, l.diffuse0);
            put3(mem, l.specular, l.specular0);
            if (l.backside.at) put3(mem, l.backside, l.backside0);
        }
        for (auto& h : hemis) {
            put3(mem, h.sky, h.sky0);
            put3(mem, h.ground, h.ground0);
            wf(mem, h.intensity.at + 12, h.i0);
        }
        for (auto& f : fogs) {
            put3(mem, f.color, f.c0);
            wf(mem, f.color.at + 24, f.a0);
            wf(mem, f.start.at + 12, f.s0);
            wf(mem, f.end.at + 12, f.e0);
        }
        for (auto& l : lamps) {
            wf(mem, l.intensity.at + 12, l.i0);
            mem[l.enable.at + 12] = l.e0;
        }
        for (size_t i = 0; i < bakes.size(); ++i) wf(mem, bakes[i].at + 12, bake0[i]);
        if (has_bloom) {
            mem[bloom.enable.at + 12] = bloom.e0;
            wf(mem, bloom.intensity.at + 12, bloom.i0);
        }
        for (auto& f : flares) mem[f.first.at + 12] = f.second;
        applied = false;
        applied_light = {0, 0, 0};
    }
};
Driver s_driver;

std::mutex s_sky_mutex;
SkyState s_sky;

// Simulation clock and weather, kept across plaza visits.
struct Clock {
    using steady = std::chrono::steady_clock;
    steady::time_point last{};
    double hour = 14.5, seconds = 0;
    int time_setting = -1;
    bool cycle_setting = false;
    float cloud = 0.3f, rain = 0, wet = 0, overcast = 0;
    bool first = true;
    int weather_setting = -1;
    // Live weather: the current state and when it changes next (simulation seconds).
    int live = 0;
    double live_until = 0;
    // Lightning: strike start, next strike, bolt direction and seed.
    double strike_at = -100, next_strike = 0;
    float bolt_azimuth = 0, bolt_seed = 0, flash = 0;
    uint32_t rng = 0x9E3779B9u;
    float random() {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return (rng & 0xFFFFFF) / float(0x1000000);
    }
};
Clock s_clock;

void advance(const rbrew_settings::Values& v) {
    const auto now = Clock::steady::now();
    double dt = s_clock.first ? 0 : std::chrono::duration<double>(now - s_clock.last).count();
    s_clock.last = now;
    dt = std::clamp(dt, 0.0, 0.25);
    if (v.time_of_day != s_clock.time_setting || v.day_cycle != s_clock.cycle_setting) {
        s_clock.time_setting = v.time_of_day;
        s_clock.cycle_setting = v.day_cycle;
        s_clock.hour = v.time_of_day / 60.0;
    }
    if (v.day_cycle) s_clock.hour = std::fmod(s_clock.hour + dt * 24.0 / (std::max(1, v.day_length) * 60.0), 24.0);
    if (v.real_time) {
        const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &t);
#else
        localtime_r(&t, &local);
#endif
        s_clock.hour = local.tm_hour + local.tm_min / 60.0 + local.tm_sec / 3600.0;
    }
    s_clock.seconds += dt;
    using rbrew_settings::kAfterRain;
    using rbrew_settings::kClear;
    using rbrew_settings::kCloudy;
    using rbrew_settings::kRain;
    using rbrew_settings::kStorm;
    int w = v.weather;
    // Live weather: clear -> overcast -> rain (sometimes a thunderstorm) ->
    // after the rain -> clear, each for a few minutes; the transitions are
    // the usual smooth ones below.
    if (w == rbrew_settings::kLiveWeather) {
        if (s_clock.live_until <= s_clock.seconds) {
            const float r = s_clock.random();
            int next = kClear;
            switch (s_clock.live) {
            case kClear: next = r < 0.65f ? kCloudy : kClear; break;
            case kCloudy: next = r < 0.55f ? kRain : r < 0.8f ? kCloudy : kClear; break;
            case kRain: next = r < 0.3f ? kStorm : kAfterRain; break;
            case kStorm: next = r < 0.5f ? kRain : kAfterRain; break;
            default: next = kClear; break;
            }
            if (s_clock.live_until == 0) next = kClear; // a live session starts fair
            const float minutes = next == kClear       ? 6 + 8 * s_clock.random()
                                  : next == kStorm     ? 2 + 2 * s_clock.random()
                                  : next == kAfterRain ? 5 + 5 * s_clock.random()
                                                       : 3 + 4 * s_clock.random();
            s_clock.live = next;
            s_clock.live_until = s_clock.seconds + minutes * 60.0;
        }
        w = s_clock.live;
    } else
        s_clock.live_until = 0;
    float cloud = w == kClear ? 0.30f : w == kAfterRain ? 0.36f : w == kCloudy ? 0.86f : w == kRain ? 0.97f : 1.0f;
    const float overcast = w == kClear || w == kAfterRain ? 0.f : 1.f;
    if (v.cloud_amount >= 0) cloud = 0.05f + 0.93f * v.cloud_amount / 100.f; // the player's slider
    const float rain = w == kRain ? 1.f : w == kStorm ? 1.f : 0.f;
    if (s_clock.first) {
        s_clock.cloud = cloud;
        s_clock.overcast = overcast;
        s_clock.rain = rain;
        s_clock.wet = rain;
    }
    // Choosing "after the rain" starts with soaked streets.
    if (w != s_clock.weather_setting) {
        if (w == kAfterRain) s_clock.wet = 0.75f;
        s_clock.weather_setting = w;
    }
    const float k = float(1 - std::exp(-dt / 12));
    s_clock.cloud += (cloud - s_clock.cloud) * k;
    s_clock.overcast += (overcast - s_clock.overcast) * k;
    // Rain follows the clouds; puddles take minutes to form and dry.
    s_clock.rain += ((s_clock.overcast > 0.7f ? rain : 0.f) - s_clock.rain) * float(1 - std::exp(-dt / 5));
    const float dry = w == kAfterRain ? 1200.f : 240.f;
    s_clock.wet = std::clamp(s_clock.wet + float(dt) * (s_clock.rain / 40 - (1 - s_clock.rain) / dry), 0.f, 1.f);
    // Thunderstorm: a strike every 4-16 s, two or three flickers each.
    if (w == kStorm && s_clock.rain > 0.6f) {
        if (s_clock.seconds >= s_clock.next_strike) {
            s_clock.strike_at = s_clock.seconds;
            s_clock.next_strike = s_clock.seconds + 4 + 12 * s_clock.random();
            s_clock.bolt_azimuth = s_clock.random() * 6.2831853f;
            s_clock.bolt_seed = s_clock.random() * 100.f;
        }
    } else
        s_clock.next_strike = s_clock.seconds + 3;
    const double t = s_clock.seconds - s_clock.strike_at;
    float flash = 0;
    if (t >= 0 && t < 1.2) {
        const double flickers[3] = {0.0, 0.11, 0.27};
        for (double f : flickers)
            if (t >= f) flash = std::max(flash, float(std::exp(-(t - f) * 14.0) * (f == 0.0 ? 1.0 : 0.75)));
    }
    // RBREW_LIGHTNING_TEST=1 (diagnostics): a constant strike for screenshots.
    static const bool lightning_test = [] {
        const char* v = std::getenv("RBREW_LIGHTNING_TEST");
        return v && v[0] == '1';
    }();
    if (lightning_test && w == kStorm) {
        flash = 1;
        s_clock.bolt_azimuth = 1.0f;
        s_clock.bolt_seed = 7.0f;
    }
    s_clock.flash = flash;
    s_clock.first = false;
}

void drive(uint8_t* mem, Driver& d, const gx2sky::Celestial& sky, float& lamps_out, bool plaza) {
    // After a scene change the driven lights blend in over 1.5 s from the
    // scene's own (no jump from the original look). RBREW_LIGHT_FADE=0: at once.
    static const bool fade_on = [] {
        const char* v = std::getenv("RBREW_LIGHT_FADE");
        return !(v && v[0] == '0');
    }();
    d.fade = fade_on ? std::min(1.f, d.fade + 1.f / 90.f) : 1.f;
    const float fd = d.fade;
    auto lerp = [fd](float a, float b) { return a + (b - a) * fd; };
    const float e = sky.sun_elevation;
    const float overcast = s_clock.overcast, rain = s_clock.rain;
    const float weather_direct = (1 - 0.78f * overcast) * (1 - 0.5f * rain);
    // Sun by day, moon by night; the switch happens while both are faint.
    const float sun_k = smoothstep(-2.5f, 3.0f, e);
    const float moon_k = smoothstep(-3.0f, -9.0f, e) * smoothstep(-2.0f, 8.0f, sky.moon_elevation);
    const bool moonlit = e < -3.0f;
    Vec3 light = moonlit ? sky.moon : sky.sun;
    // The game's shadow map smears shadows of a very low light into long
    // streaks: shadows are cast from at least 14 degrees (the sky and the
    // light colour keep the real sun). RBREW_SHADOW_MIN_ELEVATION (degrees).
    {
        static const float min_deg = [] {
            const char* v = std::getenv("RBREW_SHADOW_MIN_ELEVATION");
            return v ? float(std::atof(v)) : 14.f;
        }();
        const float min_y = std::sin(min_deg * 0.0174533f);
        if (light[1] < min_y) {
            const float h = std::sqrt(std::max(light[0] * light[0] + light[2] * light[2], 1e-6f));
            const float k = std::sqrt(1 - min_y * min_y) / h;
            light = {light[0] * k, min_y, light[2] * k};
        }
    }
    Vec3 color;
    float intensity;
    if (!moonlit) {
        const Vec3 t = gx2sky::sun_transmittance(e);
        color = {std::min(t[0], 1.15f), std::min(t[1], 1.15f), std::min(t[2], 1.15f)};
        // White balance: a camera (and the eye) adapts to evening light, so a
        // low sun reads golden white, not the raw orange-red of the
        // transmittance; brightness is kept.
        {
            const float y = std::max(luma(color), 1e-3f);
            const Vec3 golden_white{1.08f * y, 0.96f * y, 0.78f * y};
            color = mix(color, golden_white, .6f * (1.f - smoothstep(12.f, 35.f, e)));
        }
        // The eye adapts to evening light: a low sun still reads strong, only redder.
        intensity = sun_k * weather_direct * (1.0f + 0.6f * (1.0f - smoothstep(6.0f, 28.0f, e))) * 1.15f;
    } else {
        color = {0.55f, 0.66f, 1.0f};
        intensity = 0.2f * moon_k * weather_direct;
    }
    if (fd < 1.f) { // from the game's own sun towards ours
        Vec3 m = mix(gx2sky::kGameSun, light, fd);
        const float ml = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        if (ml > 1e-4f) light = scale(m, 1.f / ml);
    }
    // Direction: rotate every area light by the sun's rotation from the game's
    // sun. Steps of 0.2 degrees keep static shadow rebuilds rare.
    const float moved = std::acos(std::clamp(dot(light, d.applied_light), -1.f, 1.f)) * 57.2958f;
    const bool turn = !d.applied || moved > 0.2f;
    if (turn) d.applied_light = light;
    for (auto& l : d.lights) {
        if (turn) {
            const Vec3 nd = rotate(gx2sky::kGameSun, d.applied_light, l.d0);
            d.put3(mem, l.dir, nd);
        }
        wf(mem, l.intensity.at + 12, l.i0 * lerp(1.f, intensity));
        {
            // Main is rebuilt from area lights: tint the sources as well.
            d.put3(mem, l.diffuse, mix(l.diffuse0, mul(l.diffuse0, color), fd));
            d.put3(mem, l.specular, mix(l.specular0, mul(l.specular0, color), fd));
            // Backlight: a low sun wraps golden light around silhouettes.
            if (l.backside.at) {
                const float rim = moonlit ? 0.f : smoothstep(25.f, 4.f, e) * smoothstep(-2.f, 2.f, e) * weather_direct;
                d.put3(mem, l.backside,
                       mix(l.backside0, backlight_color(l.backside0, color, moonlit, moon_k, rim), fd));
            }
        }
    }
    if (turn && d.found.shadow) mem[d.found.shadow + 0x47C] = 1;

    // Sky light and fog, greyer and closer under clouds.
    const Key k = key_at(e);
    const Key& day = s_keys[0];
    auto grey = [](const Vec3& c, float t) {
        const float y = luma(c);
        return mix(c, Vec3{y * 1.02f, y * 1.04f, y * 1.08f}, t);
    };
    for (auto& h : d.hemis) {
        Vec3 sky_c, ground_c;
        float i;
        if (h.o->name == "Amb" && (!d.main || h.o->group == d.main->o->group)) {
            sky_c = k.sky;
            ground_c = k.ground;
            i = k.intensity;
        } else {
            auto ratio = [](const Vec3& a, const Vec3& b) {
                return Vec3{a[0] / std::max(b[0], 1e-3f), a[1] / std::max(b[1], 1e-3f), a[2] / std::max(b[2], 1e-3f)};
            };
            sky_c = mul(h.sky0, ratio(k.sky, day.sky));
            ground_c = mul(h.ground0, ratio(k.ground, day.ground));
            i = h.i0 * k.intensity / std::max(day.intensity, 1e-3f);
            // Channel ratios cannot turn the character's zero-blue orange fill
            // into moonlight. Preserve brightness and adopt the environment hue.
            const float night = smoothstep(4.f, -6.f, e);
            sky_c = mix(sky_c, ambient_from_environment(h.sky0, day.sky, k.sky), night);
            ground_c = mix(ground_c, ambient_from_environment(h.ground0, day.ground, k.ground), night);
        }
        sky_c = grey(sky_c, 0.75f * overcast);
        ground_c = grey(ground_c, 0.45f * overcast);
        i *= 1 + 0.3f * overcast * smoothstep(-4, 10, e);
        // Preserve the sky's fill light: crushing it cannot create direct-light
        // contrast and hides the surface normals in shaded materials.
        static const float ambient_scale = [] {
            const char* v = std::getenv("RBREW_ENV_AMBIENT_SCALE");
            return v ? float(std::atof(v)) : 1.f;
        }();
        i *= 1.0f + (ambient_scale - 1.0f) * smoothstep(-2.0f, 12.0f, e) * (1.0f - overcast);
        if (s_clock.flash > 0) {
            sky_c = mix(sky_c, Vec3{0.75f, 0.82f, 1.0f}, s_clock.flash * 0.8f);
            i *= 1 + 2.5f * s_clock.flash;
        }
        d.put3(mem, h.sky, mix(h.sky0, sky_c, fd));
        d.put3(mem, h.ground, mix(h.ground0, ground_c, fd));
        wf(mem, h.intensity.at + 12, lerp(h.i0, i));
    }
    // The fog keys are the plaza's: other scenes (Octo Valley's far islands)
    // keep their own fog, it turned everything white.
    for (auto& f : d.fogs) {
        if (!plaza) continue;
        Vec3 c = f.o->name == "FogTPS"
                     ? k.fog
                     : mul(f.c0, Vec3{k.fog[0] / std::max(day.fog[0], 1e-3f), k.fog[1] / std::max(day.fog[1], 1e-3f),
                                      k.fog[2] / std::max(day.fog[2], 1e-3f)});
        const float daylight = smoothstep(-8, 12, e);
        c = mix(c, scale(Vec3{0.52f, 0.55f, 0.60f}, 0.15f + 0.85f * daylight), 0.65f * overcast);
        const float closer = 1 - 0.45f * overcast - 0.2f * rain, farther = 1 - 0.4f * overcast - 0.3f * rain;
        const float s = f.o->name == "FogTPS" ? k.fog_start : f.s0, en = f.o->name == "FogTPS" ? k.fog_end : f.e0;
        d.put3(mem, f.color, mix(f.c0, c, fd));
        wf(mem, f.color.at + 24,
           lerp(f.a0, f.o->name == "FogTPS" ? std::min(1.f, k.fog_alpha + 0.12f * overcast) : f.a0));
        wf(mem, f.start.at + 12, lerp(f.s0, s * closer));
        wf(mem, f.end.at + 12, lerp(f.e0, en * farther));
    }
    // Lamps and baked light come on at dusk and under heavy clouds.
    const float lamps = std::max(smoothstep(4.0f, -5.0f, e), 0.45f * overcast * (0.5f + 0.5f * rain)) * fd;
    lamps_out = lamps;
    float lamp_ceiling = 0;
    for (const auto& l : d.lamps)
        if (l.e0) lamp_ceiling = std::max(lamp_ceiling, l.i0);
    for (auto& l : d.lamps) {
        mem[l.enable.at + 12] = l.e0 || (lamps > .15f && l.i0 > 0 && lamp_ceiling > 0);
        wf(mem, l.intensity.at + 12, night_lamp_intensity(l.i0, l.e0, lamps, lamp_ceiling));
    }
    for (size_t i = 0; i < d.bakes.size(); ++i) wf(mem, d.bakes[i].at + 12, d.bake0[i] + (1.0f - d.bake0[i]) * lamps);
    if (d.has_bloom) {
        mem[d.bloom.enable.at + 12] = d.bloom.e0 || lamps > 0.05f;
        wf(mem, d.bloom.intensity.at + 12, d.bloom.e0 ? d.bloom.i0 : 1.4f * lamps);
    }
    // The lens flare marks the main light; the moon has none, nor a sun behind clouds.
    // Our own glare takes over at a low sun (the game's flare draws cyan ghosts).
    for (auto& f : d.flares) mem[f.first.at + 12] = f.second && !moonlit && sun_k * weather_direct > 0.25f && e > 20.f;
    d.applied = true;
}

// --- Diagnostics ---------------------------------------------------------

// RBREW_ENV_SCAN=<frame>: every parameter object of the heap with its
// parameters to RBREW_ENV_SCAN_OUT (map hashes with CRC32 of the names).
void scan(const uint8_t* mem, uint64_t frame) {
    static const uint64_t at = [] {
        const char* v = std::getenv("RBREW_ENV_SCAN");
        return v ? std::strtoull(v, nullptr, 10) : 0ull;
    }();
    if (!at || frame != at) return;
    const char* path = std::getenv("RBREW_ENV_SCAN_OUT");
    FILE* out = std::fopen(path && *path ? path : "env_scan.txt", "w");
    if (!out) return;
    for_each_heap_region(mem, [&](uint32_t lo, uint32_t hi) {
        for (uint32_t a = lo; a + 64 <= hi; a += 4) {
            if (r32(mem, a) != crc32("enable") || r32(mem, a + 8) != kVtBool) continue;
            std::fprintf(out, "obj %08X\n", a);
            for (uint32_t q = a, n = 0; q >= lo && q + 64 <= hi && n < 80; q = r32(mem, q + 4), ++n) {
                std::fprintf(out, "  p %08X %08X %08X", q, r32(mem, q), r32(mem, q + 8));
                for (int i = 0; i < 16; ++i) std::fprintf(out, " %02x", mem[q + 12 + i]);
                const uint32_t s = r32(mem, q + 12);
                if (s >= lo && s + 64 <= hi && r32(mem, q + 16) >= kVtableLo && r32(mem, q + 16) < kVtableHi) {
                    std::fputs(" s=", out);
                    for (int i = 0; i < 48 && mem[s + i]; ++i) std::fputc(mem[s + i], out);
                }
                std::fputc('\n', out);
                if (!r32(mem, q + 4)) break;
            }
        }
    });
    std::fclose(out);
}

// RBREW_ENV_FIND=<name>[,<name>...],<frame>: every agl parameter in the heap
// whose hash is one of the names, with its value and the chain around it, to
// RBREW_ENV_SCAN_OUT (find game settings such as the camera's).
void find_params(const uint8_t* mem, uint64_t frame) {
    static std::vector<std::string> names;
    static uint64_t at = 0;
    static const bool parsed = [] {
        if (const char* v = std::getenv("RBREW_ENV_FIND")) {
            std::string list = v, cur;
            for (char ch : list + ",") {
                if (ch == ',') {
                    if (!cur.empty()) names.push_back(cur);
                    cur.clear();
                } else
                    cur += ch;
            }
            if (!names.empty() && std::isdigit(static_cast<unsigned char>(names.back()[0]))) {
                at = std::strtoull(names.back().c_str(), nullptr, 10);
                names.pop_back();
            }
        }
        return true;
    }();
    (void)parsed;
    if (!at || frame != at) return;
    const char* path = std::getenv("RBREW_ENV_SCAN_OUT");
    FILE* out = std::fopen(path && *path ? path : "env_find.txt", "w");
    if (!out) return;
    std::vector<uint32_t> hashes;
    for (const auto& n : names) hashes.push_back(crc32(n.c_str()));
    for_each_heap_region(mem, [&](uint32_t lo, uint32_t hi) {
        for (uint32_t a = lo; a + 32 <= hi; a += 4) {
            const uint32_t h = r32(mem, a);
            size_t k = 0;
            while (k < hashes.size() && hashes[k] != h) ++k;
            if (k == hashes.size()) continue;
            const uint32_t vt = r32(mem, a + 8);
            if (vt < kVtableLo || vt >= kVtableHi) continue;
            std::fprintf(out, "%s at %08X vt %08X value %g (%08X)\n", names[k].c_str(), a, vt, rf(mem, a + 12),
                         r32(mem, a + 12));
            // The parameters chained after it (hash, vtable, value).
            for (uint32_t q = r32(mem, a + 4), n = 0; q >= lo && q + 32 <= hi && n < 40; q = r32(mem, q + 4), ++n)
                std::fprintf(out, "   %08X h %08X vt %08X value %g (%08X)\n", q, r32(mem, q), r32(mem, q + 8),
                             rf(mem, q + 12), r32(mem, q + 12));
        }
    });
    std::fclose(out);
}

#if defined(_WIN32)
// RBREW_ENV_TRAP=<hex guest address>,<frame>: from that frame on the page is
// read-only and writes to the 12 bytes at the address log the writer's call
// stack (RVAs into Splatoon.exe; symbolize with build\game\Splatoon.map).
uint8_t* s_trap_page = nullptr;
uintptr_t s_trap_lo = 0, s_trap_hi = 0;
thread_local bool s_trap_step = false;
std::atomic<int> s_trap_logs{0};
LONG CALLBACK trap_handler(EXCEPTION_POINTERS* e) {
    auto* r = e->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2 && r->ExceptionInformation[0] == 1) {
        const uintptr_t addr = r->ExceptionInformation[1];
        if (addr < uintptr_t(s_trap_page) || addr >= uintptr_t(s_trap_page) + 4096) return EXCEPTION_CONTINUE_SEARCH;
        if (addr >= s_trap_lo && addr < s_trap_hi && s_trap_logs.fetch_add(1) < 40) {
            const uintptr_t module = uintptr_t(GetModuleHandleW(nullptr));
            CONTEXT c = *e->ContextRecord;
            char line[512];
            int n = std::snprintf(line, sizeof line, "tid %lu write %+d:", GetCurrentThreadId(), int(addr - s_trap_lo));
            for (int depth = 0; depth < 10 && c.Rip && n < int(sizeof line) - 24; ++depth) {
                n += std::snprintf(line + n, sizeof line - n, " %llx", (unsigned long long)(c.Rip - module));
                DWORD64 image = 0;
                auto* fn = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
                if (!fn) {
                    c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
                    c.Rsp += 8;
                    continue;
                }
                void* data;
                DWORD64 frame;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, fn, &c, &data, &frame, nullptr);
            }
            RBREW_LOG_DEBUG("env-trap", "%s", line);
        }
        DWORD old;
        VirtualProtect(s_trap_page, 4096, PAGE_READWRITE, &old);
        e->ContextRecord->EFlags |= 0x100;
        s_trap_step = true;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && s_trap_step) {
        s_trap_step = false;
        DWORD old;
        VirtualProtect(s_trap_page, 4096, PAGE_READONLY, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
void trap(uint8_t* mem, uint64_t frame) {
    static uint32_t addr = 0;
    static uint64_t at = 0;
    static const bool parsed = [] {
        if (const char* v = std::getenv("RBREW_ENV_TRAP")) {
            char* p;
            addr = uint32_t(std::strtoul(v, &p, 16));
            if (*p == ',') at = std::strtoull(p + 1, nullptr, 10);
        }
        return true;
    }();
    (void)parsed;
    if (!addr || frame != at) return;
    s_trap_lo = uintptr_t(mem + addr);
    s_trap_hi = s_trap_lo + 12;
    s_trap_page = reinterpret_cast<uint8_t*>(s_trap_lo & ~uintptr_t(4095));
    AddVectoredExceptionHandler(1, trap_handler);
    DWORD old;
    VirtualProtect(s_trap_page, 4096, PAGE_READONLY, &old);
    RBREW_LOG_DEBUG("env-trap", "armed %08X", addr);
}
#endif
} // namespace

void frame(uint8_t* mem, uint64_t swap_count) {
    if (!mem) return;
#if defined(_WIN32)
    trap(mem, swap_count);
#endif
    scan(mem, swap_count);
    find_params(mem, swap_count);
    // RBREW_PEEK=<hex>,<hex>...,<frame> (diagnostics): floats at guest addresses.
    {
        static std::vector<uint32_t> peek;
        static uint64_t peek_at = 0;
        static const bool peek_parsed = [] {
            if (const char* v = std::getenv("RBREW_PEEK")) {
                std::vector<uint64_t> all;
                for (const char* q = v; *q;) {
                    char* e;
                    all.push_back(std::strtoull(q, &e, 16));
                    q = *e == ',' ? e + 1 : e;
                    if (e == q && *q) break;
                }
                if (!all.empty()) {
                    peek_at = std::strtoull(std::to_string(all.back()).c_str(), nullptr, 10);
                }
                // The last entry is the frame in decimal: parse it again as decimal.
                const char* last = std::strrchr(v, ',');
                if (last) peek_at = std::strtoull(last + 1, nullptr, 10);
                for (size_t i = 0; i + 1 < all.size(); ++i) peek.push_back(uint32_t(all[i]));
            }
            return true;
        }();
        (void)peek_parsed;
        if (peek_at && swap_count == peek_at)
            for (uint32_t a : peek) RBREW_LOG_DEBUG("peek", "%08X %g", a, rf(mem, a));
    }
    // RBREW_ENV_LIGHT=0: the game's original plaza lights (the sky shaders
    // still run with a fixed sun).
    static const bool enabled = [] {
        const char* v = std::getenv("RBREW_ENV_LIGHT");
        return !(v && v[0] == '0');
    }();
    const auto settings = rbrew_settings::get();
    int scene_id = -1;
    const int scene_type = rbrew_control_defaults::active_scene_type(mem, &scene_id);
    {
        static int last_type = -2, last_id = -2;
        if (scene_type != last_type || scene_id != last_id) {
            RBREW_LOG_INFO("scene", "type %d id %d", scene_type, scene_id);
            last_type = scene_type;
            last_id = scene_id;
        }
    }
    const bool plaza = scene_type == 2;
    // The shaders, field of view and camera features in every 3D scene
    // (tutorial, Octo Valley, levels), not only the plaza; only the plaza's
    // lights follow the clock. RBREW_SHADERS_ANYWHERE=0: plaza only.
    static const bool anywhere = [] {
        const char* v = std::getenv("RBREW_SHADERS_ANYWHERE");
        return !(v && v[0] == '0');
    }();
    // Scene types: 2 plaza, 3 Octo Valley, 5/6 tutorial; 1 = story levels and
    // 16 = title stay original for now. RBREW_SHADERS_ANYWHERE=all: every scene.
    static const bool every = [] {
        const char* v = std::getenv("RBREW_SHADERS_ANYWHERE");
        return v && !std::strcmp(v, "all");
    }();
    // Story levels (type 1): the picture effects only - contact shadows, AO,
    // bloom, edge smoothing, colours, focus blur - with the game's own sky
    // and lights (no time of day, clouds or weather). RBREW_LEVEL_EFFECTS=0: off.
    static const bool level_effects = [] {
        const char* v = std::getenv("RBREW_LEVEL_EFFECTS");
        return !(v && v[0] == '0');
    }();
    const auto policy = scene_graphics(scene_type, anywhere, level_effects, every);
    const bool world = policy.environment, post_only = policy.post_only, camera_scene = world || post_only;
    static uint64_t world_frames = 0;
    static int world_scene = -2;
    static int world_type = -2;
    if (!camera_scene || scene_id != world_scene || scene_type != world_type) {
        world_frames = 0;
        world_scene = scene_id;
        world_type = scene_type;
    }
    if (camera_scene) ++world_frames;
    const bool on = settings.shader_quality != rbrew_settings::kShadersOff && (world || post_only);
    advance(settings);
    const auto sky = gx2sky::celestial(s_clock.hour);
    Driver& d = s_driver;
    float lamps = std::max(smoothstep(4.0f, -5.0f, sky.sun_elevation), 0.45f * s_clock.overcast);
    // The lights follow the clock in every scene whose light group is found
    // (the plaza's is PlazaDay; others are found by their Main light).
    static int light_scene = -2;
    static int light_type = -2;
    if (!(world || post_only) || scene_id != light_scene || scene_type != light_type) {
        if (d.ready && d.all_valid(mem)) d.restore(mem);
        if (d.ready || d.job.valid()) d.reset();
        d.plaza_frames = 0;
        light_scene = scene_id;
        light_type = scene_type;
    }
    if (world || post_only) {
        ++d.plaza_frames;
        if (d.ready && !d.all_valid(mem)) {
            RBREW_LOG_INFO("env", "plaza lights moved; searching again");
            d.reset();
            d.retry_at = d.plaza_frames + 60;
        }
        if (plaza && enabled && on && !d.ready && !d.job.valid() && d.plaza_frames > 20 && d.plaza_frames >= d.retry_at)
            d.job =
                std::async(std::launch::async, search, static_cast<const uint8_t*>(mem), plaza ? "PlazaDay" : nullptr);
        if (d.job.valid() && d.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            d.found = d.job.get();
            d.job = {};
            d.adopt(mem);
            d.ready = !d.lights.empty() && d.all_valid(mem);
            if (!d.ready) {
                d.reset();
                d.retry_at = d.plaza_frames + 60;
            }
        }
        if (d.ready) {
            if (on && enabled && plaza)
                drive(mem, d, sky, lamps, plaza);
            else
                d.restore(mem);
        }
    }
    CameraFrameContext camera;
    camera.mem = mem;
    camera.swap_count = swap_count;
    camera.world_frames = world_frames;
    camera.camera_scene = camera_scene;
    camera.shaders_on = on;
    camera.world = world;
    camera.plaza = plaza;
    camera.sun = sky.sun.data();
    camera.settings = &settings;
    camera_frame(camera);
    std::lock_guard<std::mutex> lock(s_sky_mutex);
    s_sky.active = on;
    s_sky.scene_type = scene_type;
    s_sky.scene_id = scene_id;
    s_sky.plaza = plaza;
    s_sky.post_only = post_only;
    std::memcpy(s_sky.sun, sky.sun.data(), sizeof s_sky.sun);
    std::memcpy(s_sky.moon, sky.moon.data(), sizeof s_sky.moon);
    std::memcpy(s_sky.pole, sky.pole.data(), sizeof s_sky.pole);
    s_sky.sun_elevation = sky.sun_elevation;
    s_sky.moon_elevation = sky.moon_elevation;
    s_sky.star_angle = sky.star_angle;
    s_sky.hour = float(s_clock.hour);
    s_sky.cloud_cover = s_clock.cloud;
    s_sky.rain = s_clock.rain;
    s_sky.wetness = s_clock.wet;
    s_sky.lamps = lamps;
    s_sky.seconds = s_clock.seconds;
    s_sky.lightning = s_clock.flash;
    s_sky.bolt_azimuth = s_clock.bolt_azimuth;
    s_sky.bolt_seed = s_clock.bolt_seed;
    if (on && !plaza) {
        // A scene whose lights were not found keeps its own lighting: sky and
        // post use a fixed afternoon sun from the game's own sun direction.
        const float l =
            std::sqrt(gx2sky::kGameSun[0] * gx2sky::kGameSun[0] + gx2sky::kGameSun[1] * gx2sky::kGameSun[1] +
                      gx2sky::kGameSun[2] * gx2sky::kGameSun[2]);
        for (int i = 0; i < 3; ++i) s_sky.sun[i] = gx2sky::kGameSun[i] / l;
        // A level's own main light gives its real sun (contact shadows must
        // fall the same way as the game's shadows).
        if (post_only && d.ready && d.main) {
            const Vec3 m = d.main->d0;
            const float ml = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
            const float sgn = m[1] < 0.f ? -1.f : 1.f; // the light points down; the sun is the other way
            if (ml > 1e-4f)
                for (int i = 0; i < 3; ++i) s_sky.sun[i] = sgn * m[i] / ml;
        }
        s_sky.sun_elevation = std::asin(std::clamp(s_sky.sun[1], -1.f, 1.f)) * 57.29578f;
        s_sky.moon[0] = -s_sky.sun[0];
        s_sky.moon[1] = -std::abs(s_sky.sun[1]);
        s_sky.moon[2] = -s_sky.sun[2];
        s_sky.moon_elevation = -s_sky.sun_elevation;
        s_sky.hour = 14.5f;
        s_sky.lamps = 0.f;
        s_sky.cloud_cover = std::min(s_clock.cloud, 0.5f);
        s_sky.rain = 0.f;
        s_sky.wetness = 0.f;
        s_sky.lightning = 0.f;
    }
}

SkyState sky_state() {
    std::lock_guard<std::mutex> lock(s_sky_mutex);
    return s_sky;
}
} // namespace gx2env

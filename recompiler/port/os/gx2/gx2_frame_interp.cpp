#include "gx2_frame_interp.h"
#include "../../runtime/rebrewu_runtime.h"
#include "gx2_native_context.h"
#include "../../runtime/port_settings.h"
#include "../../runtime/benchmark_input.h"
#include "vk/gx2_vk_capture.h"
#include "vk/gx2_render_vk_state.h"

#include <algorithm>
#include <bit>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

void gx2_render_query_poll_completed(); // gx2_render.h

namespace gx2interp {
namespace {
using Clock = std::chrono::steady_clock;
uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

std::atomic<bool> s_wanted{false};

// One draw unit's uniform bytes: the packed register block and the guest
// uniform blocks, per stage (0 vertex, 1 pixel), as offsets into Record::bytes.
struct Unit {
    uint64_t key = 0, base = 0; // key = base + occurrence
    uint32_t uf_off[2]{}, uf_size[2]{};
    uint32_t bank_off[2][16]{}, bank_size[2][16]{};
    uint32_t bank_mask[2]{};
    bool has_uf[2]{};
    // Uniform buffers the draw bound (for the Vulkan replay's substitution).
    // owner: the unit whose recorded bytes the buffer holds (a draw that
    // does not set a block keeps the binding an earlier draw uploaded).
    struct Ref {
        uint64_t buffer, offset;
        uint32_t owner;
        uint8_t stage, bank;
    }; // bank 255: register block
    Ref refs[36];
    bool transparent = false; // color blending on (glass, effects)
    uint8_t ref_count = 0;
};
struct Record {
    std::vector<Unit> units;
    std::vector<uint8_t> bytes;
    std::unordered_map<uint64_t, uint32_t> by_key;
    std::unordered_map<uint64_t, uint32_t> occurrences; // per base key: how many draws
    void clear() {
        units.clear();
        bytes.clear();
        by_key.clear();
        occurrences.clear();
    }
    uint32_t put(const void* data, size_t size) {
        const uint32_t offset = uint32_t(bytes.size());
        bytes.insert(bytes.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        return offset;
    }
};
Record s_records[2];
Record* s_cur = &s_records[0];                        // frame being recorded (frame N during its replay)
Record* s_last = &s_records[1];                       // previous complete frame (N-1 for the replay)
std::unordered_map<uint64_t, uint32_t> s_occurrences; // per frame (record) or replay
uint32_t s_unit = UINT32_MAX;                         // recording: index of the current unit
const Unit* s_replay_cur = nullptr;                   // replay: N's unit for this draw
const Unit* s_replay_prev = nullptr;                  // replay: N-1's matching unit

std::vector<Gx2AsyncWork*> s_works, s_replay_works;
bool s_replaying = false, s_pending_replay = false, s_frame_broken = false;
// Vulkan replay (gx2_vk_capture.h): the captured commands of frame N, issued
// again with blended uniform buffers. RBREW_INTERP_VK=0 replays the GX2
// commands instead (the first implementation, far more render-thread time).
bool s_vk_replay = false, s_capture_mid_frame = false, s_capture_armed = false;
uint64_t s_frame_first_serial = 0;     // recording serial when the frame's capture started
std::vector<const void*> s_prev_cache; // per unit of N: matched N-1 unit (or the sentinel)
struct SpanOwner {
    uint32_t unit;
    uint8_t stage, bank;
};
struct Substituted {
    VkBuffer buffer;
    VkDeviceSize offset;
};
std::unordered_map<uint64_t, Substituted> s_substituted; // replay: (owner, stage, bank, range) -> blended span
std::unordered_map<uint64_t, Substituted> s_by_content;  // replay: block bytes -> what the first draw with them got
std::unordered_map<uint64_t, SpanOwner> s_span_owner;    // this frame: uniform span -> unit that recorded its bytes
uint64_t span_key(uint64_t buffer, uint64_t offset) {
    return buffer * 0x9E3779B97F4A7C15ull ^ offset;
}
bool vk_mode() {
    static const bool on = [] {
        const char* v = std::getenv("RBREW_INTERP_VK");
        return !(v && v[0] == '0');
    }();
    return on;
}
unsigned s_scan_index = 0;
bool s_held = false;
uint64_t s_held_due = 0;
uint64_t s_last_complete = 0;
double s_last_complete_interval_ns = 0;
double s_interval_ns = 16683333.0;
// Render-thread budget: a frame's own work plus its replay must fit in one
// frame interval, or the game (which waits for the renderer at its swap)
// slows down. Averages decide; a skipped frame is simply presented at once.
uint64_t s_frame_busy_ns = 0;
double s_busy_avg_ns = 0, s_replay_avg_ns = 0;
unsigned s_skip_streak = 0;
// Share of frames that get an in-between frame, steered by the game's own
// frame interval: the game waits for the renderer at queries and at its swap,
// so render time alone does not tell whether a replay slows the game.
double s_ratio = 1.0, s_credit = 0.0, s_game_interval_ns = 0;
// In-between frames per game frame (1: 120 FPS, 2: 180 FPS) and the blend
// position of the one being drawn (1/2; 1/3 and 2/3).
unsigned s_replay_count = 1;
// Per replay: matrices blended and matrices kept because they jumped.
unsigned s_blended_matrices = 0, s_jumped_matrices = 0;
float s_blend_t = 0.5f;
bool s_mid = false; // 180 FPS: the second in-between frame waits
uint64_t s_mid_due = 0;
std::vector<uint8_t> s_blend; // replay_bank result

struct Stats {
    unsigned frames = 0, replays = 0, held = 0, skipped = 0, units = 0, matched = 0, missing = 0, failed = 0,
             changed = 0, set_binds = 0, cuts = 0;
    double replay_ms = 0, busy_ms = 0;
};
Stats s_stats;
FILE* stats_file() {
    static FILE* f = [] {
        const char* p = std::getenv("RBREW_INTERP_STATS");
        return p && *p ? std::fopen(p, "w") : nullptr;
    }();
    return f;
}
void report_stats() {
    FILE* f = stats_file();
    if (!f) return;
    static uint64_t second = now_ns();
    const uint64_t now = now_ns();
    if (now - second < 1000000000ull) return;
    std::fprintf(f,
                 "frames %u replays %u skipped %u held %u ratio %.2f | units %u matched %.0f%% changed %u set_binds %u "
                 "missing %u | replay %.2f ms frame busy %.2f ms | failed %u cuts %u\n",
                 s_stats.frames, s_stats.replays, s_stats.skipped, s_stats.held, s_ratio, s_stats.units,
                 s_stats.units ? 100.0 * s_stats.matched / s_stats.units : 0.0, s_stats.changed, s_stats.set_binds,
                 s_stats.missing, s_stats.replays ? s_stats.replay_ms / s_stats.replays : 0.0,
                 s_stats.frames ? s_stats.busy_ms / s_stats.frames : 0.0, s_stats.failed, s_stats.cuts);
    std::fflush(f);
    s_stats = {};
    second = now;
}

uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

void drop_works(std::vector<Gx2AsyncWork*>& works) {
    for (Gx2AsyncWork* work : works) delete work;
    works.clear();
}

// A float word that blends: finite, not denormal/huge (else it is an integer
// or bit pattern), magnitude in a plausible range.
bool plausible(float v) {
    if (!std::isfinite(v)) return false;
    const float a = std::fabs(v);
    return a == 0.0f || (a > 1e-20f && a < 1e20f);
}
float to_float(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
uint32_t to_bits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
uint32_t be(const uint8_t* p) {
    uint32_t w;
    std::memcpy(&w, p, 4);
    return rbrew_bswap32(w);
}
uint32_t le(const uint8_t* p) {
    uint32_t w;
    std::memcpy(&w, p, 4);
    return w;
}
// Only transforms are blended: three consecutive vec4 rows (or columns)
// whose 3x3 parts are mutually orthogonal - camera, view-projection, object
// and bone matrices - plus the vec4 after them (the translation column or
// the projection's w row) unless it starts another matrix. Every other word
// (ink and material parameters, colours, counters, packed data) stays at
// frame N: blending those turned the ground blue and hid the player.
// Guest uniform blocks are big-endian; the renderer's packed register block
// (uniform registers, remapped constants) is host order.
bool vec3_ok(const float* v, float& norm) {
    for (int i = 0; i < 4; ++i)
        if (!plausible(v[i])) return false;
    norm = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return norm > 1e-6f && norm < 1e7f;
}
bool is_matrix(const float* m) { // m: 3 vec4
    float n0, n1, n2;
    if (!vec3_ok(m, n0) || !vec3_ok(m + 4, n1) || !vec3_ok(m + 8, n2)) return false;
    auto ortho = [](const float* a, const float* b, float na, float nb) {
        return std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) <= 0.02f * na * nb;
    };
    return ortho(m, m + 4, n0, n1) && ortho(m, m + 8, n0, n2) && ortho(m + 4, m + 8, n1, n2);
}
bool vec4_ok(const float* v) {
    for (int i = 0; i < 4; ++i)
        if (!plausible(v[i])) return false;
    return true;
}
bool parallel(const float* x, const float* y) {
    const float nx = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]),
                ny = std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    if (nx < 1e-6f || ny < 1e-6f) return false;
    return std::fabs(x[0] * y[0] + x[1] * y[1] + x[2] * y[2]) >= 0.98f * nx * ny;
}
// Not motion between two frames: a row turning by more than ~20 degrees
// (another object, a camera cut) or a translation changing by more than a
// quarter of its size (a texture matrix switching buffers, a teleport).
bool transform_jump(const float* a, const float* b) {
    for (int row = 0; row < 3; ++row) {
        const float* x = a + row * 4;
        const float* y = b + row * 4;
        const float dx = x[0] - y[0], dy = x[1] - y[1], dz = x[2] - y[2];
        const float diff = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float scale = std::max(std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]),
                                     std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]));
        if (diff > 0.35f * scale) return true;
    }
    const float dt =
        std::sqrt((a[3] - b[3]) * (a[3] - b[3]) + (a[7] - b[7]) * (a[7] - b[7]) + (a[11] - b[11]) * (a[11] - b[11]));
    const float ta = std::sqrt(a[3] * a[3] + a[7] * a[7] + a[11] * a[11]),
                tb = std::sqrt(b[3] * b[3] + b[7] * b[7] + b[11] * b[11]);
    return dt > 0.05f && dt > 0.25f * std::max(ta, tb);
}
void blend_bytes(const uint8_t* prev, const uint8_t* cur, uint8_t* out, size_t size, bool host_order) {
    std::memcpy(out, cur, size);
    if (std::memcmp(prev, cur, size) == 0) return;
    // Only rows (vec4) that differ can start or belong to a moving matrix:
    // the 4 KB register block of a draw usually differs in a few rows.
    const size_t rows = size / 16;
    auto read = [&](const uint8_t* p) { return to_float(host_order ? le(p) : be(p)); };
    auto load = [&](const uint8_t* base, size_t row, size_t count, float* dst) {
        for (size_t i = 0; i < count * 4; ++i) dst[i] = read(base + row * 16 + i * 4);
    };
    auto put = [&](size_t word, float v) {
        const uint32_t w = host_order ? to_bits(v) : rbrew_bswap32(to_bits(v));
        std::memcpy(out + word * 4, &w, 4);
    };
    for (size_t r = 0; r < rows;) {
        if (std::memcmp(prev + r * 16, cur + r * 16, 16) == 0) {
            ++r;
            continue;
        }
        size_t next = r + 1;
        for (size_t start = r >= 3 ? r - 3 : 0; start <= r && start + 3 <= rows; ++start) {
            float a[16], b[16];
            const size_t avail = std::min<size_t>(4, rows - start);
            load(prev, start, avail, a);
            load(cur, start, avail, b);
            if (!is_matrix(a) || !is_matrix(b)) continue;
            // The 4th vec4 belongs to it only as a projection's w row, which is
            // parallel to the 3rd row. In the register block the vec4 after a
            // 3x4 matrix is another parameter (ink, material): blending those
            // made the ground ink flicker.
            size_t n = 12;
            if (avail == 4 && vec4_ok(a + 12) && vec4_ok(b + 12) && parallel(a + 8, a + 12) && parallel(b + 8, b + 12))
                n = 16;
            if (transform_jump(a, b)) {
                ++s_jumped_matrices;
                next = std::max(next, start + n / 4);
                break;
            } // keep frame N
            ++s_blended_matrices;
            for (size_t k = 0; k < n; ++k)
                if (a[k] != b[k]) put(start * 4 + k, a[k] + (b[k] - a[k]) * s_blend_t);
            next = std::max(next, start + n / 4);
            break;
        }
        r = next;
    }
}
// How different two recordings of a draw are (sum of float differences,
// capped per word): picks, among the previous frame's draws of the same
// program and mesh, the one that is the same object.
double distance(const Record& ra, const Unit& a, const Record& rb, const Unit& b) {
    double total = 0;
    auto add = [&](const uint8_t* pa, const uint8_t* pb, size_t size, bool host_order) {
        size = std::min<size_t>(size, 256);
        for (size_t i = 0; i + 4 <= size; i += 4) {
            const uint32_t wa = host_order ? le(pa + i) : be(pa + i), wb = host_order ? le(pb + i) : be(pb + i);
            if (wa == wb) continue;
            const float fa = to_float(wa), fb = to_float(wb);
            total += plausible(fa) && plausible(fb) ? std::min(1000.0, double(std::fabs(fa - fb))) : 1.0;
        }
    };
    for (unsigned stage = 0; stage < 2; ++stage) {
        if (a.has_uf[stage] && b.has_uf[stage] && a.uf_size[stage] == b.uf_size[stage])
            add(ra.bytes.data() + a.uf_off[stage], rb.bytes.data() + b.uf_off[stage], a.uf_size[stage], true);
        for (uint32_t m = a.bank_mask[stage] & b.bank_mask[stage]; m; m &= m - 1) {
            const unsigned bank = unsigned(std::countr_zero(m));
            if (a.bank_size[stage][bank] == b.bank_size[stage][bank])
                add(ra.bytes.data() + a.bank_off[stage][bank], rb.bytes.data() + b.bank_off[stage][bank],
                    a.bank_size[stage][bank], false);
        }
    }
    return total;
}
bool no_blend() { // RBREW_INTERP_NOBLEND=1 (diagnostics): replay frame N exactly
    static const bool on = [] {
        const char* v = std::getenv("RBREW_INTERP_NOBLEND");
        return v && v[0] == '1';
    }();
    return on;
}
// N-1's unit for N's unit `cur`: one previous draw of the same program and
// mesh is that one; several (NPCs, ink drops with one mesh, whose order
// changes with sorting) the most similar; very many (particles) none.
const Unit* match_prev(const Unit& cur) {
    if (no_blend()) return nullptr;
    // RBREW_INTERP_KEEP_TRANSPARENT=1 (testing): transparent draws keep frame N.
    static const bool keep_transparent = [] {
        const char* v = std::getenv("RBREW_INTERP_KEEP_TRANSPARENT");
        return v && v[0] == '1';
    }();
    if (cur.transparent && keep_transparent) return nullptr;
    const auto prev = s_last->occurrences.find(cur.base);
    if (prev == s_last->occurrences.end()) return nullptr;
    const uint32_t count = prev->second;
    auto unit_of = [&](uint32_t occurrence) -> const Unit* {
        const auto it = s_last->by_key.find(mix(cur.base, occurrence));
        return it != s_last->by_key.end() ? &s_last->units[it->second] : nullptr;
    };
    if (count == 1) return unit_of(1);
    // RBREW_INTERP_UNIQUE_ONLY=1 (diagnostics): never blend draws with copies.
    static const bool unique_only = [] {
        const char* v = std::getenv("RBREW_INTERP_UNIQUE_ONLY");
        return v && v[0] == '1';
    }();
    if (count > 48 || unique_only) return nullptr;
    // Draw order is mostly stable: the same occurrence, when it is close.
    const Unit* best_unit = nullptr;
    double best = 1e300;
    for (uint32_t occurrence = 1; occurrence <= count; ++occurrence) {
        if (mix(cur.base, occurrence) != cur.key) continue;
        if (const Unit* same = unit_of(occurrence)) {
            best = distance(*s_cur, cur, *s_last, *same);
            best_unit = same;
            if (best < 2.0) return same;
        }
        break;
    }
    // Many copies (text glyphs, ink drops, particles): only an unchanged
    // position in the order counts (above); never blend two different ones.
    if (count > 8) return best < 2.0 ? best_unit : nullptr;
    for (uint32_t occurrence = 1; occurrence <= count && best > 0; ++occurrence)
        if (const Unit* candidate = unit_of(occurrence); candidate && candidate != best_unit) {
            const double d = distance(*s_cur, cur, *s_last, *candidate);
            if (d < best) {
                best = d;
                best_unit = candidate;
            }
        }
    return best_unit;
}
// Vulkan replay: a uniform buffer bound by N's draw `unit` is replaced by its
// N-1/N blend, uploaded for the in-between frame.
bool substitute(uint32_t unit, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range, VkBuffer& out_buffer,
                VkDeviceSize& out_offset) {
    if (unit >= s_cur->units.size()) return false;
    const Unit& binder = s_cur->units[unit];
    const uint64_t handle = uint64_t(reinterpret_cast<uintptr_t>(buffer));
    const Unit::Ref* ref = nullptr;
    for (uint8_t i = 0; i < binder.ref_count; ++i)
        if (binder.refs[i].buffer == handle && binder.refs[i].offset == offset) {
            ref = &binder.refs[i];
            break;
        }
    if (!ref || ref->owner >= s_cur->units.size()) return false;
    const Unit& cur = s_cur->units[ref->owner];
    // Draws that share a span share its blend.
    const uint64_t cache_key =
        (uint64_t(ref->owner) << 40) ^ (uint64_t(ref->stage) << 36) ^ (uint64_t(ref->bank) << 28) ^ uint64_t(range);
    if (const auto hit = s_substituted.find(cache_key); hit != s_substituted.end()) {
        if (!hit->second.buffer) return false;
        out_buffer = hit->second.buffer;
        out_offset = hit->second.offset;
        return true;
    }
    s_substituted[cache_key] = {VK_NULL_HANDLE, 0}; // "no substitution" unless replaced below
    // The pre-pass and the main pass draw an object with the same uniform
    // bytes. Whatever the first draw with these bytes got (blended or frame
    // N), every later one gets too: matched separately, one pass could blend
    // an object the other kept, and the main pass's depth test against the
    // pre-pass then left holes (a green floor). RBREW_INTERP_CONTENT_CACHE=0:
    // per draw.
    static const bool content_cache = [] {
        const char* v = std::getenv("RBREW_INTERP_CONTENT_CACHE");
        return !(v && v[0] == '0');
    }();
    uint64_t content_key = 0;
    if (content_cache) {
        const uint8_t* bytes = nullptr;
        size_t length = 0;
        const unsigned st = ref->stage;
        if (ref->bank == 255) {
            if (cur.has_uf[st]) {
                bytes = s_cur->bytes.data() + cur.uf_off[st];
                length = cur.uf_size[st];
            }
        } else if (cur.bank_mask[st] >> ref->bank & 1) {
            bytes = s_cur->bytes.data() + cur.bank_off[st][ref->bank];
            length = cur.bank_size[st][ref->bank];
        }
        if (bytes) {
            uint64_t h = 1469598103934665603ull ^ length ^ (uint64_t(ref->bank == 255) << 63);
            for (size_t i = 0; i + 8 <= length; i += 8) {
                uint64_t w;
                std::memcpy(&w, bytes + i, 8);
                h = (h ^ w) * 1099511628211ull;
            }
            for (size_t i = length & ~size_t(7); i < length; ++i) h = (h ^ bytes[i]) * 1099511628211ull;
            content_key = h ^ uint64_t(range) * 0x9E3779B97F4A7C15ull;
            if (const auto hit = s_by_content.find(content_key); hit != s_by_content.end()) {
                s_substituted[cache_key] = hit->second;
                if (!hit->second.buffer) return false;
                out_buffer = hit->second.buffer;
                out_offset = hit->second.offset;
                return true;
            }
            s_by_content[content_key] = {VK_NULL_HANDLE, 0};
        }
    }
    if (s_prev_cache.size() != s_cur->units.size()) s_prev_cache.assign(s_cur->units.size(), &s_prev_cache);
    if (s_prev_cache[ref->owner] == &s_prev_cache) s_prev_cache[ref->owner] = match_prev(cur);
    const Unit* prev = static_cast<const Unit*>(s_prev_cache[ref->owner]);
    ++s_stats.units;
    if (!prev) return false; // frame N's own data is already bound
    const unsigned stage = ref->stage;
    const uint8_t* a = nullptr;
    const uint8_t* b = nullptr;
    size_t size = 0;
    if (ref->bank == 255) {
        if (!cur.has_uf[stage] || !prev->has_uf[stage] || cur.uf_size[stage] != prev->uf_size[stage]) return false;
        size = cur.uf_size[stage];
        a = s_last->bytes.data() + prev->uf_off[stage];
        b = s_cur->bytes.data() + cur.uf_off[stage];
    } else {
        const unsigned bank = ref->bank;
        if (!(cur.bank_mask[stage] >> bank & 1) || !(prev->bank_mask[stage] >> bank & 1) ||
            cur.bank_size[stage][bank] != prev->bank_size[stage][bank])
            return false;
        size = cur.bank_size[stage][bank];
        a = s_last->bytes.data() + prev->bank_off[stage][bank];
        b = s_cur->bytes.data() + cur.bank_off[stage][bank];
    }
    if (std::memcmp(a, b, size) == 0) return false; // unchanged: keep N's buffer
    const VkDeviceSize bytes = std::max<VkDeviceSize>(range, size);
    const VkDeviceSize align = std::max<VkDeviceSize>(gx2vk::caps().limits.minUniformBufferOffsetAlignment, 16);
    const gx2vk::Span span = gx2vk::upload(bytes, align);
    if (!span.data) return false;
    // Blend in host memory: the upload span is write-combined, reading it
    // back (a comparison) cost ~25 us per block.
    thread_local std::vector<uint8_t> blended;
    blended.resize(size);
    blend_bytes(a, b, blended.data(), size, ref->bank == 255);
    if (std::memcmp(blended.data(), b, size) != 0) ++s_stats.changed;
    std::memcpy(span.data, blended.data(), size);
    // Bindings often declare 64 KB for a block of a few hundred bytes; zeroing
    // all of it was 70 MB per in-between frame. The game's block is all the
    // shader reads; a short zeroed tail covers vec4 over-reads.
    if (bytes > size) std::memset(span.data + size, 0, size_t(std::min<VkDeviceSize>(bytes - size, 1024)));
    ++s_stats.matched;
    out_buffer = span.buffer;
    out_offset = span.offset;
    s_substituted[cache_key] = {span.buffer, span.offset};
    if (content_key) s_by_content[content_key] = {span.buffer, span.offset};
    return true;
}
// Capture follows the frame: on while a frame is recorded for interpolation.
void sync_capture() {
    if (!vk_mode()) return;
    const bool want = capturing();
    if (want && !gx2vk::capture::enabled()) {
        gx2vk::capture::install();
        gx2vk::capture::set_enabled(true);
        s_capture_mid_frame = !s_capture_armed; // false: the frame starts here
        s_frame_first_serial = gx2vk::recording_serial();
        s_capture_armed = false;
    } else if (!want && gx2vk::capture::enabled()) {
        gx2vk::capture::set_enabled(false);
        gx2vk::capture::discard();
    }
}
} // namespace

// ESC menu "120 FPS mode" (settings.ini fps120; RBREW_FPS120=interp turns it on).
bool mode_enabled() {
    return rbrew_settings::g_fps120.load(std::memory_order_relaxed);
}
void set_wanted(bool wanted) {
    s_wanted.store(wanted && mode_enabled(), std::memory_order_relaxed);
}
bool capturing() {
    // keep() runs after the present has scheduled its replay. Reopening the
    // capture there would record the replay's swapchain operations as game work.
    return !s_replaying && !s_pending_replay && s_wanted.load(std::memory_order_relaxed);
}
bool replaying() {
    return s_replaying;
}
bool replay_draw_known() {
    return s_replay_cur != nullptr;
}
void add_busy(uint64_t ns) {
    if (!s_replaying) s_frame_busy_ns += ns;
}

void note_ubo(unsigned stage, unsigned bank, uint64_t buffer, uint64_t offset) {
    if (s_unit == UINT32_MAX || s_replaying || stage > 1) return;
    Unit& unit = s_cur->units[s_unit];
    if (unit.ref_count >= sizeof(unit.refs) / sizeof(unit.refs[0])) return;
    const bool own = bank == 255 ? unit.has_uf[stage] : bank < 16 && (unit.bank_mask[stage] >> bank & 1);
    const uint64_t key = span_key(buffer, offset);
    SpanOwner owner{s_unit, uint8_t(stage), uint8_t(bank)};
    if (own)
        s_span_owner.try_emplace(key, owner);
    else if (const auto it = s_span_owner.find(key); it != s_span_owner.end())
        owner = it->second;
    else
        return; // bound before this frame's capture: no bytes to blend
    unit.refs[unit.ref_count++] = {buffer, offset, owner.unit, owner.stage, owner.bank};
}
void before_item() {
    sync_capture();
} // render thread, before each queue item
void present_begin() { // gx2_render_present: the present's own commands are not the frame's
    gx2r::end_rendering();
    if (gx2vk::capture::enabled()) gx2vk::capture::set_enabled(false);
}

void keep(Gx2AsyncWork* work) {
    sync_capture();
    if (!capturing() || !work->replayable() || s_frame_broken) {
        delete work;
        return;
    }
    s_works.push_back(work);
    // A loading screen without presents must not grow without bound.
    if (s_works.size() > 400000) {
        drop_works(s_works);
        s_frame_broken = true;
    }
}

void draw_unit(uint64_t key, bool transparent) {
    const uint64_t full = mix(key, ++s_occurrences[key]);
    if (s_replaying) {
        s_replay_cur = nullptr;
        s_replay_prev = nullptr;
        // By key, not position: a draw skipped in one run (shader still
        // compiling) must not shift every later draw onto other data.
        if (const auto it = s_cur->by_key.find(full); it != s_cur->by_key.end()) {
            s_replay_cur = &s_cur->units[it->second];
            if (!no_blend())
                if (const auto prev = s_last->occurrences.find(key); prev != s_last->occurrences.end()) {
                    // One previous draw: that one. Several (NPCs, ink drops
                    // with one mesh, whose order changes with sorting): the
                    // most similar. Very many (particles): no blending.
                    const uint32_t count = prev->second;
                    auto unit_of = [&](uint32_t occurrence) -> const Unit* {
                        const auto it = s_last->by_key.find(mix(key, occurrence));
                        return it != s_last->by_key.end() ? &s_last->units[it->second] : nullptr;
                    };
                    if (count == 1)
                        s_replay_prev = unit_of(1);
                    else if (count <= 48) {
                        double best = 1e300;
                        for (uint32_t occurrence = 1; occurrence <= count && best > 0; ++occurrence)
                            if (const Unit* candidate = unit_of(occurrence)) {
                                const double d = distance(*s_cur, *s_replay_cur, *s_last, *candidate);
                                if (d < best) {
                                    best = d;
                                    s_replay_prev = candidate;
                                }
                            }
                    }
                }
        } else {
            ++s_stats.missing;
        }
        ++s_stats.units;
        if (s_replay_prev) ++s_stats.matched;
        return;
    }
    s_unit = UINT32_MAX;
    sync_capture();
    if (!capturing()) return;
    s_unit = uint32_t(s_cur->units.size());
    gx2vk::capture::set_unit(s_unit);
    s_cur->by_key.emplace(full, s_unit);
    s_cur->occurrences[key] = s_occurrences[key];
    s_cur->units.push_back({});
    s_cur->units.back().key = full;
    s_cur->units.back().base = key;
    s_cur->units.back().transparent = transparent;
}

void record_uf(unsigned stage, const void* data, size_t size) {
    if (s_unit == UINT32_MAX || s_replaying || stage > 1) return;
    Unit& unit = s_cur->units[s_unit];
    unit.uf_off[stage] = s_cur->put(data, size);
    unit.uf_size[stage] = uint32_t(size);
    unit.has_uf[stage] = true;
}
void record_bank(unsigned stage, unsigned bank, const void* data, size_t size) {
    if (s_unit == UINT32_MAX || s_replaying || stage > 1 || bank > 15) return;
    Unit& unit = s_cur->units[s_unit];
    unit.bank_off[stage][bank] = s_cur->put(data, size);
    unit.bank_size[stage][bank] = uint32_t(size);
    unit.bank_mask[stage] |= 1u << bank;
}

bool replay_uf(unsigned stage, void* data, size_t size) {
    if (!s_replaying || !s_replay_cur || stage > 1 || !s_replay_cur->has_uf[stage] ||
        s_replay_cur->uf_size[stage] != size)
        return false;
    const uint8_t* cur = s_cur->bytes.data() + s_replay_cur->uf_off[stage];
    if (s_replay_prev && s_replay_prev->has_uf[stage] && s_replay_prev->uf_size[stage] == size)
        blend_bytes(s_last->bytes.data() + s_replay_prev->uf_off[stage], cur, static_cast<uint8_t*>(data), size, true);
    else
        std::memcpy(data, cur, size);
    return true;
}
const uint8_t* replay_bank(unsigned stage, unsigned bank, size_t size) {
    if (!s_replaying || !s_replay_cur || stage > 1 || bank > 15 || !(s_replay_cur->bank_mask[stage] >> bank & 1) ||
        s_replay_cur->bank_size[stage][bank] != size)
        return nullptr;
    const uint8_t* cur = s_cur->bytes.data() + s_replay_cur->bank_off[stage][bank];
    s_blend.resize(size);
    if (s_replay_prev && (s_replay_prev->bank_mask[stage] >> bank & 1) && s_replay_prev->bank_size[stage][bank] == size)
        blend_bytes(s_last->bytes.data() + s_replay_prev->bank_off[stage][bank], cur, s_blend.data(), size, false);
    else
        std::memcpy(s_blend.data(), cur, size);
    return s_blend.data();
}

namespace {
void rotate() {
    std::swap(s_last, s_cur); // the finished frame becomes N-1
    s_cur->clear();
    s_occurrences.clear();
    s_unit = UINT32_MAX;
    s_frame_broken = false;
    s_prev_cache.clear();
    s_span_owner.clear();
    // The next frame is captured from its first command (before_item), not
    // from here: the present that follows is not part of it.
    s_capture_armed = true;
}
void present_mid_now() {
    if (!s_mid) return;
    s_mid = false;
    gx2r::end_rendering();
    const bool capture = gx2vk::capture::enabled();
    if (capture) gx2vk::capture::set_enabled(false);
    gx2_render_interp_present_mid();
    if (capture) gx2vk::capture::set_enabled(true);
    ++s_stats.replays;
}
void present_held_now() {
    present_mid_now(); // in order, also when late
    if (!s_held) return;
    s_held = false;
    gx2r::end_rendering();
    const bool capture = gx2vk::capture::enabled();
    if (capture) gx2vk::capture::set_enabled(false);
    gx2_render_interp_present_held();
    if (capture) gx2vk::capture::set_enabled(true);
    ++s_stats.held;
}
} // namespace

bool frame_complete(unsigned scan_index) {
    const uint64_t now = now_ns();
    if (s_last_complete) {
        const double interval = double(now - s_last_complete);
        if (interval > 4e6 && interval < 50e6) s_interval_ns = s_interval_ns * 0.9 + interval * 0.1;
    }
    s_last_complete_interval_ns = s_last_complete ? double(now - s_last_complete) : 0;
    s_last_complete = now;
    present_held_now(); // late: the previous held frame goes out first
    ++s_stats.frames;
    s_stats.busy_ms += double(s_frame_busy_ns) / 1e6;
    s_busy_avg_ns = s_busy_avg_ns ? s_busy_avg_ns * 0.8 + double(s_frame_busy_ns) * 0.2 : double(s_frame_busy_ns);
    s_frame_busy_ns = 0;
    report_stats();
    gx2vk::capture::set_enabled(false);
    gx2vk::capture::finish_frame(); // the captured commands become the replay frame
    s_vk_replay = vk_mode() && !s_capture_mid_frame && gx2vk::capture::replay_complete();
    bool replay = capturing() && !s_frame_broken && !s_last->units.empty() && !s_cur->units.empty() &&
                  (s_vk_replay || !s_works.empty());
    // The game must keep its 60 logic steps per second: when frames take
    // longer than a 60 Hz frame on average, fewer in-between frames are
    // drawn (down to none); when it keeps up, more (up to every frame).
    // RBREW_INTERP_ALWAYS=1 (diagnostics) replays every frame.
    constexpr double kLogicFrameNs = 1e9 * 1001.0 / 60000.0;
    static const bool always = [] {
        const char* v = std::getenv("RBREW_INTERP_ALWAYS");
        return v && v[0] == '1';
    }();
    // A single hitch (streaming, 30-70 ms) counts as at most 1.5 frames: at
    // full weight it kept the average above the limit for 20+ frames and cut
    // the in-between frames for half a second (2026-10-27).
    // RBREW_INTERP_HITCH_CLAMP=0: full weight.
    static const bool clamp_hitch = [] {
        const char* v = std::getenv("RBREW_INTERP_HITCH_CLAMP");
        return !(v && v[0] == '0');
    }();
    if (s_last_complete_interval_ns > 0 && s_last_complete_interval_ns < 100e6) {
        const double sample =
            clamp_hitch ? std::min(s_last_complete_interval_ns, kLogicFrameNs * 1.5) : s_last_complete_interval_ns;
        s_game_interval_ns = s_game_interval_ns ? s_game_interval_ns * 0.85 + sample * 0.15 : sample;
    }
    // Sustained slowdowns only: the game alone has single slow frames
    // (streaming), and a half share of in-between frames judders more than
    // none. The interval average reacts within ~10 frames.
    static unsigned s_slow_frames = 0;
    s_slow_frames = s_game_interval_ns > kLogicFrameNs * 1.03 ? s_slow_frames + 1 : 0;
    if (s_slow_frames > 20)
        s_ratio = std::max(0.0, s_ratio - 0.05);
    else if (s_game_interval_ns < kLogicFrameNs * 1.01)
        s_ratio = std::min(1.0, s_ratio + 0.05);
    // No in-between frame right after a hitch: those are loading frames that
    // set up the ink map (uploads the capture does not repeat). Replaying
    // them painted the whole tutorial level pink for good. The hitch clamp
    // above no longer stops replays there by accident, so this does.
    static unsigned s_after_hitch = 0;
    if (s_last_complete_interval_ns > kLogicFrameNs * 3)
        s_after_hitch = 10; // > 50 ms
    else if (s_after_hitch)
        --s_after_hitch;
    if (s_after_hitch && clamp_hitch) {
        replay = false;
        ++s_stats.skipped;
    }
    // No in-between frames on loading screens (few draws) and for a second
    // after them: those frames set up state the next frames keep (exposure,
    // ink map). Replayed - twice per frame at 180 FPS - they left Octo
    // Valley completely black until the next loading screen.
    // RBREW_INTERP_LOAD_GUARD=0: off.
    static const bool load_guard = [] {
        const char* v = std::getenv("RBREW_INTERP_LOAD_GUARD");
        return !(v && v[0] == '0');
    }();
    static unsigned s_after_load = 0;
    if (rbrew_benchmark::frame_draws.load(std::memory_order_relaxed) < 300)
        s_after_load = 60;
    else if (s_after_load)
        --s_after_load;
    if (s_after_load && load_guard && replay) {
        replay = false;
        ++s_stats.skipped;
    }
    // 180 FPS (two in-between frames) outside the plaza; the controller's
    // share applies to both, so it gives up the second one first.
    // RBREW_INTERP_FRAMES=1/2 forces the count (testing).
    static const int forced_frames = [] {
        const char* v = std::getenv("RBREW_INTERP_FRAMES");
        return v && (v[0] == '1' || v[0] == '2') ? v[0] - '0' : 0;
    }();
    const unsigned wanted = !s_vk_replay    ? 1u
                            : forced_frames ? unsigned(forced_frames)
                            : rbrew_settings::g_fps180.load(std::memory_order_relaxed) && !gx2vk::capture::plaza() ? 2u
                                                                                                                   : 1u;
    s_replay_count = wanted;
    if (replay && !always) {
        s_credit = std::min(double(wanted), s_credit + s_ratio * wanted);
        if (s_credit >= 1.0) {
            s_replay_count = unsigned(s_credit);
            s_credit -= s_replay_count;
        } else {
            replay = false;
            ++s_stats.skipped;
        }
    }
    if (!replay) {
        drop_works(s_works);
        if (capturing())
            rotate();
        else {
            s_cur->clear();
            s_last->clear();
            s_occurrences.clear();
            s_unit = UINT32_MAX;
            s_frame_broken = false;
        }
        return false;
    }
    gx2_render_interp_hold(scan_index);
    s_scan_index = scan_index;
    s_replay_works.swap(s_works);
    drop_works(s_works);
    s_pending_replay = true;
    return true;
}

void service() {
    if (s_pending_replay) {
        s_pending_replay = false;
        const uint64_t start = now_ns();
        s_replaying = true;
        s_occurrences.clear();
        bool ok = true;
        if (s_vk_replay) {
            // N's upload data must outlive the in-between frames' GPU work.
            gx2vk::retain_until_current_submission(s_frame_first_serial);
            s_stats.set_binds += unsigned(gx2vk::capture::replay_set_binds());
            // A camera cut or a very fast turn: many matrices jump and keep
            // frame N while others of the same camera blend; the in-between
            // frame then shows parts of two views (a whole quarter of the
            // screen in Octo Valley). It is discarded; frame N simply stays
            // on screen longer. RBREW_INTERP_CUT_GUARD=0: keep it.
            static const bool cut_guard = [] {
                const char* v = std::getenv("RBREW_INTERP_CUT_GUARD");
                return !(v && v[0] == '0');
            }();
            auto cut = [] {
                const unsigned all = s_blended_matrices + s_jumped_matrices;
                return all >= 20 && s_jumped_matrices * 4 > all;
            };
            s_blended_matrices = s_jumped_matrices = 0;
            for (unsigned i = 0; i < s_replay_count; ++i) {
                if (i == 1 && cut_guard && cut()) break;
                // 180 FPS: the second one is drawn now as well and kept,
                // before the next frame's commands touch the renderer.
                if (i == 1) {
                    s_replaying = false;
                    gx2_render_interp_present_scan(s_scan_index);
                    s_replaying = true;
                }
                s_blend_t = float(i + 1) / float(s_replay_count + 1);
                s_substituted.clear();
                s_by_content.clear();
                s_occurrences.clear();
                gx2vk::capture::replay(&substitute);
            }
            s_blend_t = 0.5f;
            if (cut_guard && cut()) {
                ok = false;
                ++s_stats.cuts;
            }
        } else
            try {
                // Publish finished occlusion queries meanwhile: the game reads the
                // previous frame's results early in its frame (gx2_draw.cpp).
                unsigned n = 0;
                for (Gx2AsyncWork* work : s_replay_works) {
                    work->run();
                    if ((++n & 31) == 0) gx2_render_query_poll_completed();
                }
            } catch (...) {
                ok = false;
                ++s_stats.failed;
            }
        s_replaying = false;
        s_replay_cur = s_replay_prev = nullptr;
        drop_works(s_replay_works);
        if (ok && s_replay_count == 2)
            gx2_render_interp_keep_mid(s_scan_index);
        else if (ok)
            gx2_render_interp_present_scan(s_scan_index);
        const double took = double(now_ns() - start);
        s_replay_avg_ns = s_replay_avg_ns ? s_replay_avg_ns * 0.8 + took * 0.2 : took;
        ++s_stats.replays;
        s_stats.replay_ms += took / 1e6;
        // The held frame N goes out half a frame interval after the
        // in-between frame (180 FPS: the second one after a third, N after
        // two thirds), so presents are evenly spaced.
        const uint64_t step = uint64_t(s_interval_ns / double(s_replay_count + 1));
        s_mid = ok && s_replay_count == 2;
        s_mid_due = now_ns() + step;
        s_held = true;
        s_held_due = now_ns() + step * (s_mid ? 2 : 1);
        rotate();
    }
    if (s_mid && now_ns() >= s_mid_due) present_mid_now();
    if (s_held && now_ns() >= s_held_due) present_held_now();
}

uint64_t deadline_ns() {
    return s_pending_replay ? 1 : s_mid ? s_mid_due : s_held ? s_held_due : 0;
}
} // namespace gx2interp

// Offline AOT shader extraction, R700->GLSL translation, and SPIR-V cache preparation.
// Runs without a Vulkan device or game entry point.
#include "../os/gx2/gx2_pm4_state.h"
#include "../os/gx2/gx2_shader_key.h"
#include "../os/gx2/r700_to_glsl.h"
#include "../os/gx2/vk/gx2_vk_shader.h"
#include "rebrewu/logger.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kTranslationMagic = 0x4C544252u; // "RBTL"
constexpr uint32_t kTranslationVersion = 2u;
constexpr uint32_t kTranslationPackMagic = 0x50544252u; // "RBTP"
constexpr uint32_t kTranslationPackVersion = 2u;
constexpr uint32_t kWarmMagic = 0x4D574252u; // "RBWM"
constexpr uint32_t kWarmVersion = 1u;
constexpr uint32_t kExactKeyTag = 0x45584B33u; // "EXK3"
constexpr uint32_t kCompatibility = 2u;
constexpr const char* kCachePrefix = "v2_c1_";

class ProgressReporter {
public:
    ProgressReporter(const char* label, const std::atomic<size_t>& current, size_t total,
                     std::chrono::milliseconds interval = std::chrono::seconds(2))
        : label_(label), current_(current), total_(total), interval_(interval),
          start_(std::chrono::steady_clock::now()), last_time_(start_) {
        if (total_ > 0) thread_ = std::thread([this] { run(); });
    }

    ~ProgressReporter() { stop(); }

    ProgressReporter(const ProgressReporter&) = delete;
    ProgressReporter& operator=(const ProgressReporter&) = delete;

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) return;
            stopped_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

private:
    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopped_) {
            if (cv_.wait_for(lock, interval_, [this] { return stopped_; })) break;
            lock.unlock();
            report(false);
            lock.lock();
        }
        lock.unlock();
        report(true);
    }

    void report(bool final_call) {
        const auto now = std::chrono::steady_clock::now();
        const size_t done = std::min(current_.load(std::memory_order_relaxed), total_);
        if (final_call && done == last_reported_) return;
        last_reported_ = done;
        const double elapsed = std::chrono::duration<double>(now - start_).count();
        const double dt = std::chrono::duration<double>(now - last_time_).count();
        if (dt > 0.05 && done > last_done_) {
            const double inst_rate = double(done - last_done_) / dt;
            // Reset smoothing if a fast disk-cache burst transitions into real work.
            if (smoothed_rate_ <= 0.0 || inst_rate < smoothed_rate_ * 0.25) {
                smoothed_rate_ = inst_rate;
            } else {
                smoothed_rate_ = 0.35 * inst_rate + 0.65 * smoothed_rate_;
            }
            last_done_ = done;
            last_time_ = now;
        }
        const double pct = total_ ? (100.0 * double(done) / double(total_)) : 100.0;
        if (!final_call && done < total_ && smoothed_rate_ > 0.0) {
            const double eta = double(total_ - done) / smoothed_rate_;
            RBREW_LOG_INFO("aot", "%s: %zu/%zu (%.1f%%) | elapsed %.0fs | ETA %.0fs\n", label_, done, total_, pct,
                           elapsed, eta);
        } else {
            RBREW_LOG_INFO("aot", "%s: %zu/%zu (%.1f%%) | elapsed %.1fs\n", label_, done, total_, pct, elapsed);
        }
    }

    const char* label_;
    const std::atomic<size_t>& current_;
    size_t total_;
    std::chrono::milliseconds interval_;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point last_time_;
    size_t last_done_ = 0;
    size_t last_reported_ = ~size_t(0);
    double smoothed_rate_ = 0.0;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopped_ = false;
    std::thread thread_;
};

constexpr unsigned kExactKeyRegisterCount = kGx2ExactKeyRegisterCount;

constexpr unsigned exact_word_index(uint32_t r) {
    return gx2_exact_word_index(r);
}

constexpr const auto& kExactKeyRegisters = kGx2ExactKeyRegisters;

// Must stay byte-for-byte identical to KeyHash in gx2_render_vk_draw.cpp.
struct KeyHash {
    uint64_t lane[4] = {0x60EA27EEADC0B5D6ull, 0xC2B2AE3D27D4EB4Full, 0x0ull, 0x61C8864E7A143579ull};
    static uint64_t rotl(uint64_t v, int r) { return (v << r) | (v >> (64 - r)); }
    static uint64_t round(uint64_t acc, uint64_t input) {
        acc += input * 0xC2B2AE3D27D4EB4Full;
        acc = rotl(acc, 31);
        return acc * 0x9E3779B185EBCA87ull;
    }
    void add(const void* data, size_t bytes) {
        const auto* p = static_cast<const uint8_t*>(data);
        lane[0] = round(lane[0], bytes);
        size_t i = 0;
        for (; i + 32 <= bytes; i += 32) {
            uint64_t w[4];
            std::memcpy(w, p + i, 32);
            lane[0] = round(lane[0], w[0]);
            lane[1] = round(lane[1], w[1]);
            lane[2] = round(lane[2], w[2]);
            lane[3] = round(lane[3], w[3]);
        }
        unsigned l = 1;
        for (; i + 8 <= bytes; i += 8, l = (l + 1) & 3) {
            uint64_t w;
            std::memcpy(&w, p + i, 8);
            lane[l] = round(lane[l], w);
        }
        if (i < bytes) {
            uint64_t w = 0;
            std::memcpy(&w, p + i, bytes - i);
            lane[l] = round(lane[l], w);
        }
    }
    uint64_t finish() const {
        uint64_t h = rotl(lane[0], 1) + rotl(lane[1], 7) + rotl(lane[2], 12) + rotl(lane[3], 18);
        h ^= h >> 33;
        h *= 0xC2B2AE3D27D4EB4Full;
        h ^= h >> 29;
        h *= 0x165667B19E3779F9ull;
        h ^= h >> 32;
        return h;
    }
};

struct ExactReads {
    uint32_t vs_parameter_mask = 0;
    uint32_t vs_texture_mask = 0;
    uint32_t ps_texture_mask = 0;
};

enum WarmTag : uint32_t { kWarmProgram = 1, kWarmMasks = 2 };
struct WarmProgramRecord {
    uint64_t key;
    uint32_t rect, points;
};
struct WarmMasksRecord {
    uint64_t base;
    ExactReads reads;
};
static_assert(std::is_trivially_copyable_v<WarmProgramRecord> && std::is_trivially_copyable_v<WarmMasksRecord>);

uint16_t read_u16_be(const uint8_t* p) {
    return uint16_t(uint16_t(p[0]) << 8 | p[1]);
}
uint16_t read_u16_le(const uint8_t* p) {
    return uint16_t(uint16_t(p[1]) << 8 | p[0]);
}
uint32_t read_u32_be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
int32_t read_i32_be(const uint8_t* p) {
    return static_cast<int32_t>(read_u32_be(p));
}
uint32_t read_u32_le(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

void write_u32_be(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

std::string read_cstr(std::span<const uint8_t> buf, size_t off) {
    if (off >= buf.size()) return {};
    size_t end = off;
    while (end < buf.size() && buf[end] != 0) ++end;
    return std::string(reinterpret_cast<const char*>(buf.data() + off), end - off);
}

size_t resolve_rel(std::span<const uint8_t> buf, size_t field_off) {
    if (field_off + 4 > buf.size()) return 0;
    const int32_t delta = read_i32_be(buf.data() + field_off);
    if (delta == 0) return 0;
    const int64_t target = int64_t(field_off) + int64_t(delta);
    if (target < 0 || size_t(target) >= buf.size()) return 0;
    return size_t(target);
}

bool read_file_bytes(const fs::path& path, std::vector<uint8_t>& out) {
    std::error_code ec;
    const auto sz = fs::file_size(path, ec);
    if (ec || sz > (16ull << 30) || sz > SIZE_MAX) return false;
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) return false;
    out.resize(size_t(sz));
    size_t pos = 0;
    while (pos < out.size()) {
        const size_t chunk = std::min<size_t>(out.size() - pos, 64u << 20);
        const size_t n = std::fread(out.data() + pos, 1, chunk, f);
        if (n == 0) break;
        pos += n;
    }
    std::fclose(f);
    return pos == out.size();
}

bool write_file_atomic(const fs::path& path, const void* data, size_t size) {
    const fs::path tmp = path.string() + ".tmp";
    std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
    if (!f) return false;
    bool ok = size == 0 || std::fwrite(data, 1, size, f) == size;
    ok = std::fclose(f) == 0 && ok;
    std::error_code ec;
    if (ok) fs::rename(tmp, path, ec);
    if (!ok || ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool decompress_yaz0(std::span<const uint8_t> src, std::vector<uint8_t>& dst) {
    if (src.size() < 16 || std::memcmp(src.data(), "Yaz0", 4) != 0) return false;
    const uint32_t uncomp_size = read_u32_be(src.data() + 4);
    if (uncomp_size > (512u << 20)) return false;
    dst.resize(uncomp_size);
    size_t sp = 16;
    size_t dp = 0;
    uint32_t valid_bits = 0;
    uint8_t code = 0;
    while (dp < uncomp_size && sp < src.size()) {
        if (valid_bits == 0) {
            code = src[sp++];
            valid_bits = 8;
        }
        if (code & 0x80u) {
            dst[dp++] = src[sp++];
        } else {
            if (sp + 2 > src.size()) break;
            const uint8_t b1 = src[sp++];
            const uint8_t b2 = src[sp++];
            const size_t dist = (size_t(b1 & 0x0Fu) << 8) | b2;
            if (dist + 1 > dp) return false;
            size_t copy_pos = dp - (dist + 1);
            size_t n = b1 >> 4;
            if (n == 0) {
                if (sp >= src.size()) break;
                n = size_t(src[sp++]) + 0x12u;
            } else {
                n += 2u;
            }
            for (size_t i = 0; i < n && dp < uncomp_size; ++i) dst[dp++] = dst[copy_pos++];
        }
        code <<= 1;
        --valid_bits;
    }
    return dp == uncomp_size;
}

struct SarcEntry {
    std::string name;
    std::span<const uint8_t> data;
};

bool parse_sarc(std::span<const uint8_t> buf, std::vector<SarcEntry>& entries) {
    entries.clear();
    if (buf.size() < 20 || std::memcmp(buf.data(), "SARC", 4) != 0) return false;
    const bool be = (buf[6] == 0xFE && buf[7] == 0xFF);
    auto u16 = [&](size_t off) { return be ? read_u16_be(buf.data() + off) : read_u16_le(buf.data() + off); };
    auto u32 = [&](size_t off) { return be ? read_u32_be(buf.data() + off) : read_u32_le(buf.data() + off); };
    const uint32_t data_off = u32(12);
    const size_t sfat_off = 0x14;
    if (sfat_off + 12 > buf.size() || std::memcmp(buf.data() + sfat_off, "SFAT", 4) != 0) return false;
    const uint16_t node_count = u16(sfat_off + 6);
    const size_t nodes_off = sfat_off + 12;
    const size_t sfnt_off = nodes_off + size_t(node_count) * 16;
    if (sfnt_off + 8 > buf.size() || std::memcmp(buf.data() + sfnt_off, "SFNT", 4) != 0) return false;
    entries.reserve(node_count);
    for (uint16_t i = 0; i < node_count; ++i) {
        const size_t n = nodes_off + size_t(i) * 16;
        const uint32_t flags = u32(n + 4);
        const uint32_t start = u32(n + 8);
        const uint32_t end = u32(n + 12);
        if (end < start || size_t(data_off) + end > buf.size()) continue;
        std::string name;
        if (flags & 0x01000000u) {
            const size_t name_off = sfnt_off + 8 + size_t(flags & 0xFFFFu) * 4;
            name = read_cstr(buf, name_off);
        } else {
            name = "node_" + std::to_string(i);
        }
        entries.push_back({std::move(name), buf.subspan(size_t(data_off) + start, end - start)});
    }
    return true;
}

uint64_t bytes_hash(const void* data, size_t bytes) {
    KeyHash h;
    h.add(data, bytes);
    return h.finish();
}

uint64_t translation_body_hash(const std::vector<uint8_t>& blob) {
    KeyHash h;
    h.add(blob.data(), std::min<size_t>(blob.size(), 8));
    if (blob.size() > 16) h.add(blob.data() + 16, blob.size() - 16);
    return h.finish();
}

std::vector<uint8_t> serialize_translation_blob(uint64_t key, const char* vs, const char* ps,
                                                const std::array<R700TranslationInfo, 2>& info) {
    std::vector<uint8_t> out;
    auto write = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    };
    auto write_u32 = [&](uint32_t v) { write(&v, 4); };
    auto write_str = [&](const char* s) {
        const uint32_t n = uint32_t(std::strlen(s));
        write_u32(n);
        write(s, n);
    };
    write_u32(kTranslationMagic);
    write_u32(kTranslationVersion);
    write(&key, 8);
    write_str(vs);
    write_str(ps);
    for (const auto& i : info) {
        write_u32(uint32_t(i.remapped.size()));
        write(i.remapped.data(), i.remapped.size() * sizeof(R700UniformEntry));
        write(i.attribute_locations.data(), sizeof(i.attribute_locations));
        write(i.buffer_bindings.data(), sizeof(i.buffer_bindings));
        write_u32(i.uniform_mode);
        write_u32(i.register_count);
        write(i.texture_compare.data(), sizeof(i.texture_compare));
        write_u32(i.streamout_mask);
        write(i.streamout_strides.data(), sizeof(i.streamout_strides));
        for (const auto& words : i.streamout_written_words) {
            write_u32(uint32_t(words.size()));
            write(words.data(), words.size());
        }
        write(&i.vk, sizeof(i.vk));
    }
    write_u32(kTranslationMagic);
    return out;
}

bool parse_translation_blob(std::span<const uint8_t> bytes, uint64_t expected_key, bool preloaded, std::string& out_vs,
                            std::string& out_ps, std::array<R700TranslationInfo, 2>& out_info) {
    size_t pos = 0;
    bool ok = true;
    auto read = [&](void* p, size_t n) {
        if (!ok || !n) return;
        if (pos + n > bytes.size()) {
            ok = false;
            return;
        }
        std::memcpy(p, bytes.data() + pos, n);
        pos += n;
    };
    auto read_u32 = [&] {
        uint32_t v = 0;
        read(&v, 4);
        return v;
    };
    auto read_str = [&](std::string& dst) {
        const uint32_t n = read_u32();
        if (!ok || n > (64u << 20)) {
            ok = false;
            return;
        }
        dst.resize(n);
        read(dst.data(), n);
    };
    uint64_t stored_key = 0;
    if (read_u32() != kTranslationMagic || read_u32() != kTranslationVersion) ok = false;
    read(&stored_key, 8);
    if (stored_key != (preloaded ? 0ull : expected_key)) ok = false;
    if (ok) read_str(out_vs);
    if (ok) read_str(out_ps);
    for (auto& i : out_info) {
        if (!ok) break;
        const uint32_t remapped = read_u32();
        if (remapped > 65536) {
            ok = false;
            break;
        }
        i.remapped.resize(remapped);
        read(i.remapped.data(), remapped * sizeof(R700UniformEntry));
        read(i.attribute_locations.data(), sizeof(i.attribute_locations));
        read(i.buffer_bindings.data(), sizeof(i.buffer_bindings));
        i.uniform_mode = read_u32();
        i.register_count = read_u32();
        read(i.texture_compare.data(), sizeof(i.texture_compare));
        i.streamout_mask = read_u32();
        read(i.streamout_strides.data(), sizeof(i.streamout_strides));
        for (auto& words : i.streamout_written_words) {
            const uint32_t n = read_u32();
            if (n > (1u << 20)) {
                ok = false;
                break;
            }
            words.resize(n);
            read(words.data(), n);
        }
        read(&i.vk, sizeof(i.vk));
    }
    if (read_u32() != kTranslationMagic) ok = false;
    return ok;
}

struct ProgramKeys {
    uint64_t exact_key = 0;
    uint64_t exact_base = 0;
    uint64_t canon_key = 0;
    ExactReads reads{};
};

ProgramKeys compute_program_keys(std::span<const uint8_t> vs_bc, std::span<const uint8_t> ps_bc,
                                 const std::array<uint32_t, 0x10000>& regs,
                                 const std::vector<R700FetchAttribute>& attributes, uint32_t num_attribs, uint32_t rect,
                                 uint32_t points) {
    ProgramKeys out;
    std::array<uint32_t, kExactKeyRegisterCount> words{};
    for (unsigned i = 0; i < kExactKeyRegisterCount; ++i) words[i] = regs[kExactKeyRegisters[i]];
    const uint32_t ps_inputs = std::min<uint32_t>(regs[0xA1B3] & 0x3Fu, 32u);
    constexpr unsigned kPsInputWord = exact_word_index(0xA191);
    std::fill(words.begin() + kPsInputWord + ps_inputs, words.begin() + kPsInputWord + 32, 0u);

    std::array<uint32_t, 36> texture_bits{};
    unsigned t = 0;
    for (unsigned base : kGx2TranslatorTextureBases)
        for (unsigned slot = 0; slot < 18; ++slot)
            texture_bits[t++] = (regs[base + slot * 7] & 7u) | (((regs[base + slot * 7 + 4] >> 8) & 3u) << 3);

    KeyHash fetch_hash_builder;
    for (const auto& a : attributes) {
        const uint32_t data[] = {a.semantic,
                                 a.buffer,
                                 a.offset,
                                 a.format,
                                 a.divisor,
                                 a.endian,
                                 a.destination_select,
                                 unsigned(a.per_instance)};
        fetch_hash_builder.add(data, sizeof(data));
    }
    const uint64_t fetch_hash = fetch_hash_builder.finish();

    KeyHash h;
    h.add(&kExactKeyTag, sizeof(kExactKeyTag));
    h.add(vs_bc.data(), vs_bc.size());
    h.add(ps_bc.data(), ps_bc.size());
    h.add(&rect, sizeof(rect));
    h.add(&points, sizeof(points));
    const KeyHash canon_base = h;
    h.add(&num_attribs, sizeof(num_attribs));

    KeyHash base = h;
    for (unsigned i = 0; i < kExactKeyRegisterCount; ++i)
        if (i < exact_word_index(0xA185) || i > exact_word_index(0xA18E)) base.add(&words[i], 4);
    base.add(&fetch_hash, sizeof(fetch_hash));
    out.exact_base = base.finish();

    const auto vs_scan = r700_scan_shader_reads(vs_bc.data(), uint32_t(vs_bc.size()), false);
    const auto ps_scan = r700_scan_shader_reads(ps_bc.data(), uint32_t(ps_bc.size()), true);
    uint32_t known = 0;
    if (vs_scan.valid && ps_scan.valid) {
        known = 1;
        out.reads = ExactReads{vs_scan.vs_parameter_mask, vs_scan.texture_unit_mask, ps_scan.texture_unit_mask};
        for (unsigned k = 0; k < 10; ++k)
            if (k >= 8 || ((out.reads.vs_parameter_mask >> (k * 4)) & 0xFu) == 0)
                words[exact_word_index(0xA185 + k)] = 0;
        for (unsigned slot = 0; slot < 18; ++slot) {
            if (!((out.reads.ps_texture_mask >> slot) & 1u)) texture_bits[slot] = 0;
            if (!((out.reads.vs_texture_mask >> slot) & 1u)) texture_bits[18 + slot] = 0;
        }
    }

    h.add(&out.exact_base, sizeof(out.exact_base));
    h.add(&known, sizeof(known));
    h.add(words.data() + exact_word_index(0xA185), 10 * 4);
    h.add(texture_bits.data(), sizeof(texture_bits));
    out.exact_key = h.finish();

    KeyHash c = canon_base;
    gx2_hash_canonical_state(c, words.data(), texture_bits.data(), &attributes,
                             [](uint32_t fmt) { return r700_native_attribute(fmt).default_endian; });
    out.canon_key = c.finish();
    return out;
}

void init_default_texture_registers(std::array<uint32_t, 0x10000>& regs) {
    // Latte E_DIM::DIM_2D == 1: keep unbound/default units valid 2D textures instead of DIM_1D (0).
    for (uint32_t unit = 0; unit < 18; ++unit) {
        regs[0xE000 + unit * 7] = (regs[0xE000 + unit * 7] & ~7u) | 1u;
        regs[0xE460 + unit * 7] = (regs[0xE460 + unit * 7] & ~7u) | 1u;
    }
}

// Map GX2SamplerVarType into Latte SQ_TEX_RESOURCE WORD0 DIM and WORD4 NUM_FORMAT_ALL.
void set_sampler_registers(std::array<uint32_t, 0x10000>& regs, uint32_t stage_base, uint32_t unit,
                           uint32_t sampler_var_type) {
    if (unit >= 18) return;
    // Latte::E_DIM: 0=1D, 1=2D, 2=3D, 3=CUBEMAP, 4=1D_ARRAY, 5=2D_ARRAY, 6=2D_MSAA
    uint32_t dim = 1; // default DIM_2D
    switch (sampler_var_type) {
    case 0: // SAMPLER_1D
    case 5: // SAMPLER_1D_SHADOW
        dim = 0;
        break;
    case 2:  // SAMPLER_3D
    case 3:  // GX2_SAMPLER_VAR_TYPE_3D (cTexture3d_0, cTexCorrectionMap, cLightTexture0)
    case 37: // 3D slice sampler in agl_common.sharcfb
        dim = 2;
        break;
    case 4:  // GX2_SAMPLER_VAR_TYPE_CUBE
    case 13: // SAMPLER_CUBE_ARRAY (gsys_cube_map, cEnvCubeMap)
        dim = 3;
        break;
    case 10: // SAMPLER_2D_ARRAY (gsys_light_prepass, sTexHeadArray)
    case 12: // SAMPLER_2D_ARRAY_SHADOW (gsys_depth_shadow_cascade)
        dim = 5;
        break;
    default: // 1 (2D), 6 (2D_SHADOW), 17 (2D LD/RESINFO), 32 (2D)
        dim = 1;
        break;
    }
    const uint32_t base = stage_base + unit * 7;
    regs[base] = (regs[base] & ~7u) | (dim & 7u);
    regs[base + 4] &= ~(3u << 8); // float/normalized sampler
}

uint32_t default_dest_select_for_format(uint32_t fmt) {
    switch (fmt & 0x3Fu) {
    case 0:
    case 2:
    case 3:
    case 5:
    case 6: return 0x00040405u; // (x, 0, 0, 1)
    case 1:
    case 4:
    case 7:
    case 8:
    case 12:
    case 13: return 0x00010405u; // (x, y, 0, 1)
    case 9:
    case 16:
    case 17: return 0x00010205u; // (x, y, z, 1)
    default: return 0x00010203u; // (x, y, z, w)
    }
}

bool var_type_to_fetch(uint32_t vtype, uint32_t semantic, R700FetchAttribute& out) {
    out = {};
    out.semantic = semantic;
    out.buffer = 0;
    out.offset = 0;
    switch (vtype) {
    case 4: // FLOAT
        out.format = 0x806u;
        out.destination_select = 0x00040405u;
        break;
    case 9: // FLOAT2
        out.format = 0x80Du;
        out.destination_select = 0x00010405u;
        break;
    case 10: // FLOAT3
        out.format = 0x811u;
        out.destination_select = 0x00010205u;
        break;
    case 11: // FLOAT4
        out.format = 0x813u;
        out.destination_select = 0x00010203u;
        break;
    case 3: // UINT
        out.format = 0x105u;
        out.destination_select = 0x00040405u;
        break;
    case 15: // UINT2
        out.format = 0x10Cu;
        out.destination_select = 0x00010405u;
        break;
    case 16: // UINT3
        out.format = 0x110u;
        out.destination_select = 0x00010205u;
        break;
    case 17: // UINT4
        out.format = 0x112u;
        out.destination_select = 0x00010203u;
        break;
    default:
        out.format = 0x813u;
        out.destination_select = 0x00010203u;
        break;
    }
    out.endian = r700_native_attribute(out.format).default_endian;
    return true;
}

class AotCollector {
public:
    struct Stats {
        size_t fsha_models = 0;
        size_t fsha_programs = 0;
        size_t bahs_pairs = 0;
        size_t gfx2_pairs = 0;
        size_t migrated_v2 = 0;
        size_t translated_programs = 0;
        size_t translation_failures = 0;
        size_t vs_failures = 0;
        size_t ps_failures = 0;
        size_t fsha_failures = 0;
        size_t bahs_failures = 0;
        size_t gfx2_failures = 0;
        size_t unique_spirv_stages = 0;
        size_t spirv_failures = 0;
    };

    struct PretranslatedVs {
        bool valid = false;
        std::string glsl;
        R700TranslationInfo info{};
    };

    bool submit_program(std::span<const uint8_t> vs_bc, std::span<const uint8_t> ps_bc,
                        const std::array<uint32_t, 0x10000>& regs, const std::vector<R700FetchAttribute>& attributes,
                        uint32_t num_attribs, uint32_t rect, uint32_t points, const char* origin = "unknown",
                        std::string_view label = {}, PretranslatedVs* cached_vs = nullptr) {
        (void)label;
        if (vs_bc.empty() || ps_bc.empty() || (vs_bc.size() & 7) || (ps_bc.size() & 7)) return false;
        const ProgramKeys keys = compute_program_keys(vs_bc, ps_bc, regs, attributes, num_attribs, rect, points);
        if (keys.reads.vs_parameter_mask || keys.reads.vs_texture_mask || keys.reads.ps_texture_mask) {
            masks_.try_emplace(keys.exact_base, keys.reads);
        }

        if (canon_to_body_.find(keys.canon_key) != canon_to_body_.end()) {
            return true;
        }

        std::unique_ptr<std::array<uint32_t, 0x10000>> point_regs;
        const uint32_t* active_regs = regs.data();
        if (points != 0u && regs[0x2256] != 1u) {
            point_regs = std::make_unique<std::array<uint32_t, 0x10000>>(regs);
            (*point_regs)[0x2256] = 1u;
            active_regs = point_regs->data();
        }

        std::array<R700TranslationInfo, 2> info{};
        char* owned_vs = nullptr;
        const char* vs_glsl = nullptr;
        if (cached_vs && cached_vs->valid) {
            vs_glsl = cached_vs->glsl.c_str();
            info[0] = cached_vs->info;
        } else {
            R700TranslateInput vi{};
            vi.data = vs_bc.data();
            vi.size = uint32_t(vs_bc.size());
            vi.is_pixel = false;
            vi.num_attribs = num_attribs;
            vi.context_registers = active_regs;
            vi.attributes = attributes;
            vi.info = &info[0];
            vi.vulkan = true;
            vi.vulkan_negative_one_to_one = true;
            owned_vs = r700_to_glsl(vi);
            vs_glsl = owned_vs;
            if (owned_vs && cached_vs) {
                cached_vs->valid = true;
                cached_vs->glsl = owned_vs;
                cached_vs->info = info[0];
            }
        }

        char* ps_glsl = nullptr;
        if (vs_glsl) {
            R700TranslateInput pi{};
            pi.data = ps_bc.data();
            pi.size = uint32_t(ps_bc.size());
            pi.is_pixel = true;
            pi.context_registers = active_regs;
            pi.info = &info[1];
            pi.vulkan = true;
            ps_glsl = r700_to_glsl(pi);
        }

        if (!vs_glsl || !ps_glsl) {
            if (!vs_glsl)
                ++stats_.vs_failures;
            else
                ++stats_.ps_failures;
            if (std::string_view(origin) == "FSHA")
                ++stats_.fsha_failures;
            else if (std::string_view(origin) == "BAHS")
                ++stats_.bahs_failures;
            else if (std::string_view(origin) == "Gfx2")
                ++stats_.gfx2_failures;
            if (stats_.translation_failures < 16) {
                RBREW_LOG_ERROR("aot",
                                "Translation failed (%s %.*s): stage=%s vs_sz=%zu ps_sz=%zu attribs=%u rect=%u\n",
                                origin, int(label.size()), label.data(), !vs_glsl ? "VS" : "PS", vs_bc.size(),
                                ps_bc.size(), num_attribs, rect);
            }
            std::free(owned_vs);
            std::free(ps_glsl);
            ++stats_.translation_failures;
            return false;
        }

        ++stats_.translated_programs;
        add_spirv_stage(VK_SHADER_STAGE_VERTEX_BIT, vs_glsl);
        add_spirv_stage(VK_SHADER_STAGE_FRAGMENT_BIT, ps_glsl);
        if (rect) add_spirv_stage(VK_SHADER_STAGE_GEOMETRY_BIT, r700_rect_geometry(vs_glsl, rect == 2));

        std::vector<uint8_t> blob = serialize_translation_blob(0, vs_glsl, ps_glsl, info);
        std::free(owned_vs);
        std::free(ps_glsl);

        const uint32_t body_id = intern_body(std::move(blob));
        canon_to_body_.emplace(keys.canon_key, body_id);
        key_to_body_.try_emplace(keys.canon_key, body_id);

        const uint64_t warm_dedup = bytes_hash(&keys.canon_key, 8) ^ (uint64_t(rect) << 1) ^ points;
        if (warm_dedup_.insert(warm_dedup).second) {
            warm_programs_.push_back(WarmProgramRecord{keys.canon_key, rect, points});
        }
        return true;
    }

    void import_existing_v2_blob(uint64_t key, std::span<const uint8_t> blob, uint32_t rect, uint32_t points,
                                 bool record_warm) {
        std::string vs, ps;
        std::array<R700TranslationInfo, 2> info{};
        const bool is_preloaded = blob.size() >= 16 && std::memcmp(blob.data() + 8, "\0\0\0\0\0\0\0\0", 8) == 0;
        if (!parse_translation_blob(blob, key, is_preloaded, vs, ps, info)) return;
        add_spirv_stage(VK_SHADER_STAGE_VERTEX_BIT, vs);
        add_spirv_stage(VK_SHADER_STAGE_FRAGMENT_BIT, ps);
        if (rect) add_spirv_stage(VK_SHADER_STAGE_GEOMETRY_BIT, r700_rect_geometry(vs.c_str(), rect == 2));

        std::vector<uint8_t> zeroed(blob.begin(), blob.end());
        if (zeroed.size() >= 16) std::memset(zeroed.data() + 8, 0, 8);
        const uint32_t body_id = intern_body(std::move(zeroed));
        if (key_to_body_.try_emplace(key, body_id).second) ++stats_.migrated_v2;
        if (record_warm) {
            const uint64_t warm_dedup = key ^ (uint64_t(rect) << 1) ^ points;
            if (warm_dedup_.insert(warm_dedup).second) warm_programs_.push_back(WarmProgramRecord{key, rect, points});
        }
    }

    void import_v1_glsl_for_spirv(std::span<const uint8_t> blob) {
        std::string vs, ps;
        std::array<R700TranslationInfo, 2> info{};
        const bool is_preloaded = blob.size() >= 16 && std::memcmp(blob.data() + 8, "\0\0\0\0\0\0\0\0", 8) == 0;
        if (!parse_translation_blob(blob, 0, is_preloaded, vs, ps, info)) return;
        add_spirv_stage(VK_SHADER_STAGE_VERTEX_BIT, vs);
        add_spirv_stage(VK_SHADER_STAGE_FRAGMENT_BIT, ps);
    }

    void add_mask(uint64_t base, const ExactReads& reads) { masks_.try_emplace(base, reads); }

    bool compile_all_spirv() {
        stats_.unique_spirv_stages = spirv_stages_.size();
        const uint64_t initial_hits = gx2vk::shader_disk_hits();
        const uint64_t initial_compiles = gx2vk::shader_compile_count();
        std::atomic<size_t> next{0};
        std::atomic<size_t> done{0};
        std::atomic<size_t> compiled{0};
        std::atomic<size_t> failed{0};
        const unsigned workers = std::clamp(std::thread::hardware_concurrency(), 2u, 16u);
        std::vector<std::thread> pool;
        pool.reserve(workers);
        {
            ProgressReporter progress("Compiling SPIR-V", done, spirv_stages_.size());
            for (unsigned w = 0; w < workers; ++w) {
                pool.emplace_back([&] {
                    for (size_t idx; (idx = next.fetch_add(1, std::memory_order_relaxed)) < spirv_stages_.size();) {
                        const auto& item = spirv_stages_[idx];
                        std::vector<uint32_t> spirv;
                        std::string log;
                        if (gx2vk::compile_glsl(item.first, item.second, spirv, &log)) {
                            compiled.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            const size_t n = failed.fetch_add(1, std::memory_order_relaxed);
                            if (n < 16)
                                RBREW_LOG_ERROR("aot", "SPIR-V compile failed (stage %d): %s\n", int(item.first),
                                                log.c_str());
                        }
                        done.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }
            for (auto& t : pool) t.join();
            progress.stop();
        }
        stats_.spirv_failures = failed.load();
        RBREW_LOG_INFO("aot", "SPIR-V summary: compiled=%llu cached=%llu failed=%zu\n",
                       (unsigned long long)(gx2vk::shader_compile_count() - initial_compiles),
                       (unsigned long long)(gx2vk::shader_disk_hits() - initial_hits), stats_.spirv_failures);
        return stats_.spirv_failures == 0;
    }

    bool write_outputs(const fs::path& translated_dir) const {
        std::error_code ec;
        fs::create_directories(translated_dir, ec);
        if (ec) return false;

        // 1. Write individual .tl files so run_translation_preload accepts every key in pack.bin.
        std::vector<std::pair<uint64_t, uint32_t>> sorted(key_to_body_.begin(), key_to_body_.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

        std::atomic<size_t> next{0};
        std::atomic<size_t> written{0};
        std::atomic<bool> tl_ok{true};
        const unsigned workers = std::clamp(std::thread::hardware_concurrency(), 2u, 8u);
        std::vector<std::thread> pool;
        {
            ProgressReporter progress("Writing .tl cache", written, sorted.size());
            for (unsigned w = 0; w < workers; ++w) {
                pool.emplace_back([&] {
                    std::vector<uint8_t> blob;
                    char name[64];
                    for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < sorted.size();) {
                        const uint64_t key = sorted[i].first;
                        const auto& body = bodies_[sorted[i].second];
                        std::snprintf(name, sizeof(name), "%s%016llx.tl", kCachePrefix, (unsigned long long)key);
                        const fs::path p = translated_dir / name;
                        std::error_code local_ec;
                        if (fs::exists(p, local_ec) && !local_ec) {
                            const auto existing_sz = fs::file_size(p, local_ec);
                            if (!local_ec && existing_sz == body.size()) {
                                written.fetch_add(1, std::memory_order_relaxed);
                                continue;
                            }
                        }
                        blob = body;
                        if (blob.size() >= 16) std::memcpy(blob.data() + 8, &key, 8);
                        if (!write_file_atomic(p, blob.data(), blob.size())) tl_ok.store(false);
                        written.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }
            for (auto& t : pool) t.join();
            progress.stop();
        }
        if (!tl_ok.load()) return false;

        // 2. Write RBTP v2 pack.bin.
        RBREW_LOG_INFO("aot", "Writing %spack.bin (%zu keys, %zu bodies)...\n", kCachePrefix, sorted.size(),
                       bodies_.size());
        const fs::path pack_path = translated_dir / (std::string(kCachePrefix) + "pack.bin");
        const fs::path pack_tmp = pack_path.string() + ".tmp";
        {
            std::FILE* f = std::fopen(pack_tmp.string().c_str(), "wb");
            if (!f) return false;
            std::setvbuf(f, nullptr, _IOFBF, 8u << 20);
            const uint32_t hdr[4] = {kTranslationPackMagic, kTranslationPackVersion, uint32_t(sorted.size()),
                                     uint32_t(bodies_.size())};
            bool ok = std::fwrite(hdr, 4, 4, f) == 4;
            for (const auto& [key, body_id] : sorted)
                ok = ok && std::fwrite(&key, 8, 1, f) == 1 && std::fwrite(&body_id, 4, 1, f) == 1;
            for (const auto& body : bodies_) {
                const uint32_t sz = uint32_t(body.size());
                ok = ok && std::fwrite(&sz, 4, 1, f) == 1;
            }
            for (const auto& body : bodies_) ok = ok && std::fwrite(body.data(), 1, body.size(), f) == body.size();
            ok = std::fclose(f) == 0 && ok;
            if (ok) fs::rename(pack_tmp, pack_path, ec);
            if (!ok || ec) {
                fs::remove(pack_tmp, ec);
                return false;
            }
        }

        // 3. Write RBWM v1 warm.bin.
        RBREW_LOG_INFO("aot", "Writing %swarm.bin (%zu masks, %zu programs)...\n", kCachePrefix, masks_.size(),
                       warm_programs_.size());
        const fs::path warm_path = translated_dir / (std::string(kCachePrefix) + "warm.bin");
        const fs::path warm_tmp = warm_path.string() + ".tmp";
        {
            std::FILE* f = std::fopen(warm_tmp.string().c_str(), "wb");
            if (!f) return false;
            const uint32_t hdr[2] = {kWarmMagic, kWarmVersion};
            bool ok = std::fwrite(hdr, 4, 2, f) == 2;
            auto write_rec = [&](uint32_t tag, const void* data, uint32_t bytes) {
                ok = ok && std::fwrite(&tag, 4, 1, f) == 1 && std::fwrite(&bytes, 4, 1, f) == 1 &&
                     std::fwrite(data, 1, bytes, f) == bytes;
            };
            for (const auto& [base, reads] : masks_) {
                WarmMasksRecord m{};
                m.base = base;
                m.reads = reads;
                write_rec(kWarmMasks, &m, sizeof(m));
            }
            for (const auto& wp : warm_programs_) write_rec(kWarmProgram, &wp, sizeof(wp));
            ok = std::fclose(f) == 0 && ok;
            if (ok) fs::rename(warm_tmp, warm_path, ec);
            if (!ok || ec) {
                fs::remove(warm_tmp, ec);
                return false;
            }
        }
        return true;
    }

    Stats stats_;
    std::vector<std::vector<uint8_t>> bodies_;
    std::unordered_map<uint64_t, uint32_t> key_to_body_;

private:
    uint32_t intern_body(std::vector<uint8_t>&& blob) {
        const uint64_t hash = translation_body_hash(blob);
        auto& bucket = body_intern_[hash];
        for (uint32_t idx : bucket)
            if (bodies_[idx] == blob) return idx;
        const uint32_t idx = uint32_t(bodies_.size());
        bodies_.push_back(std::move(blob));
        bucket.push_back(idx);
        return idx;
    }

    void add_spirv_stage(VkShaderStageFlagBits stage, const std::string& glsl) {
        const uint64_t h = bytes_hash(glsl.data(), glsl.size()) ^ (uint64_t(stage) << 32);
        auto& bucket = spirv_seen_[h];
        for (size_t idx : bucket)
            if (spirv_stages_[idx].first == stage && spirv_stages_[idx].second == glsl) return;
        const size_t idx = spirv_stages_.size();
        spirv_stages_.emplace_back(stage, glsl);
        bucket.push_back(idx);
    }

    std::unordered_map<uint64_t, std::vector<uint32_t>> body_intern_;
    std::unordered_map<uint64_t, uint32_t> canon_to_body_;
    std::unordered_map<uint64_t, ExactReads> masks_;
    std::vector<WarmProgramRecord> warm_programs_;
    std::unordered_set<uint64_t> warm_dedup_;
    std::vector<std::pair<VkShaderStageFlagBits, std::string>> spirv_stages_;
    std::unordered_map<uint64_t, std::vector<size_t>> spirv_seen_;
};

struct FshaAttrDef {
    std::string name;
    uint8_t semantic = 0xFFu;
    uint8_t var_type = 11u;
    bool valid = false;
};

struct FvtxLayout {
    std::vector<R700FetchAttribute> attrs;
    bool synthesized_uv = false;
};

std::vector<FvtxLayout> extract_fvtx_layouts(std::span<const uint8_t> fres, const std::vector<size_t>& fvtx_offsets,
                                             const std::vector<uint32_t>& secondary_uv_formats,
                                             const std::vector<FshaAttrDef>& attr_defs, uint32_t attrib_mask,
                                             const std::array<bool, 256>& vs_uses) {
    std::vector<FvtxLayout> result;
    std::set<std::vector<uint32_t>> seen;

    for (size_t pos : fvtx_offsets) {
        const uint8_t num_attr = fres[pos + 4];
        const size_t arr = resolve_rel(fres, pos + 0x10);
        if (!num_attr || num_attr > 32 || !arr || arr + size_t(num_attr) * 12 > fres.size()) continue;

        struct FvtxAttr {
            std::string name;
            uint8_t buffer = 0;
            uint8_t divisor = 0;
            uint16_t offset = 0;
            uint32_t format = 0;
        };
        std::vector<FvtxAttr> fvtx_attrs;
        fvtx_attrs.reserve(num_attr);
        uint32_t u0_format = 0;
        for (uint8_t i = 0; i < num_attr; ++i) {
            const size_t entry = arr + size_t(i) * 12;
            const size_t name_off = resolve_rel(fres, entry);
            if (!name_off) continue;
            const uint8_t buf = fres[entry + 4];
            const uint8_t div = fres[entry + 5];
            const uint16_t offset = read_u16_be(fres.data() + entry + 6);
            const uint32_t fmt = read_u32_be(fres.data() + entry + 8);
            if ((fmt & 0x3Fu) >= 20u || buf >= 16u) continue;
            std::string attr_name = read_cstr(fres, name_off);
            if (attr_name == "_u0") u0_format = fmt;
            fvtx_attrs.push_back({std::move(attr_name), buf, div, offset, fmt});
        }

        const uint32_t primary_uv_fmt = u0_format ? u0_format : 0x7u;
        auto append_layout = [&](uint32_t missing_uv_fmt, uint32_t missing_u3_fmt,
                                 bool override_u2_with_u0 = false) -> bool {
            // Match nw::g3d attribute order: iterate FSHA aarr 0..num_attribs-1, gated by pp+0x04 attrib_mask.
            std::vector<R700FetchAttribute> attrs;
            std::vector<uint32_t> sig;
            bool synthesized_uv = false;
            bool synthesized_u23 = false;
            bool overrode_u2 = false;
            bool synthesized_u3 = false;
            for (size_t a = 0; a < attr_defs.size(); ++a) {
                if (!attr_defs[a].valid || (a < 32 && ((attrib_mask >> a) & 1u) == 0)) continue;
                const FvtxAttr* found = nullptr;
                for (const auto& candidate : fvtx_attrs) {
                    if (candidate.name == attr_defs[a].name) {
                        found = &candidate;
                        break;
                    }
                }
                R700FetchAttribute attr{};
                if (found) {
                    const uint32_t fmt = (override_u2_with_u0 && attr_defs[a].name == "_u2" && u0_format != 0u &&
                                          found->format != u0_format)
                                             ? u0_format
                                             : found->format;
                    if (fmt != found->format) overrode_u2 = true;
                    attr.semantic = attr_defs[a].semantic;
                    attr.buffer = found->buffer;
                    attr.offset = found->offset;
                    attr.format = fmt;
                    attr.divisor = found->divisor;
                    attr.endian = r700_native_attribute(fmt).default_endian;
                    attr.destination_select = default_dest_select_for_format(fmt);
                } else if (missing_uv_fmt != 0u && vs_uses[attr_defs[a].semantic] &&
                           attr_defs[a].name.starts_with("_u") && attr_defs[a].name != "_u0") {
                    // Models lacking secondary UV streams (e.g. _u2, _u3) bind a default 2D format at draw time;
                    // _u1 always matches _u0, while _u2/_u3 may also use 0x207/0x808.
                    const bool is_u1 = attr_defs[a].name == "_u1";
                    const uint32_t fmt =
                        is_u1
                            ? primary_uv_fmt
                            : ((attr_defs[a].name == "_u3" && missing_u3_fmt != 0u) ? missing_u3_fmt : missing_uv_fmt);
                    if (attr_defs[a].name == "_u3") synthesized_u3 = true;
                    if (!is_u1) synthesized_u23 = true;
                    attr.semantic = attr_defs[a].semantic;
                    attr.format = fmt;
                    attr.endian = r700_native_attribute(fmt).default_endian;
                    attr.destination_select = default_dest_select_for_format(fmt);
                    synthesized_uv = true;
                } else {
                    continue;
                }
                attrs.push_back(attr);
                sig.push_back(attr.semantic);
                sig.push_back(attr.format);
                sig.push_back(attr.endian);
                sig.push_back(attr.destination_select);
                sig.push_back(attr.buffer >= 16 ? 1u : 0u);
            }
            if (override_u2_with_u0 && (!overrode_u2 || !synthesized_u3)) return false;
            if (!attrs.empty() && seen.insert(sig).second) result.push_back({std::move(attrs), synthesized_uv});
            return synthesized_u23;
        };

        // Always keep the un-synthesized FVTX layout (also enables alpha=0xE cutout variants).
        append_layout(0u, 0u);
        if (append_layout(primary_uv_fmt, primary_uv_fmt)) {
            for (uint32_t fallback_fmt : secondary_uv_formats) {
                if (fallback_fmt != primary_uv_fmt) append_layout(fallback_fmt, fallback_fmt);
            }
            if (u0_format != 0u) append_layout(u0_format, 0x808u, true);
        }
    }
    return result;
}

void extract_fres_shaders(std::span<const uint8_t> data, AotCollector& collector) {
    std::vector<size_t> fvtx_offsets;
    std::vector<uint32_t> secondary_uv_formats;
    std::set<uint32_t> seen_uv_fmts;
    for (size_t fpos = 0; fpos + 28 <= data.size(); fpos += 4) {
        if (std::memcmp(data.data() + fpos, "FVTX", 4) != 0) continue;
        fvtx_offsets.push_back(fpos);
        const uint8_t num_attr = data[fpos + 4];
        const size_t arr = resolve_rel(data, fpos + 0x10);
        if (!num_attr || num_attr > 32 || !arr || arr + size_t(num_attr) * 12 > data.size()) continue;
        for (uint8_t i = 0; i < num_attr; ++i) {
            const size_t entry = arr + size_t(i) * 12;
            const size_t name_off = resolve_rel(data, entry);
            const uint32_t fmt = read_u32_be(data.data() + entry + 8);
            if (!name_off || (fmt & 0x3Fu) >= 20u) continue;
            const std::string attr_name = read_cstr(data, name_off);
            if (attr_name.starts_with("_u") && attr_name != "_u0" && (fmt == 0x7u || fmt == 0x207u || fmt == 0x808u) &&
                seen_uv_fmts.insert(fmt).second) {
                secondary_uv_formats.push_back(fmt);
            }
        }
    }
    if (secondary_uv_formats.empty()) secondary_uv_formats.push_back(0x7u);

    size_t pos = 0;
    while (pos + 8 <= data.size()) {
        if (std::memcmp(data.data() + pos, "FSHA\x03\x05\x00\x04", 8) != 0) {
            ++pos;
            continue;
        }
        const size_t md_off = resolve_rel(data, pos + 0x2C);
        if (md_off && md_off + 8 <= data.size()) {
            const uint32_t model_count = read_u32_be(data.data() + md_off + 4);
            for (uint32_t m = 0; m < model_count; ++m) {
                const size_t epos = md_off + 24 + size_t(m) * 16;
                if (epos + 16 > data.size()) break;
                const size_t mpos = resolve_rel(data, epos + 12);
                if (!mpos || mpos + 0x60 > data.size()) continue;
                ++collector.stats_.fsha_models;

                const uint8_t num_attribs = data[mpos + 0x08];
                const uint8_t num_samplers = data[mpos + 0x09];
                const uint16_t num_progs = read_u16_be(data.data() + mpos + 0x06);

                std::vector<FshaAttrDef> attr_defs(num_attribs);
                const size_t aarr = resolve_rel(data, mpos + 0x3C);
                const size_t adict = resolve_rel(data, mpos + 0x40);
                if (aarr && adict && num_attribs > 0) {
                    for (uint8_t a = 0; a < num_attribs; ++a) {
                        const size_t de = adict + 24 + size_t(a) * 16;
                        if (de + 16 > data.size() || aarr + size_t(a) * 4 + 4 > data.size()) break;
                        const size_t n_off = resolve_rel(data, de + 8);
                        if (!n_off) continue;
                        const size_t ptr = resolve_rel(data, de + 12);
                        const size_t idx =
                            (ptr >= aarr && ptr < aarr + size_t(num_attribs) * 4 && ((ptr - aarr) % 4) == 0)
                                ? (ptr - aarr) / 4
                                : size_t(a);
                        if (idx < num_attribs) {
                            attr_defs[idx] = {read_cstr(data, n_off), data[aarr + idx * 4 + 3],
                                              data[aarr + idx * 4 + 1], true};
                        }
                    }
                }

                std::vector<uint8_t> sampler_types(num_samplers, 1);
                std::vector<std::string> sampler_names(num_samplers);
                const size_t sarr = resolve_rel(data, mpos + 0x44);
                const size_t sdict = resolve_rel(data, mpos + 0x48);
                if (sarr && sarr + size_t(num_samplers) * 8 <= data.size()) {
                    for (uint8_t s = 0; s < num_samplers; ++s) sampler_types[s] = data[sarr + size_t(s) * 8 + 1];
                }
                if (sarr && sdict && sdict + 8 <= data.size()) {
                    const uint32_t sc = read_u32_be(data.data() + sdict + 4);
                    for (uint32_t s = 0; s < sc && s < num_samplers; ++s) {
                        const size_t de = sdict + 24 + size_t(s) * 16;
                        if (de + 16 > data.size()) break;
                        const size_t n_off = resolve_rel(data, de + 8);
                        if (!n_off) continue;
                        const size_t ptr = resolve_rel(data, de + 12);
                        const size_t idx =
                            (ptr >= sarr && ptr < sarr + size_t(num_samplers) * 8 && ((ptr - sarr) % 8) == 0)
                                ? (ptr - sarr) / 8
                                : size_t(s);
                        if (idx < num_samplers) sampler_names[idx] = read_cstr(data, n_off);
                    }
                }

                const size_t parr = resolve_rel(data, mpos + 0x58);
                if (!parr) continue;
                for (uint16_t p = 0; p < num_progs; ++p) {
                    const size_t pp = parr + size_t(p) * 0x4C;
                    if (pp + 0x4C > data.size()) break;
                    const uint32_t attrib_mask = read_u32_be(data.data() + pp + 0x04);
                    const size_t vs_off = resolve_rel(data, pp + 0x34);
                    const size_t ps_off = resolve_rel(data, pp + 0x40);
                    if (!vs_off || !ps_off || vs_off + 0x18 > data.size() || ps_off + 0x18 > data.size()) continue;
                    if (read_u32_be(data.data() + vs_off) != 0x02010000u ||
                        read_u32_be(data.data() + ps_off) != 0x05010000u)
                        continue;

                    const uint32_t vs_sz = read_u32_be(data.data() + vs_off + 0x08);
                    const uint32_t vs_dl_sz = read_u32_be(data.data() + vs_off + 0x0C);
                    const size_t vs_bc_off = resolve_rel(data, vs_off + 0x10);
                    const size_t vs_dl_off = resolve_rel(data, vs_off + 0x14);

                    const uint32_t ps_sz = read_u32_be(data.data() + ps_off + 0x08);
                    const uint32_t ps_dl_sz = read_u32_be(data.data() + ps_off + 0x0C);
                    const size_t ps_bc_off = resolve_rel(data, ps_off + 0x10);
                    const size_t ps_dl_off = resolve_rel(data, ps_off + 0x14);
                    if (!vs_bc_off || !ps_bc_off || vs_bc_off + vs_sz > data.size() || ps_bc_off + ps_sz > data.size())
                        continue;

                    std::array<uint32_t, 0x10000> regs{};
                    std::fill(regs.begin() + 0xA0E0, regs.begin() + 0xA100, 0xFFu);
                    regs[0xA206] = 0x43Fu;
                    init_default_texture_registers(regs);

                    auto apply_dl = [&](size_t dl_off, uint32_t dl_sz) {
                        if (!dl_off || dl_off + dl_sz > data.size()) return;
                        const auto dl = gx2_decode_static_display_list(data.data() + dl_off, dl_sz);
                        for (const auto& w : dl.writes) {
                            if (w.index < regs.size()) {
                                regs[w.index] = w.value;
                                if (w.index == 0xA238) gx2_apply_semantic_clear(regs.data(), w.value);
                            }
                        }
                    };
                    apply_dl(vs_dl_off, vs_dl_sz);
                    apply_dl(ps_dl_off, ps_dl_sz);

                    std::vector<uint8_t> user_array_ps_units;
                    bool has_alpha_tex = false;
                    const size_t sloc = resolve_rel(data, pp + 0x2C);
                    if (sloc && sloc + size_t(num_samplers) * 4 <= data.size()) {
                        for (uint8_t s = 0; s < num_samplers; ++s) {
                            const uint8_t vs_unit = data[sloc + size_t(s) * 4 + 0];
                            const uint8_t ps_unit = data[sloc + size_t(s) * 4 + 2];
                            if (vs_unit < 18) set_sampler_registers(regs, 0xE460, vs_unit, sampler_types[s]);
                            if (ps_unit < 18) {
                                set_sampler_registers(regs, 0xE000, ps_unit, sampler_types[s]);
                                if (sampler_names[s].starts_with("_a")) has_alpha_tex = true;
                                if (sampler_types[s] == 10 && sampler_names[s].starts_with("gsys_user"))
                                    user_array_ps_units.push_back(ps_unit);
                            }
                        }
                    }

                    std::array<bool, 256> vs_uses{};
                    for (uint32_t k = 0; k < 32; ++k) {
                        const uint32_t sem = regs[0xA0E0 + k];
                        if (sem < 256u) vs_uses[sem] = true;
                    }
                    auto layouts =
                        extract_fvtx_layouts(data, fvtx_offsets, secondary_uv_formats, attr_defs, attrib_mask, vs_uses);
                    if (layouts.empty()) {
                        std::vector<R700FetchAttribute> fallback;
                        for (size_t a = 0; a < attr_defs.size(); ++a) {
                            if (!attr_defs[a].valid || (a < 32 && ((attrib_mask >> a) & 1u) == 0)) continue;
                            R700FetchAttribute fa;
                            if (var_type_to_fetch(attr_defs[a].var_type, attr_defs[a].semantic, fa))
                                fallback.push_back(fa);
                        }
                        layouts.push_back({std::move(fallback), false});
                    }

                    const auto vs_bc = data.subspan(vs_bc_off, vs_sz);
                    const auto ps_bc = data.subspan(ps_bc_off, ps_sz);
                    ++collector.stats_.fsha_programs;
                    const FvtxLayout* primary_layout = nullptr;
                    for (const auto& layout : layouts) {
                        if (!primary_layout && !layout.synthesized_uv) primary_layout = &layout;
                        const auto& attrs = layout.attrs;
                        AotCollector::PretranslatedVs cached_vs{};
                        regs[0xA104] = 0u;
                        if (!collector.submit_program(vs_bc, ps_bc, regs, attrs, uint32_t(attrs.size()), 0, 0, "FSHA",
                                                      {}, &cached_vs))
                            continue;
                        if (!layout.synthesized_uv && has_alpha_tex) {
                            // Alpha-test GEQUAL (0xE) variant for cutout/foliage materials with an albedo map.
                            regs[0xA104] = 0xEu;
                            collector.submit_program(vs_bc, ps_bc, regs, attrs, uint32_t(attrs.size()), 0, 0, "FSHA",
                                                     {}, &cached_vs);
                        }
                    }
                    // Paintable stage shaders declare gsys_user* as SAMPLER_2D_ARRAY but often bind 2D textures.
                    // Submit once on the primary layout so all 2D PS translations & SPIR-V stages stay cached within RAM budget.
                    if (!user_array_ps_units.empty() && !layouts.empty()) {
                        const auto& attrs = (primary_layout ? *primary_layout : layouts.front()).attrs;
                        AotCollector::PretranslatedVs cached_vs{};
                        auto user_regs = regs;
                        for (uint8_t pu : user_array_ps_units) set_sampler_registers(user_regs, 0xE000, pu, 1u);
                        user_regs[0xA104] = 0u;
                        collector.submit_program(vs_bc, ps_bc, user_regs, attrs, uint32_t(attrs.size()), 0, 0, "FSHA",
                                                 {}, &cached_vs);
                        if (has_alpha_tex) {
                            user_regs[0xA104] = 0xEu;
                            collector.submit_program(vs_bc, ps_bc, user_regs, attrs, uint32_t(attrs.size()), 0, 0,
                                                     "FSHA", {}, &cached_vs);
                        }
                    }
                }
            }
        }
        pos += 8;
    }
}

void extract_bahs_shaders(std::span<const uint8_t> data, std::string_view name_hint, AotCollector& collector) {
    if (data.size() < 28) return;
    const bool is_lght = name_hint.find("agl_technique_lght") != std::string_view::npos;
    const bool is_gambit_proc = name_hint.find("Gambit_Proc") != std::string_view::npos;
    const uint32_t name_len = read_u32_le(data.data() + 20);
    const size_t pos = 24 + size_t(name_len);
    if (pos + 8 > data.size()) return;
    const uint32_t count = read_u32_le(data.data() + pos + 4);
    size_t cur = pos + 8;

    struct BahsStage {
        bool valid = false;
        std::array<uint8_t, 0x134> desc_be{};
        std::span<const uint8_t> bytecode;
        std::vector<std::pair<uint32_t, uint32_t>> samplers; // (loc, type)
        std::vector<R700FetchAttribute> attributes;
        uint32_t num_attribs = 0;
    };

    BahsStage last_vs{};
    for (uint32_t i = 0; i < count && cur + 16 <= data.size(); ++i) {
        const uint32_t esz = read_u32_le(data.data() + cur);
        const uint32_t stype = read_u32_le(data.data() + cur + 4);
        const uint32_t body_sz = read_u32_le(data.data() + cur + 12);
        if (esz < 16 || cur + esz > data.size() || body_sz > esz - 16) break;
        const auto base = data.subspan(cur + 16, body_sz);

        if (stype == 0 && base.size() >= 0x134) {
            const uint32_t mode = read_u32_le(base.data() + 0xD8);
            if (mode == 2) {
                last_vs = {};
                cur += esz;
                continue;
            }
            BahsStage vs{};
            const uint32_t bc_sz = read_u32_le(base.data() + 0xD0);
            const uint32_t bc_off = read_u32_le(base.data() + 0xD4);
            if (bc_sz && bc_off + bc_sz <= base.size()) {
                for (size_t off = 0; off < 0x134; off += 4)
                    write_u32_be(vs.desc_be.data() + off, read_u32_le(base.data() + off));
                vs.bytecode = base.subspan(bc_off, bc_sz);
                const uint32_t sv_c = read_u32_le(base.data() + 0xFC);
                const uint32_t sv_p = read_u32_le(base.data() + 0x100);
                if (sv_p + size_t(sv_c) * 12 <= base.size()) {
                    for (uint32_t s = 0; s < sv_c; ++s) {
                        const uint32_t vtype = read_u32_le(base.data() + sv_p + size_t(s) * 12 + 4);
                        const uint32_t loc = read_u32_le(base.data() + sv_p + size_t(s) * 12 + 8);
                        vs.samplers.emplace_back(loc, vtype);
                    }
                }
                const uint32_t av_c = read_u32_le(base.data() + 0x104);
                const uint32_t av_p = read_u32_le(base.data() + 0x108);
                vs.num_attribs = av_c;
                if (av_p + size_t(av_c) * 16 <= base.size()) {
                    for (uint32_t a = 0; a < av_c; ++a) {
                        const uint32_t vtype = read_u32_le(base.data() + av_p + size_t(a) * 16 + 4);
                        const uint32_t loc = read_u32_le(base.data() + av_p + size_t(a) * 16 + 12);
                        R700FetchAttribute fa;
                        if (var_type_to_fetch(vtype, loc, fa)) vs.attributes.push_back(fa);
                    }
                }
                vs.valid = true;
            }
            last_vs = std::move(vs);
        } else if (stype == 1 && base.size() >= 0xE8 && last_vs.valid) {
            const uint32_t bc_sz = read_u32_le(base.data() + 0xA4);
            const uint32_t bc_off = read_u32_le(base.data() + 0xA8);
            if (bc_sz && bc_off + bc_sz <= base.size()) {
                std::array<uint8_t, 0xE8> ps_desc_be{};
                for (size_t off = 0; off < 0xE8; off += 4)
                    write_u32_be(ps_desc_be.data() + off, read_u32_le(base.data() + off));

                std::array<uint32_t, 0x10000> regs{};
                regs[0xA206] = 0x43Fu;
                init_default_texture_registers(regs);
                r700_shader_registers(regs.data(), last_vs.desc_be.data(), false);
                r700_shader_registers(regs.data(), ps_desc_be.data(), true);
                for (const auto& [loc, vtype] : last_vs.samplers) set_sampler_registers(regs, 0xE460, loc, vtype);

                const uint32_t sv_c = read_u32_le(base.data() + 0xD0);
                const uint32_t sv_p = read_u32_le(base.data() + 0xD4);
                bool ps0_2d = false;
                bool ps0_arr = false;
                if (sv_p + size_t(sv_c) * 12 <= base.size()) {
                    for (uint32_t s = 0; s < sv_c; ++s) {
                        const uint32_t vtype = read_u32_le(base.data() + sv_p + size_t(s) * 12 + 4);
                        const uint32_t loc = read_u32_le(base.data() + sv_p + size_t(s) * 12 + 8);
                        set_sampler_registers(regs, 0xE000, loc, vtype);
                        if (loc == 0 && vtype == 1) ps0_2d = true;
                        if (loc == 0 && vtype == 10) ps0_arr = true;
                    }
                }

                const auto ps_bc = base.subspan(bc_off, bc_sz);
                ++collector.stats_.bahs_pairs;
                AotCollector::PretranslatedVs cached_vs{};
                for (uint32_t alpha : {0u, 0xAu, 0xCu, 0xEu}) {
                    regs[0xA104] = alpha;
                    collector.submit_program(last_vs.bytecode, ps_bc, regs, last_vs.attributes, last_vs.num_attribs, 0,
                                             0, "BAHS", {}, &cached_vs);
                }
                if (is_lght && ps0_2d) {
                    auto arr_regs = regs;
                    set_sampler_registers(arr_regs, 0xE000, 0, 10);
                    for (uint32_t alpha : {0u, 0xAu, 0xCu, 0xEu}) {
                        arr_regs[0xA104] = alpha;
                        collector.submit_program(last_vs.bytecode, ps_bc, arr_regs, last_vs.attributes,
                                                 last_vs.num_attribs, 0, 0, "BAHS", {}, &cached_vs);
                    }
                }
                if (is_gambit_proc && ps0_arr) {
                    auto d2_regs = regs;
                    set_sampler_registers(d2_regs, 0xE000, 0, 1);
                    for (uint32_t alpha : {0u, 0xAu, 0xCu, 0xEu}) {
                        d2_regs[0xA104] = alpha;
                        collector.submit_program(last_vs.bytecode, ps_bc, d2_regs, last_vs.attributes,
                                                 last_vs.num_attribs, 0, 0, "BAHS", {}, &cached_vs);
                    }
                }
                regs[0xA104] = 0u;
                collector.submit_program(last_vs.bytecode, ps_bc, regs, last_vs.attributes, last_vs.num_attribs, 0, 1,
                                         "BAHS");
                // agl_common / agl_technique_pfx attribute order and shared-VAO variants.
                if (last_vs.attributes.size() >= 2) {
                    const bool is_sem_sorted =
                        std::is_sorted(last_vs.attributes.begin(), last_vs.attributes.end(),
                                       [](const auto& a, const auto& b) { return a.semantic < b.semantic; });
                    if (!is_sem_sorted) {
                        auto sorted_attrs = last_vs.attributes;
                        std::stable_sort(sorted_attrs.begin(), sorted_attrs.end(),
                                         [](const auto& a, const auto& b) { return a.semantic < b.semantic; });
                        collector.submit_program(last_vs.bytecode, ps_bc, regs, sorted_attrs,
                                                 uint32_t(sorted_attrs.size()), 0, 0, "BAHS");
                        collector.submit_program(last_vs.bytecode, ps_bc, regs, sorted_attrs,
                                                 uint32_t(sorted_attrs.size()), 0, 1, "BAHS");
                    }
                    if (last_vs.attributes.size() == 2 && last_vs.attributes[0].format == 0x811u &&
                        last_vs.attributes[1].format == 0x80Du) {
                        auto float2_attrs = last_vs.attributes;
                        if (var_type_to_fetch(9u, float2_attrs[0].semantic, float2_attrs[0]))
                            collector.submit_program(last_vs.bytecode, ps_bc, regs, float2_attrs, 2u, 0, 0, "BAHS");
                    }
                } else if (last_vs.attributes.size() == 1) {
                    if (last_vs.attributes[0].format == 0x80Du) {
                        auto float3_attrs = last_vs.attributes;
                        if (var_type_to_fetch(10u, float3_attrs[0].semantic, float3_attrs[0]))
                            collector.submit_program(last_vs.bytecode, ps_bc, regs, float3_attrs, 1u, 0, 0, "BAHS");
                    } else if (last_vs.attributes[0].format == 0x811u) {
                        auto float2_attrs = last_vs.attributes;
                        if (var_type_to_fetch(9u, float2_attrs[0].semantic, float2_attrs[0]))
                            collector.submit_program(last_vs.bytecode, ps_bc, regs, float2_attrs, 1u, 0, 0, "BAHS");
                    }
                    if (last_vs.attributes[0].semantic == 0) {
                        auto padded_attrs = last_vs.attributes;
                        R700FetchAttribute extra{};
                        if (var_type_to_fetch(9u, 1u, extra)) {
                            padded_attrs.push_back(extra);
                            collector.submit_program(last_vs.bytecode, ps_bc, regs, padded_attrs, 2u, 0, 0, "BAHS");
                        }
                    }
                }
            }
        } else if (stype == 2) {
            last_vs = {};
        }
        cur += esz;
    }
}

int particle_attr_rank(std::string_view name) {
    static constexpr std::string_view kOrder[] = {
        "sysPosAttr",
        "sysNormalAttr",
        "sysTangentAttr",
        "sysVertexColor0Attr",
        "sysTexCoordAttr",
        "sysLocalPosAttr",
        "sysLocalVecAttr",
        "sysLocalDiffAttr",
        "sysScaleAttr",
        "sysRandomAttr",
        "sysInitRotateAttr",
        "sysColor0Attr",
        "sysColor1Attr",
        "sysEmtMat0Attr",
        "sysEmtMat1Attr",
        "sysEmtMat2Attr",
        "sysEmtRTMat0Attr",
        "sysEmtRTMat1Attr",
        "sysEmtRTMat2Attr",
        "sysInPos",
        "sysInVec",
        "sysEmitterPluginAttr0",
        "sysEmitterPluginAttr1",
        "sysEmitterPluginAttr2",
        "sysEmitterPluginAttr3",
        "sysEmitterPluginAttr4",
    };
    for (int i = 0; i < int(std::size(kOrder)); ++i) {
        if (name == kOrder[i]) return i;
    }
    return 1000;
}

void extract_gfx2_shaders(std::span<const uint8_t> data, std::string_view name_hint, AotCollector& collector) {
    struct Gfx2Vs {
        bool valid = false;
        bool skip_gs = false;
        bool has_streamout = false;
        bool has_vs12_shadow = false;
        std::array<uint8_t, 0x134> desc{};
        std::vector<std::pair<uint32_t, uint32_t>> samplers;
        std::vector<R700FetchAttribute> attributes;
        std::vector<R700FetchAttribute> ranked_attributes;
        uint32_t num_attribs = 0;
        std::span<const uint8_t> bytecode;
    };
    struct Gfx2Ps {
        bool valid = false;
        std::array<uint8_t, 0xE8> desc{};
        std::vector<std::pair<uint32_t, uint32_t>> samplers;
        std::span<const uint8_t> bytecode;
    };

    const bool is_layout =
        name_hint.find("lyt_") != std::string_view::npos || name_hint.find("font_") != std::string_view::npos;
    const bool is_particle = name_hint.find("Particle") != std::string_view::npos;
    const bool is_layout_or_particle = is_layout || is_particle || name_hint.find(".bgsh") != std::string_view::npos;

    size_t pos = 0;
    while (pos + 32 <= data.size()) {
        if (std::memcmp(data.data() + pos, "Gfx2", 4) != 0 || read_u32_be(data.data() + pos + 4) != 32u) {
            ++pos;
            continue;
        }
        const size_t gfx2_pos = pos;
        size_t off = pos + 32;
        std::vector<Gfx2Vs> vss;
        std::vector<Gfx2Ps> pss;
        Gfx2Vs cur_vs{};
        Gfx2Ps cur_ps{};

        while (off + 32 <= data.size() && std::memcmp(data.data() + off, "BLK{", 4) == 0) {
            const uint32_t bsz = read_u32_be(data.data() + off + 4);
            const uint32_t btype = read_u32_be(data.data() + off + 16);
            const uint32_t dlen = read_u32_be(data.data() + off + 20);
            if (off + bsz + dlen > data.size()) break;
            const auto payload = data.subspan(off + bsz, dlen);
            if (btype == 3 && payload.size() >= 0x134) {
                cur_vs = Gfx2Vs{};
                if (read_u32_be(payload.data() + 0xD8) == 2) {
                    cur_vs.skip_gs = true;
                } else {
                    std::memcpy(cur_vs.desc.data(), payload.data(), 0x134);
                    cur_vs.has_streamout = read_u32_be(payload.data() + 0x110) != 0;
                    const uint32_t sv_c = read_u32_be(payload.data() + 0xFC);
                    const uint32_t sv_p = read_u32_be(payload.data() + 0x100) & 0xFFFFFu;
                    if (sv_p + size_t(sv_c) * 12 <= payload.size()) {
                        for (uint32_t s = 0; s < sv_c; ++s) {
                            const uint32_t vtype = read_u32_be(payload.data() + sv_p + size_t(s) * 12 + 4);
                            const uint32_t loc = read_u32_be(payload.data() + sv_p + size_t(s) * 12 + 8);
                            cur_vs.samplers.emplace_back(loc, vtype);
                            if (loc == 12u && vtype == 6u) cur_vs.has_vs12_shadow = true;
                        }
                    }
                    const uint32_t av_c = read_u32_be(payload.data() + 0x104);
                    const uint32_t av_p = read_u32_be(payload.data() + 0x108) & 0xFFFFFu;
                    cur_vs.num_attribs = av_c;
                    if (av_p + size_t(av_c) * 16 <= payload.size()) {
                        struct RankedAttr {
                            int rank;
                            R700FetchAttribute attr;
                        };
                        std::vector<RankedAttr> ranked;
                        if (is_particle) ranked.reserve(av_c);
                        for (uint32_t a = 0; a < av_c; ++a) {
                            const uint32_t name_off = read_u32_be(payload.data() + av_p + size_t(a) * 16) & 0xFFFFFu;
                            const uint32_t vtype = read_u32_be(payload.data() + av_p + size_t(a) * 16 + 4);
                            const uint32_t loc = read_u32_be(payload.data() + av_p + size_t(a) * 16 + 12);
                            R700FetchAttribute fa;
                            if (var_type_to_fetch(vtype, loc, fa)) {
                                cur_vs.attributes.push_back(fa);
                                if (is_particle) {
                                    const std::string attr_name =
                                        name_off < payload.size() ? read_cstr(payload, name_off) : std::string{};
                                    if (attr_name == "sysInPos" || attr_name == "sysInVec") fa.endian = 0u;
                                    ranked.push_back({particle_attr_rank(attr_name), fa});
                                }
                            }
                        }
                        if (is_particle && !ranked.empty()) {
                            std::stable_sort(ranked.begin(), ranked.end(),
                                             [](const RankedAttr& a, const RankedAttr& b) { return a.rank < b.rank; });
                            cur_vs.ranked_attributes.reserve(ranked.size());
                            for (const auto& item : ranked) cur_vs.ranked_attributes.push_back(item.attr);
                        }
                    }
                }
            } else if (btype == 5) {
                if (!cur_vs.skip_gs) {
                    cur_vs.bytecode = payload;
                    cur_vs.valid = true;
                }
                vss.push_back(std::move(cur_vs));
                cur_vs = Gfx2Vs{};
            } else if (btype == 6 && payload.size() >= 0xE8) {
                cur_ps = Gfx2Ps{};
                std::memcpy(cur_ps.desc.data(), payload.data(), 0xE8);
                const uint32_t sv_c = read_u32_be(payload.data() + 0xD0);
                const uint32_t sv_p = read_u32_be(payload.data() + 0xD4) & 0xFFFFFu;
                if (sv_p + size_t(sv_c) * 12 <= payload.size()) {
                    for (uint32_t s = 0; s < sv_c; ++s) {
                        cur_ps.samplers.emplace_back(read_u32_be(payload.data() + sv_p + size_t(s) * 12 + 8),
                                                     read_u32_be(payload.data() + sv_p + size_t(s) * 12 + 4));
                    }
                }
            } else if (btype == 7) {
                cur_ps.bytecode = payload;
                cur_ps.valid = true;
                pss.push_back(std::move(cur_ps));
                cur_ps = Gfx2Ps{};
            }
            off += bsz + dlen;
            if (btype == 1) break;
        }

        auto last_valid_ps = [&]() -> const Gfx2Ps* {
            for (auto it = pss.rbegin(); it != pss.rend(); ++it)
                if (it->valid) return &*it;
            return nullptr;
        };
        auto last_valid_vs = [&]() -> const Gfx2Vs* {
            for (auto it = vss.rbegin(); it != vss.rend(); ++it)
                if (it->valid) return &*it;
            return nullptr;
        };
        const Gfx2Ps* fallback_ps = last_valid_ps();
        const Gfx2Vs* fallback_vs = last_valid_vs();

        auto submit_pair = [&](const Gfx2Vs& vs, const Gfx2Ps& ps) {
            std::array<uint32_t, 0x10000> regs{};
            regs[0xA206] = 0x43Fu;
            init_default_texture_registers(regs);
            r700_shader_registers(regs.data(), vs.desc.data(), false);
            r700_shader_registers(regs.data(), ps.desc.data(), true);
            for (const auto& [loc, vtype] : vs.samplers) set_sampler_registers(regs, 0xE460, loc, vtype);
            for (const auto& [loc, vtype] : ps.samplers) set_sampler_registers(regs, 0xE000, loc, vtype);
            if (vs.has_streamout) regs[0xA2AC] = 1u;

            auto submit_layout = [&](const std::vector<R700FetchAttribute>& attrs, uint32_t pts) {
                auto submit_with_regs = [&](const std::array<uint32_t, 0x10000>& r, bool allow_rects) {
                    AotCollector::PretranslatedVs cached_vs{};
                    const uint32_t alphas[] = {0u, 0xCu, 0xFu};
                    const size_t n_alphas = is_layout ? 3u : (is_particle ? 2u : 1u);
                    for (size_t ai = 0; ai < n_alphas; ++ai) {
                        auto rcopy = r;
                        rcopy[0xA104] = alphas[ai];
                        collector.submit_program(vs.bytecode, ps.bytecode, rcopy, attrs, uint32_t(attrs.size()), 0, pts,
                                                 "Gfx2", name_hint, &cached_vs);
                        if (allow_rects && alphas[ai] == 0u && is_layout_or_particle) {
                            if (!is_particle) {
                                collector.submit_program(vs.bytecode, ps.bytecode, rcopy, attrs, uint32_t(attrs.size()),
                                                         1, pts, "Gfx2", name_hint);
                            }
                            collector.submit_program(vs.bytecode, ps.bytecode, rcopy, attrs, uint32_t(attrs.size()), 2,
                                                     pts, "Gfx2", name_hint);
                        }
                    }
                };
                submit_with_regs(regs, true);
                // Particle shaders declare texDynamicShadow on VS unit 12, which stays DIM_1D when unbound.
                if (is_particle && vs.has_vs12_shadow) {
                    auto r1d = regs;
                    r1d[0xE460 + 12 * 7] &= ~7u;
                    submit_with_regs(r1d, true);
                }
                // lyt_BuildinShader binds a 1D gradient LUT onto PS unit 0 in UI draws.
                if (name_hint.find("lyt_") != std::string_view::npos) {
                    auto r1d = regs;
                    set_sampler_registers(r1d, 0xE000, 0, 0);
                    submit_with_regs(r1d, false);
                }
            };

            ++collector.stats_.gfx2_pairs;
            const uint32_t pts = vs.has_streamout ? 1u : 0u;
            if (!vs.ranked_attributes.empty()) {
                submit_layout(vs.ranked_attributes, pts);
            }
            if (vs.ranked_attributes.empty() || !is_particle) {
                submit_layout(vs.attributes, pts);
            }

            // font_BuildinShader binds [aVertex, aColor(8_8_8_8_UNORM), aTexCoord0] at runtime.
            if (name_hint.find("font_") != std::string_view::npos && vs.attributes.size() == 3) {
                std::vector<R700FetchAttribute> font_attrs;
                font_attrs.push_back(vs.attributes[2]);
                R700FetchAttribute color = vs.attributes[0];
                color.format = 0xAu;
                color.endian = r700_native_attribute(0xAu).default_endian;
                color.destination_select = 0x00010203u;
                font_attrs.push_back(color);
                font_attrs.push_back(vs.attributes[1]);
                submit_layout(font_attrs, pts);
            } else if (is_layout && vs.attributes.size() == 1 && vs.attributes[0].format == 0x811u) {
                auto float2_attrs = vs.attributes;
                if (var_type_to_fetch(9u, float2_attrs[0].semantic, float2_attrs[0])) submit_layout(float2_attrs, pts);
            }
        };

        std::set<std::pair<uint32_t, uint32_t>> emtr_pairs;
        if (is_particle && gfx2_pos >= 32 && std::memcmp(data.data(), "EFTB", 4) == 0) {
            for (size_t ep = 0; ep + 32 <= gfx2_pos; ep += 4) {
                if (std::memcmp(data.data() + ep, "EMTR", 4) != 0) continue;
                const uint32_t sz = read_u32_be(data.data() + ep + 4);
                const uint32_t h4 = read_u32_be(data.data() + ep + 16);
                const uint32_t end_off = (h4 != 0xFFFFFFFFu && h4 <= sz) ? h4 : sz;
                if (end_off >= 0x128u && ep + end_off <= gfx2_pos) {
                    const size_t st = ep + end_off - 0x128u;
                    for (int k = 0; k < 3; ++k) {
                        const uint32_t vi = read_u32_be(data.data() + st + size_t(k) * 8);
                        const uint32_t pi = read_u32_be(data.data() + st + size_t(k) * 8 + 4);
                        if (vi < vss.size() && pi < pss.size()) emtr_pairs.emplace(vi, pi);
                    }
                }
            }
        }

        if (!emtr_pairs.empty()) {
            std::vector<bool> vs_covered(vss.size(), false);
            std::vector<bool> ps_covered(pss.size(), false);
            for (const auto& [vi, pi] : emtr_pairs) {
                if (!vss[vi].valid || !pss[pi].valid) continue;
                vs_covered[vi] = true;
                ps_covered[pi] = true;
                submit_pair(vss[vi], pss[pi]);
            }
            for (size_t i = 0; i < vss.size(); ++i) {
                if (!vs_covered[i] && vss[i].valid && fallback_ps) submit_pair(vss[i], *fallback_ps);
            }
            for (size_t i = 0; i < pss.size(); ++i) {
                if (!ps_covered[i] && pss[i].valid && fallback_vs) submit_pair(*fallback_vs, pss[i]);
            }
        } else {
            const size_t total = std::max(vss.size(), pss.size());
            for (size_t i = 0; i < total; ++i) {
                const Gfx2Vs* vs = (i < vss.size() && vss[i].valid) ? &vss[i] : fallback_vs;
                const Gfx2Ps* ps = (i < pss.size() && pss[i].valid) ? &pss[i] : fallback_ps;
                if (!vs || !ps) continue;
                submit_pair(*vs, *ps);
            }
        }
        pos = off;
    }
}

void process_rom_blob(std::string_view name, std::span<const uint8_t> data, AotCollector& collector) {
    std::vector<uint8_t> unpacked;
    if (data.size() >= 16 && std::memcmp(data.data(), "Yaz0", 4) == 0) {
        if (!decompress_yaz0(data, unpacked)) return;
        data = unpacked;
    }
    if (data.size() < 4) return;
    if (std::memcmp(data.data(), "SARC", 4) == 0) {
        std::vector<SarcEntry> entries;
        if (parse_sarc(data, entries)) {
            for (const auto& e : entries) process_rom_blob(e.name, e.data, collector);
        }
    } else if (std::memcmp(data.data(), "FRES", 4) == 0) {
        extract_fres_shaders(data, collector);
    } else if (std::memcmp(data.data(), "BAHS", 4) == 0) {
        extract_bahs_shaders(data, name, collector);
    } else if (std::memcmp(data.data(), "Gfx2", 4) == 0 || std::memcmp(data.data(), "EFTB", 4) == 0) {
        extract_gfx2_shaders(data, name, collector);
    }
}

void harvest_existing_cache(const fs::path& translated_dir, AotCollector& collector) {
    if (!fs::exists(translated_dir)) return;

    auto read_pack = [&](const fs::path& pack_path, bool is_v2) {
        std::vector<uint8_t> bytes;
        if (!read_file_bytes(pack_path, bytes) || bytes.size() < 16) return;
        const uint32_t magic = read_u32_le(bytes.data());
        const uint32_t ver = read_u32_le(bytes.data() + 4);
        const uint32_t keys = read_u32_le(bytes.data() + 8);
        const uint32_t bodies = read_u32_le(bytes.data() + 12);
        if (magic != kTranslationPackMagic || ver != 2) return;
        size_t pos = 16;
        if (pos + size_t(keys) * 12 + size_t(bodies) * 4 > bytes.size()) return;
        std::vector<std::vector<uint64_t>> users(bodies);
        for (uint32_t i = 0; i < keys; ++i) {
            uint64_t k = 0;
            std::memcpy(&k, bytes.data() + pos, 8);
            const uint32_t b = read_u32_le(bytes.data() + pos + 8);
            pos += 12;
            if (b < bodies) users[b].push_back(k);
        }
        std::vector<uint32_t> sizes(bodies);
        for (uint32_t b = 0; b < bodies; ++b) {
            sizes[b] = read_u32_le(bytes.data() + pos);
            pos += 4;
        }
        for (uint32_t b = 0; b < bodies; ++b) {
            if (pos + sizes[b] > bytes.size()) break;
            const auto blob = std::span<const uint8_t>(bytes.data() + pos, sizes[b]);
            pos += sizes[b];
            if (is_v2) {
                for (uint64_t k : users[b]) collector.import_existing_v2_blob(k, blob, 0, 0, false);
            } else {
                collector.import_v1_glsl_for_spirv(blob);
            }
        }
    };

    // Preserve any existing v2_c1_warm.bin records (rects/points/masks).
    const fs::path warm_path = translated_dir / (std::string(kCachePrefix) + "warm.bin");
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> existing_warm;
    if (std::FILE* wf = std::fopen(warm_path.string().c_str(), "rb")) {
        uint32_t hdr[2]{};
        if (std::fread(hdr, 4, 2, wf) == 2 && hdr[0] == kWarmMagic && hdr[1] == kWarmVersion) {
            for (uint32_t rec[2]; std::fread(rec, 4, 2, wf) == 2;) {
                if (rec[1] > (1u << 20)) break;
                std::vector<uint8_t> d(rec[1]);
                if (std::fread(d.data(), 1, d.size(), wf) != d.size()) break;
                if (rec[0] == kWarmProgram && rec[1] == sizeof(WarmProgramRecord)) {
                    WarmProgramRecord p;
                    std::memcpy(&p, d.data(), sizeof(p));
                    existing_warm.try_emplace(p.key, std::make_pair(p.rect, p.points));
                } else if (rec[0] == kWarmMasks && rec[1] == sizeof(WarmMasksRecord)) {
                    WarmMasksRecord m;
                    std::memcpy(&m, d.data(), sizeof(m));
                    collector.add_mask(m.base, m.reads);
                }
            }
        }
        std::fclose(wf);
    }

    read_pack(translated_dir / (std::string(kCachePrefix) + "pack.bin"), true);
    read_pack(translated_dir / "v1_c1_pack.bin", false);

    std::error_code ec;
    for (fs::directory_iterator it(translated_dir, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& path = it->path();
        if (path.extension() != ".tl") continue;
        const std::string name = path.filename().string();
        if (name.starts_with(kCachePrefix) && name.size() == std::strlen(kCachePrefix) + 19) {
            const uint64_t k = std::strtoull(name.substr(std::strlen(kCachePrefix), 16).c_str(), nullptr, 16);
            if (collector.key_to_body_.count(k)) continue;
            std::vector<uint8_t> blob;
            if (!read_file_bytes(path, blob)) continue;
            auto wit = existing_warm.find(k);
            collector.import_existing_v2_blob(k, blob, wit != existing_warm.end() ? wit->second.first : 0u,
                                              wit != existing_warm.end() ? wit->second.second : 0u,
                                              wit != existing_warm.end());
        }
    }
}

int run_legacy_prepare(const char* pack_arg, const char* cache_dir, const char* rejected_arg) {
    gx2vk::shader_set_cache_dir(cache_dir);
    std::vector<uint8_t> data;
    if (!read_file_bytes(pack_arg, data)) return 2;
    size_t pos = 0;
    auto u32 = [&]() {
        if (pos + 4 > data.size()) throw std::runtime_error("truncated pack");
        uint32_t n;
        std::memcpy(&n, data.data() + pos, 4);
        pos += 4;
        return n;
    };
    try {
        if (u32() != kTranslationPackMagic || u32() != kTranslationPackVersion) return 3;
        const auto keys = u32(), bodies = u32();
        pos += size_t(keys) * 12;
        std::vector<uint32_t> sizes;
        sizes.reserve(bodies);
        for (unsigned i = 0; i < bodies; ++i) sizes.push_back(u32());
        std::map<std::pair<unsigned, std::string>, bool> unique;
        std::ofstream rejected(rejected_arg);
        if (!rejected) return 8;
        unsigned compiled = 0, failed = 0;
        unsigned body_index = 0;
        for (auto size : sizes) {
            const size_t end = pos + size;
            if (end > data.size() || size < 28 || u32() != kTranslationMagic || u32() != kTranslationVersion) return 4;
            pos += 8;
            bool body_valid = true;
            for (auto stage : {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT}) {
                const auto n = u32();
                if (pos + n > end) return 5;
                std::string source(reinterpret_cast<const char*>(data.data() + pos), n);
                pos += n;
                auto [it, inserted] = unique.emplace(std::make_pair(unsigned(stage), source), false);
                if (inserted) {
                    std::vector<uint32_t> spirv;
                    std::string log;
                    it->second = gx2vk::compile_glsl(stage, source, spirv, &log);
                    if (!it->second) {
                        ++failed;
                        RBREW_LOG_ERROR("aot", "Shader failure: %s\n", log.c_str());
                    } else {
                        ++compiled;
                    }
                }
                body_valid &= it->second;
            }
            if (!body_valid) rejected << body_index << '\n';
            ++body_index;
            pos = end;
        }
        RBREW_LOG_INFO("aot", "Unique shaders verified: %u, failed: %u\n", compiled, failed);
        return failed ? 6 : 0;
    } catch (const std::exception& e) {
        RBREW_LOG_ERROR("aot", "%s\n", e.what());
        return 7;
    }
}

int run_aot(const fs::path& rom_or_content, const fs::path& cache_dir) {
    const auto t0 = std::chrono::steady_clock::now();
    fs::path content_dir = rom_or_content;
    if (fs::exists(rom_or_content / "content")) content_dir = rom_or_content / "content";
    if (!fs::exists(content_dir) || !fs::is_directory(content_dir)) {
        RBREW_LOG_ERROR("aot", "Content directory not found: %s\n", content_dir.string().c_str());
        return 2;
    }

    gx2vk::shader_set_cache_dir(cache_dir.string());
    const fs::path translated_dir = cache_dir / "translated";
    AotCollector collector;

    RBREW_LOG_INFO("aot", "Harvesting existing cache in %s...\n", translated_dir.string().c_str());
    harvest_existing_cache(translated_dir, collector);

    std::vector<fs::path> archive_files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(content_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        const std::string ext = it->path().extension().string();
        if (ext == ".pack" || ext == ".szs" || ext == ".sarc" || ext == ".bfres" || ext == ".gsh" ||
            ext == ".sharcfb" || ext == ".ptcl") {
            archive_files.push_back(it->path());
        }
    }

    RBREW_LOG_INFO("aot", "Scanning %zu Wii U content archives in %s...\n", archive_files.size(),
                   content_dir.string().c_str());
    std::vector<uint8_t> file_bytes;
    std::atomic<size_t> scanned{0};
    {
        ProgressReporter scan_progress("Scanning archives", scanned, archive_files.size());
        for (const auto& archive_path : archive_files) {
            if (read_file_bytes(archive_path, file_bytes)) {
                process_rom_blob(archive_path.filename().string(), file_bytes, collector);
            }
            scanned.fetch_add(1, std::memory_order_relaxed);
        }
        scan_progress.stop();
    }

    RBREW_LOG_INFO("aot",
                   "Extracted FSHA(models=%zu, progs=%zu), BAHS(pairs=%zu), Gfx2(pairs=%zu), migrated_v2=%zu | "
                   "translated=%zu failed=%zu (VS=%zu PS=%zu FSHA=%zu BAHS=%zu Gfx2=%zu) bodies=%zu keys=%zu\n",
                   collector.stats_.fsha_models, collector.stats_.fsha_programs, collector.stats_.bahs_pairs,
                   collector.stats_.gfx2_pairs, collector.stats_.migrated_v2, collector.stats_.translated_programs,
                   collector.stats_.translation_failures, collector.stats_.vs_failures, collector.stats_.ps_failures,
                   collector.stats_.fsha_failures, collector.stats_.bahs_failures, collector.stats_.gfx2_failures,
                   collector.bodies_.size(), collector.key_to_body_.size());

    RBREW_LOG_INFO("aot", "Compiling unique GLSL stages to SPIR-V...\n");
    const bool spirv_ok = collector.compile_all_spirv();
    RBREW_LOG_INFO("aot", "SPIR-V stages=%zu failures=%zu\n", collector.stats_.unique_spirv_stages,
                   collector.stats_.spirv_failures);

    if (!collector.write_outputs(translated_dir)) {
        RBREW_LOG_ERROR("aot", "Failed writing AOT translation pack/warm cache to %s\n",
                        translated_dir.string().c_str());
        return 5;
    }

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    (void)secs;
    RBREW_LOG_INFO("aot", "AOT shader preparation completed in %.2f s (%spack.bin & %swarm.bin)\n", secs, kCachePrefix,
                   kCachePrefix);
    return spirv_ok && collector.stats_.translation_failures == 0 ? 0 : 6;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::string_view(argv[1]) == "--aot") return run_aot(argv[2], argv[3]);
    if (argc == 4) return run_legacy_prepare(argv[1], argv[2], argv[3]);
    RBREW_LOG_ERROR("aot", "Usage: cache_prepare --aot <rom_or_content_dir> <cache_dir>\n"
                           "       cache_prepare <pack.bin> <cache_dir> <rejected.txt>\n");
    return 2;
}

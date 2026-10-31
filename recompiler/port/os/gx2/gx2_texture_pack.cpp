// gx2_texture_pack.cpp - texture pack dumping and replacement (gx2_texture_pack.h).
#include "gx2_texture_pack.h"
#include "rebrewu/logger.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gx2texpack {
namespace {
const std::string& dump_dir() {
    static const std::string dir = [] {
        const char* v = std::getenv("RBREW_TEXTURE_DUMP");
        return std::string(v ? v : "");
    }();
    return dir;
}
// DXGI format of a Vulkan format, 0 if a pack cannot hold it.
uint32_t dxgi(VkFormat f) {
    switch (f) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return 71;
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return 72;
    case VK_FORMAT_BC2_UNORM_BLOCK: return 74;
    case VK_FORMAT_BC2_SRGB_BLOCK: return 75;
    case VK_FORMAT_BC3_UNORM_BLOCK: return 77;
    case VK_FORMAT_BC3_SRGB_BLOCK: return 78;
    case VK_FORMAT_BC4_UNORM_BLOCK: return 80;
    case VK_FORMAT_BC4_SNORM_BLOCK: return 81;
    case VK_FORMAT_BC5_UNORM_BLOCK: return 83;
    case VK_FORMAT_BC5_SNORM_BLOCK: return 84;
    case VK_FORMAT_R8G8B8A8_UNORM: return 28;
    case VK_FORMAT_R8G8B8A8_SRGB: return 29;
    default: return 0;
    }
}

struct Pack {
    std::filesystem::path dir;
    std::unordered_map<uint64_t, std::filesystem::path> files;
};
// The pack folder is listed once at the first texture upload.
const Pack& pack() {
    static const Pack p = [] {
        Pack out;
        if (const char* v = std::getenv("RBREW_TEXTURE_PACK"); v && *v)
            out.dir = v;
        else {
#if defined(_WIN32)
            wchar_t exe[32768]{};
            if (GetModuleFileNameW(nullptr, exe, 32768))
                out.dir = std::filesystem::path(exe).parent_path() / "textures";
#elif defined(__linux__)
            // Next to the executable, like on Windows (not the working directory).
            std::error_code exe_error;
            const auto exe = std::filesystem::read_symlink("/proc/self/exe", exe_error);
            out.dir = exe_error ? std::filesystem::path("textures") : exe.parent_path() / "textures";
#else
            out.dir = "textures";
#endif
        }
        std::error_code ec;
        if (!std::filesystem::is_directory(out.dir, ec)) return out;
        for (const auto& e : std::filesystem::directory_iterator(out.dir, ec)) {
            if (!e.is_regular_file(ec) || e.path().extension() != ".dds") continue;
            const std::string stem = e.path().stem().string();
            if (stem.size() != 16) continue;
            char* end = nullptr;
            const uint64_t h = std::strtoull(stem.c_str(), &end, 16);
            if (end && !*end) out.files.emplace(h, e.path());
        }
        if (!out.files.empty())
            RBREW_LOG_INFO("texpack", "%zu replacement textures in %s", out.files.size(), out.dir.string().c_str());
        return out;
    }();
    return p;
}
} // namespace

bool dumping() {
    return !dump_dir().empty();
}

void dump(uint64_t hash, VkFormat format, uint32_t width, uint32_t height, const uint8_t* level0, size_t bytes) {
    if (!dumping() || !level0 || width < 32 || height < 32) return;
    const uint32_t dx = dxgi(format);
    if (!dx) return;
    static std::mutex mutex;
    static std::unordered_set<uint64_t> written;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!written.insert(hash).second) return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dump_dir(), ec);
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.dds", (unsigned long long)hash);
    const auto path = std::filesystem::path(dump_dir()) / name;
    if (std::filesystem::exists(path, ec)) return;
    FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f) return;
    const bool block = dx != 28 && dx != 29;
    uint32_t header[31]{};
    header[0] = 124;                                                // dwSize
    header[1] = 0x1 | 0x2 | 0x4 | 0x1000 | (block ? 0x80000 : 0x8); // CAPS HEIGHT WIDTH PIXELFORMAT + LINEARSIZE/PITCH
    header[2] = height;
    header[3] = width;
    header[4] = block ? uint32_t(bytes) : width * 4;
    header[6] = 1; // mip count
    header[18] = 32;
    header[19] = 0x4; // pixel format: size, FOURCC
    std::memcpy(&header[20], "DX10", 4);
    header[26] = 0x1000;                       // caps: TEXTURE
    const uint32_t dx10[5] = {dx, 3, 0, 1, 0}; // format, TEXTURE2D, misc, array size, misc2
    std::fwrite("DDS ", 1, 4, f);
    std::fwrite(header, 4, 31, f);
    std::fwrite(dx10, 4, 5, f);
    std::fwrite(level0, 1, bytes, f);
    std::fclose(f);
}

bool replacing() {
    return !pack().files.empty();
}

bool replacement(uint64_t hash, VkFormat format, uint32_t width, uint32_t height, size_t bytes,
                 std::vector<uint8_t>& out) {
    const auto& p = pack();
    const auto it = p.files.find(hash);
    if (it == p.files.end()) return false;
    FILE* f = std::fopen(it->second.string().c_str(), "rb");
    if (!f) return false;
    uint32_t header[37]{}; // "DDS " + 31 header words + DX10 (5 words)
    const bool read = std::fread(header, 4, 37, f) == 37;
    // File words: 3 height, 4 width, 21 FOURCC, 32 DXGI format.
    bool ok = read && std::memcmp(header, "DDS ", 4) == 0 && header[3] == height && header[4] == width &&
              std::memcmp(&header[21], "DX10", 4) == 0 && header[32] == dxgi(format);
    if (ok) {
        out.resize(bytes);
        ok = std::fread(out.data(), 1, bytes, f) == bytes;
    }
    std::fclose(f);
    static std::atomic<int> warned{0};
    if (!ok && warned.fetch_add(1) < 8)
        RBREW_LOG_WARN("texpack", "%016llx: format or size does not match the game texture (%ux%u)",
                       (unsigned long long)hash, width, height);
    return ok;
}

bool hd_replacement(uint64_t hash, VkFormat format, uint32_t width, uint32_t height, HdTexture& out) {
    const auto& p = pack();
    const auto it = p.files.find(hash);
    if (it == p.files.end()) return false;
    FILE* f = std::fopen(it->second.string().c_str(), "rb");
    if (!f) return false;
    uint32_t header[37]{};
    bool ok = std::fread(header, 4, 37, f) == 37 && std::memcmp(header, "DDS ", 4) == 0 && header[1] == 124 &&
              header[19] == 32 && std::memcmp(&header[21], "DX10", 4) == 0 && header[33] == 3 && header[34] == 0 &&
              header[35] == 1;
    const uint32_t game = dxgi(format);
    const bool srgb = game == 72 || game == 75 || game == 78 || game == 29;
    const bool colour_block = game == 71 || game == 72 || game == 74 || game == 75 || game == 77 || game == 78;
    out.format = format;
    if (ok && header[32] != game) {
        // Uncompressed RGBA8 in place of a colour BC texture (no re-encoding loss).
        if (colour_block && header[32] == (srgb ? 29u : 28u))
            out.format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        else
            ok = false;
    }
    const uint32_t H = header[3], W = header[4];
    uint32_t scale = 0;
    for (uint32_t k : {2u, 4u, 8u})
        if (W == uint64_t(width) * k && H == uint64_t(height) * k) scale = k;
    uint32_t max_levels = 1;
    for (uint32_t size = std::max(W, H); size > 1; size >>= 1) ++max_levels;
    ok = ok && game && scale && W <= 16384 && H <= 16384 && header[7] <= max_levels;
    if (ok) {
        const uint32_t dx = header[32];
        const bool block = dx != 28 && dx != 29;
        const uint32_t block_bytes = dx == 71 || dx == 72 || dx == 80 || dx == 81 ? 8 : 16;
        const uint32_t levels = std::max(1u, header[7]);
        out.width = W;
        out.height = H;
        out.levels.clear();
        for (uint32_t level = 0; level < levels && ok; ++level) {
            const uint32_t lw = std::max(1u, W >> level), lh = std::max(1u, H >> level);
            const size_t bytes = block ? size_t((lw + 3) / 4) * ((lh + 3) / 4) * block_bytes : size_t(lw) * lh * 4;
            std::vector<uint8_t> data(bytes);
            if (std::fread(data.data(), 1, bytes, f) != bytes) {
                ok = false;
                break;
            }
            out.levels.push_back(std::move(data));
        }
    }
    std::fclose(f);
    if (!ok) out = HdTexture{};
    return ok && !out.levels.empty();
}
} // namespace gx2texpack

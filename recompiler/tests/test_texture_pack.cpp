#include "os/gx2/gx2_texture_pack.h"
#include "os/gx2/gx2_material_response.h"
#include "rebrewu/logger.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}
void pack_path(const std::filesystem::path& dir) {
#ifdef _WIN32
    _putenv_s("RBREW_TEXTURE_PACK", dir.string().c_str());
#else
    setenv("RBREW_TEXTURE_PACK", dir.string().c_str(), 1);
#endif
}
std::vector<uint8_t> fixture() {
    // Authored 8x8 signed BC5, four complete mip levels; no title assets.
    uint32_t h[37]{};
    std::memcpy(h, "DDS ", 4);
    h[1] = 124;
    h[2] = 0xa1007;
    h[3] = h[4] = 8;
    h[5] = 64;
    h[7] = 4;
    h[19] = 32;
    h[20] = 4;
    std::memcpy(h + 21, "DX10", 4);
    h[27] = 0x401008;
    h[32] = 84;
    h[33] = 3;
    h[35] = 1;
    std::vector<uint8_t> data(sizeof h + 112);
    std::memcpy(data.data(), h, sizeof h);
    for (size_t at = sizeof h; at < data.size(); at += 16) {
        data[at] = 64;
        data[at + 1] = 32;
        data[at + 8] = uint8_t(-32);
        data[at + 9] = uint8_t(-64);
    }
    return data;
}
void set(std::vector<uint8_t>& data, unsigned word, uint32_t value) {
    std::memcpy(data.data() + word * 4, &value, 4);
}
} // namespace
int main(int argc, char** argv) {
    try {
        require(gx2material::response(0) == nullptr, "unknown texture received a material response");
        for (const auto& r : gx2material::responses) {
            require(gx2material::response(r.albedo_hash) == &r, "material identity lookup changed");
            std::vector<uint8_t> block(652, 0x5a);
            auto put = [&](size_t at, float f) { std::memcpy(block.data() + at, &f, 4); };
            for (unsigned i = 0; i < 3; ++i) {
                put(240 + i * 4, r.original_albedo[i]);
                put(368 + i * 4, r.original_specular[i]);
            }
            put(380, r.original_intensity);
            put(384, r.original_roughness);
            put(420, .5f);
            const auto original = block;
            require(gx2material::apply(block, r), "confirmed native material was not calibrated");
            for (size_t i = 0; i < block.size(); ++i) {
                const bool parameter = (i >= 240 && i < 252) || (i >= 368 && i < 388) || (i >= 420 && i < 424);
                require(parameter || block[i] == original[i], "material adjustment changed unrelated shader data");
            }
            require(gx2material::value(block, 384) == r.roughness && gx2material::value(block, 420) == r.normal_weight,
                    "wrong roughness or normal strength");
            for (size_t length : {0u, 240u, 384u, 423u}) {
                auto short_block = original;
                short_block.resize(length);
                const auto before = short_block;
                require(!gx2material::apply(short_block, r) && short_block == before, "short block was modified");
            }
            block = original;
            put(240, 9.f);
            auto before = block;
            require(!gx2material::apply(block, r) && block == before,
                    "same texture in a different material was modified");
            block = original;
            put(384, std::numeric_limits<float>::quiet_NaN());
            before = block;
            require(!gx2material::apply(block, r) && block == before,
                    "non-finite or incompatible shader layout was modified");
            block = original;
            for (size_t i = 0; i < block.size() / 4; ++i)
                std::reverse(block.begin() + i * 4, block.begin() + i * 4 + 4);
            before = block;
            require(!gx2material::apply(block, r) && block == before, "wrong-endian material block was modified");
        }
        for (const auto& r : gx2material::responses) {
            std::vector<uint8_t> source(2048, 0x5a);
            auto put = [&](size_t at, float f) { std::memcpy(source.data() + at, &f, 4); };
            for (unsigned i = 0; i < 3; ++i) {
                put(960 + i * 4, r.original_albedo[i]);
                put(1472 + i * 4, r.original_specular[i]);
            }
            put(1520, r.original_intensity);
            put(1536, r.original_roughness);
            put(1680, .5f);
            require(gx2material::layout(source, r) == 4, "native word-addressed layout was not recognized");
            auto expected = source;
            require(gx2material::apply(expected, r), "native word-addressed parameters not changed");
            // The translator compacts only used vec4s, in a different order.
            const unsigned source_words[] = {95, 60, 105, 92, 96};
            const unsigned destinations[] = {4, 1, 3, 0, 2};
            std::array<uint8_t, 80> packed{}, wanted{};
            for (unsigned i = 0; i < 5; ++i) {
                const size_t at = size_t(source_words[i]) * 16, out = size_t(destinations[i]) * 16;
                std::memcpy(packed.data() + out, source.data() + at, 16);
                std::memcpy(wanted.data() + out, expected.data() + at, 16);
                require(gx2material::apply_range({packed.data() + out, 16}, at, r, 4),
                        "native remapped vec4 missed its parameter");
            }
            require(packed == wanted, "compacted native uniforms differ from the confirmed parameter map");
            const auto before = packed;
            require(!gx2material::apply_range(packed, 0, r, 2) && packed == before,
                    "unsupported offset unit modified data");
        }
        std::puts("PASS: material identity, native parameter offsets, unrelated bytes, foreign material/layout, finite "
                  "values and bounds");
        if (argc == 2) {
            pack_path(argv[1]);
            struct Material {
                uint64_t hash;
                uint32_t w, h;
            };
            for (const auto& m : {Material{0xfbad5c5d26c90a46, 512, 512}, Material{0x29fc844be0655494, 512, 512},
                                  Material{0x3e00e13e84054435, 256, 256}, Material{0x7c090ab5acc54b89, 512, 256}}) {
                gx2texpack::HdTexture out;
                require(gx2texpack::hd_replacement(m.hash, VK_FORMAT_BC5_SNORM_BLOCK, m.w, m.h, out),
                        "installed normal map rejected by native loader");
                require(out.format == VK_FORMAT_BC5_SNORM_BLOCK && out.width == m.w * 4 && out.height == m.h * 4,
                        "normal map changed format, sign or scale");
                require(out.levels.size() == (m.w == 256 ? 11u : 12u), "installed normal map lost mip levels");
                for (unsigned i = 0; i < out.levels.size(); ++i) {
                    const auto w = std::max(1u, out.width >> i), h = std::max(1u, out.height >> i);
                    require(out.levels[i].size() == size_t((w + 3) / 4) * ((h + 3) / 4) * 16,
                            "installed mip has incorrect block extent");
                }
            }
            std::puts("PASS: all four installed 4x signed normal maps and their complete mip chains load through the "
                      "native pack reader");
            return 0;
        }
        require(argc == 1, "optional argument: local texture-pack directory");
        const auto dir =
            std::filesystem::temp_directory_path() /
            ("rbrew-normal-pack-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(std::filesystem::create_directory(dir), "fixture directory already exists");
        struct Cleanup {
            std::filesystem::path dir;
            std::vector<std::filesystem::path> files;
            ~Cleanup() {
                std::error_code e;
                for (const auto& f : files) std::filesystem::remove(f, e);
                std::filesystem::remove(dir, e);
            }
        } cleanup{dir};
        const auto original = fixture();
        for (unsigned i = 1; i <= 8; ++i) {
            auto data = original;
            if (i == 2) set(data, 32, 83); // unsigned normal is not signed normal
            if (i == 3) set(data, 4, 0);   // invalid extent
            if (i == 4) data.pop_back();   // truncated last mip must not silently succeed
            if (i == 5) set(data, 35, 2);  // array is not a 2D texture
            if (i == 6) set(data, 7, 15);  // too many mip levels for 8x8
            if (i == 7) set(data, 32, 72); // colour sRGB is not normal data
            if (i == 8) set(data, 1, 0);   // corrupt header
            char name[32];
            std::snprintf(name, sizeof name, "%016llx.dds", (unsigned long long)i);
            const auto file = dir / name;
            cleanup.files.push_back(file);
            std::ofstream f(file, std::ios::binary);
            f.write(reinterpret_cast<const char*>(data.data()), data.size());
            require(bool(f), "fixture write failed");
        }
        pack_path(dir);
        gx2texpack::HdTexture out;
        require(gx2texpack::hd_replacement(1, VK_FORMAT_BC5_SNORM_BLOCK, 4, 4, out),
                "valid signed 2x normal map rejected");
        require(out.format == VK_FORMAT_BC5_SNORM_BLOCK && out.width == 8 && out.height == 8 && out.levels.size() == 4,
                "valid normal map contract changed");
        require(out.levels[0][8] == uint8_t(-32) && out.levels[3].size() == 16,
                "signed channels or final block mip changed");
        for (uint64_t i = 2; i <= 8; ++i)
            require(!gx2texpack::hd_replacement(i, VK_FORMAT_BC5_SNORM_BLOCK, 4, 4, out) && out.levels.empty(),
                    "invalid normal map accepted or partial mip chain exposed");
        std::puts(
            "PASS: signed BC5, complete mips, sign/colour mismatch, truncated mip, array, extent and header rejection");
        return 0;
    } catch (const std::exception& e) {
        RBREW_LOG_ERROR("test", "FAIL: %s", e.what());
        return 1;
    }
}

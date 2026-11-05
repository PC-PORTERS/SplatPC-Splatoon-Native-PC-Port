#pragma once

#include "../core/manifest/manifest.hpp"
#include "../core/rpx/rpx_types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace rebrewu::patches {

struct EncodedPatch {
    uint32_t address{};
    uint32_t word{};
    std::string instruction;
};

// Assembles every [patches] entry without touching an image. Errors are one
// line per bad patch, prefixed with its address.
bool encode(const manifest::Manifest& manifest, std::vector<EncodedPatch>& encoded, std::vector<std::string>& errors);

// Assembles the patches and checks that every address lands in an executable
// section of `image`, without writing anything.
bool validate(const manifest::Manifest& manifest, const rpx::RpxModule& image, std::vector<std::string>& errors);

// Rewrites the executable sections of `image` with the manifest's patches.
// All patches are validated and encoded before anything is written, so a bad
// patch leaves the image untouched.
bool apply(const manifest::Manifest& manifest, rpx::RpxModule& image, std::vector<std::string>& errors);

} // namespace rebrewu::patches

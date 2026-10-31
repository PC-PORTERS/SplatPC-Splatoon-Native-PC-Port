#include "manifest_patches.hpp"

#include "../core/elf/elf_types.hpp"
#include "../ppc/assembler/ppc_asm.hpp"
#include <map>

namespace rebrewu::patches {
namespace {

struct Target {
    std::size_t section{};
    std::size_t offset{};
};

bool resolve(const manifest::Manifest& manifest, const rpx::RpxModule& image, const std::vector<EncodedPatch>& encoded,
             std::vector<Target>& targets, std::vector<std::string>& errors) {
    targets.clear();
    targets.reserve(encoded.size());

    for (const EncodedPatch& patch : encoded) {
        const std::string where = manifest::format_address(patch.address);
        bool found = false;
        for (std::size_t i = 0; i < image.sections.size(); ++i) {
            const rpx::RpxSection& section = image.sections[i];
            if (!section.is_allocated() || !section.is_executable()) continue;
            if (patch.address < section.address) continue;
            if (uint64_t(patch.address) + 4u > uint64_t(section.address) + section.data.size()) continue;
            targets.push_back({i, static_cast<std::size_t>(patch.address - section.address)});
            found = true;
            break;
        }
        if (!found) errors.push_back("[patches] " + where + ": not in an executable section");
    }
    return errors.empty();
}

} // namespace

bool encode(const manifest::Manifest& manifest, std::vector<EncodedPatch>& encoded, std::vector<std::string>& errors) {
    encoded.clear();
    encoded.reserve(manifest.patches.size());
    std::map<uint32_t, std::string> seen;

    for (const manifest::Patch& patch : manifest.patches) {
        const std::string where = manifest::format_address(patch.address);
        const auto [it, inserted] = seen.emplace(patch.address, patch.instruction);
        if (!inserted) {
            errors.push_back("[patches] " + where + ": duplicate address");
            continue;
        }

        uint32_t word = 0;
        std::string error;
        if (!ppc::assemble(patch.instruction, patch.address, word, &error)) {
            errors.push_back("[patches] " + where + ": " + error);
            continue;
        }
        encoded.push_back({patch.address, word, patch.instruction});
    }
    return errors.empty();
}

bool validate(const manifest::Manifest& manifest, const rpx::RpxModule& image, std::vector<std::string>& errors) {
    std::vector<EncodedPatch> encoded;
    if (!encode(manifest, encoded, errors)) return false;
    std::vector<Target> targets;
    return resolve(manifest, image, encoded, targets, errors);
}

bool apply(const manifest::Manifest& manifest, rpx::RpxModule& image, std::vector<std::string>& errors) {
    std::vector<EncodedPatch> encoded;
    if (!encode(manifest, encoded, errors)) return false;

    std::vector<Target> targets;
    if (!resolve(manifest, image, encoded, targets, errors)) return false;

    for (std::size_t i = 0; i < encoded.size(); ++i) {
        const uint32_t word = encoded[i].word;
        std::vector<uint8_t>& data = image.sections[targets[i].section].data;
        const std::size_t offset = targets[i].offset;
        data[offset] = static_cast<uint8_t>(word >> 24);
        data[offset + 1] = static_cast<uint8_t>(word >> 16);
        data[offset + 2] = static_cast<uint8_t>(word >> 8);
        data[offset + 3] = static_cast<uint8_t>(word);
    }
    return true;
}

} // namespace rebrewu::patches

#include "core/elf/elf_types.hpp"
#include "core/manifest/manifest.hpp"
#include "core/rpx/rpx_types.hpp"
#include "patches/manifest_patches.hpp"
#include "ppc/assembler/ppc_asm.hpp"
#include <cstdio>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition) return;
    std::printf("FAIL: %s\n", message);
    ++failures;
}

void check_word(const char* text, uint32_t address, uint32_t expected) {
    uint32_t word = 0;
    std::string error;
    if (!rebrewu::ppc::assemble(text, address, word, &error)) {
        std::printf("FAIL: %s: %s\n", text, error.c_str());
        ++failures;
        return;
    }
    if (word == expected) return;
    std::printf("FAIL: %s: got 0x%08X, expected 0x%08X\n", text, word, expected);
    ++failures;
}

rebrewu::rpx::RpxModule executable_image() {
    rebrewu::rpx::RpxModule image;
    rebrewu::rpx::RpxSection text;
    text.name = ".text";
    text.address = 0x0261C900;
    text.size = 32;
    text.flags = rebrewu::elf::SHF_ALLOC | rebrewu::elf::SHF_EXECINSTR;
    text.data.assign(text.size, 0xAA);
    image.sections.push_back(std::move(text));
    return image;
}

} // namespace

int main() {
    check_word("cmpwi r6, 1", 0x0261C918, 0x2C060001);
    check_word("nop", 0, 0x60000000);
    check_word("blr", 0, 0x4E800020);
    check_word("li r3, 0x10", 0, 0x38600010);
    check_word("stw r4, -8(r1)", 0, 0x9081FFF8);
    check_word("beq 0x00000100", 0xF8, 0x41820008);
    check_word("b 0xFFFFFFFC", 0, 0x4BFFFFFC);
    check_word("beq 0xFFFFFFFC", 0, 0x4182FFFC);
    check_word("bla 0x00000100", 0, 0x48000103);
    check_word("ba 0x01FFFFFC", 0, 0x49FFFFFE);
    check_word("ba 0xFE000000", 0, 0x4A000002);
    check_word("ba 0xFFFFFFFC", 0, 0x4BFFFFFE);
    check_word("bca 12, 2, 0x7FFC", 0, 0x41827FFE);
    check_word("bca 12, 2, 0xFFFF8000", 0, 0x41828002);
    check_word("or r3, r4, r5", 0, 0x7C832B78);
    check_word("fmul f1, f2, f3", 0, 0xFC2200F2);

    uint32_t word = 0;
    std::string error;
    check(!rebrewu::ppc::assemble("not_an_instruction", 0, word, &error), "invalid instruction accepted");
    check(!rebrewu::ppc::assemble("b 0x10000000", 0, word, &error), "out-of-range branch accepted");
    check(!rebrewu::ppc::assemble("beq 0x8000", 0, word, &error), "out-of-range conditional accepted");
    check(!rebrewu::ppc::assemble("ba 0x02000000", 0, word, &error), "wrapping absolute branch accepted");
    check(!rebrewu::ppc::assemble("ba 0xFC000000", 0, word, &error), "wrapping absolute branch accepted");
    check(!rebrewu::ppc::assemble("bca 12, 2, 0x8000", 0, word, &error), "wrapping absolute conditional accepted");
    check(!rebrewu::ppc::assemble("bca 12, 2, 0xFFFF7FFC", 0, word, &error), "wrapping absolute conditional accepted");

    const auto parsed = rebrewu::manifest::parse_text("[manifest]\n"
                                                      "format = rebrew-manifest/1\n"
                                                      "[patches]\n"
                                                      "0x0261C918 = cmpwi r6, 1\n"
                                                      "0x0261C91C = blr\n",
                                                      "test");
    check(parsed.errors.empty(), "valid patches rejected");
    check(parsed.patches.size() == 2, "valid patches not parsed");

    const auto unaligned = rebrewu::manifest::parse_text("[manifest]\n"
                                                         "[patches]\n"
                                                         "0x0261C919 = nop\n",
                                                         "test");
    check(!unaligned.errors.empty(), "unaligned patch accepted");

    const auto duplicate = rebrewu::manifest::parse_text("[manifest]\n"
                                                         "[patches]\n"
                                                         "0x0261C918 = nop\n"
                                                         "0x0261c918 = blr\n",
                                                         "test");
    check(!duplicate.errors.empty(), "duplicate patch address accepted");

    const std::map<std::string, std::string> empty_section;
    const std::string empty_hash = rebrewu::manifest::format_hash(rebrewu::manifest::section_hash(empty_section));
    std::string ignored_text = "[manifest]\n";
    for (const char* section : {"identity", "modules", "imports", "entries", "hooks"})
        ignored_text += "[" + std::string(section) + "]\nhash = " + empty_hash + "\n";
    ignored_text += "[patches]\nhash = Undefined\n0x0261C918 = nop\n";
    const auto ignored_hash = rebrewu::manifest::parse_text(ignored_text, "test");
    check(ignored_hash.errors.empty(), "undefined patch hash rejected");
    check(ignored_hash.ignored_hashes.contains("patches"), "undefined patch hash not marked ignored");
    check(!ignored_hash.computed_hashes.contains("patches"), "undefined patch hash still computed");
    auto ignored_manifest = ignored_hash;
    check(rebrewu::manifest::verify_hashes(ignored_manifest), "undefined patch hash was verified");

    const auto entries = rebrewu::manifest::parse_text("[manifest]\n"
                                                       "[entries]\n"
                                                       "0x0261C918 = normal\n"
                                                       "0x0261C91C = force\n"
                                                       "0x0261C920 = corroborate\n",
                                                       "test");
    check(entries.errors.empty(), "valid entries rejected");
    check(entries.entries.size() == 3, "entries not parsed");
    check(!entries.entries[0].force && !entries.entries[0].corroborate, "normal entry has the wrong mode");
    check(entries.entries[1].force && !entries.entries[1].corroborate, "force entry has the wrong mode");
    check(!entries.entries[2].force && entries.entries[2].corroborate, "corroborating entry has the wrong mode");

    const auto duplicate_entry = rebrewu::manifest::parse_text("[manifest]\n"
                                                               "[entries]\n"
                                                               "0x0261C918 = normal\n"
                                                               "0x0261c918 = force\n",
                                                               "test");
    check(!duplicate_entry.errors.empty(), "duplicate entry address accepted");

    const auto bad_entry_mode = rebrewu::manifest::parse_text("[manifest]\n"
                                                              "[entries]\n"
                                                              "0x0261C918 = maybe\n",
                                                              "test");
    check(!bad_entry_mode.errors.empty(), "unknown entry mode accepted");

    const auto hooks = rebrewu::manifest::parse_text("[manifest]\n"
                                                     "[hooks]\n"
                                                     "0x0261C918 = hook\n"
                                                     "0x0261C91C = hook\n",
                                                     "test");
    check(hooks.errors.empty(), "valid hooks rejected");
    check(hooks.hooks.size() == 2, "hooks not parsed");

    const auto bad_hook = rebrewu::manifest::parse_text("[manifest]\n"
                                                        "[hooks]\n"
                                                        "0x0261C919 = hook\n",
                                                        "test");
    check(!bad_hook.errors.empty(), "unaligned hook accepted");

    const auto duplicate_hook = rebrewu::manifest::parse_text("[manifest]\n"
                                                              "[hooks]\n"
                                                              "0x0261C918 = hook\n"
                                                              "0x0261c918 = hook\n",
                                                              "test");
    check(!duplicate_hook.errors.empty(), "duplicate hook address accepted");

    auto image = executable_image();
    rebrewu::manifest::Manifest manifest;
    manifest.patches.push_back({0x0261C918, "cmpwi r6, 1"});
    std::vector<std::string> errors;
    check(rebrewu::patches::apply(manifest, image, errors), "valid patch failed to apply");
    const auto& data = image.sections[0].data;
    check(data[24] == 0x2C && data[25] == 0x06 && data[26] == 0x00 && data[27] == 0x01,
          "patched bytes are not big-endian");

    const auto before = image.sections[0].data;
    manifest.patches.push_back({0x0261C91C, "not_an_instruction"});
    check(!rebrewu::patches::apply(manifest, image, errors), "invalid patch applied");
    check(image.sections[0].data == before, "failed patch partially modified the image");

    manifest.patches = {{0x01000000, "nop"}};
    check(!rebrewu::patches::apply(manifest, image, errors), "patch outside executable sections applied");

    if (failures == 0) std::printf("manifest patches: all checks passed\n");
    return failures == 0 ? 0 : 1;
}

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace rebrewu::ppc {

// Assembles one PowerPC (Espresso) instruction. `address` is the guest address
// of the instruction, used to encode PC-relative branches. Numeric branch
// targets are absolute guest addresses; `b`/`bc` fail if the displacement does
// not fit, while `ba`/`bca` encode an absolute branch.
//
// Returns false and fills `error` when the text is not a supported instruction.
bool assemble(std::string_view text, uint32_t address, uint32_t& word, std::string* error);

} // namespace rebrewu::ppc

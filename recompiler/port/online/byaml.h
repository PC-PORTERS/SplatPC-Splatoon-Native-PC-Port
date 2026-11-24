// byaml.h - BYAML version 1, big-endian (Wii U), writer only: the same bytes as tools/boss/byaml.py dump().
#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace rbrew_native {

// root: an object or an array. Throws std::invalid_argument for values BYAML cannot hold.
std::string byaml_dump(const nlohmann::ordered_json& root);

} // namespace rbrew_native

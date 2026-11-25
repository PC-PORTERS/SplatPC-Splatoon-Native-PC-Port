// online_login.h - native server health check, first-time registration, pseudo claim,
// session token acquisition, and status output.
#pragma once
#include "social_http.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace rbrew_native {

std::optional<std::array<int, 3>> parse_semver(const std::string& text);
std::string derive_health_path(const std::string& register_path);
bool is_valid_pseudo(const std::string& pseudo);

// Checks GET <health_path> (e.g. "/health") against installed_version.
bool check_server_version(Transport& transport, const std::string& health_path, const std::string& installed_version,
                          std::string& error_out);

// Registers (pid, password) once against register_url (at register_path) and writes game_dir/online_registered.txt.
bool ensure_registered(Transport& transport, const std::string& register_url, const std::string& register_path,
                       const std::filesystem::path& game_dir, uint32_t pid, const std::string& password,
                       std::string& error_out);

// Full login sequence: /health -> /register -> /social/pseudo -> /social/session (returns token_out).
bool perform_account_login(Transport& transport, const std::string& register_url, const std::string& register_path,
                           const std::filesystem::path& game_dir, uint32_t pid, const std::string& password,
                           const std::string& pseudo, const std::string& installed_version,
                           const nlohmann::json& client_proof, std::string& token_out, std::string& error_out,
                           std::string& detail_out);

// Writes a status message to <friends_dir>/state.res before FriendsRelay takes over.
void write_login_state(const std::filesystem::path& friends_dir, const std::string& message,
                       const std::string& detail = "");

} // namespace rbrew_native

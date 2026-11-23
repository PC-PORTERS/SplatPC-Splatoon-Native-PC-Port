#include "online_login.h"

#include <charconv>
#include <cstdio>
#include <fstream>
#include <regex>

namespace fs = std::filesystem;

namespace rbrew_native {
namespace {

std::string trim(std::string s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    const size_t e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::string pid_hex(uint32_t pid) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", pid);
    return buf;
}

void write_atomic(const fs::path& path, const std::string& text) {
    std::error_code ec;
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
    }
    fs::rename(tmp, path, ec);
    if (ec) fs::remove(tmp, ec);
}

std::string format_semver(const std::array<int, 3>& v) {
    return std::to_string(v[0]) + "." + std::to_string(v[1]) + "." + std::to_string(v[2]);
}

std::string error_text(const nlohmann::json& body, const std::string& fallback) {
    if (body.is_object() && body.contains("error") && body["error"].is_string()) {
        const std::string s = body["error"].get<std::string>();
        if (!s.empty()) return s;
    }
    return fallback;
}

} // namespace

std::optional<std::array<int, 3>> parse_semver(const std::string& text) {
    static const std::regex kVer(R"(^v?(\d+)\.(\d+)(?:\.(\d+))?$)");
    const std::string cleaned = trim(text);
    std::smatch m;
    if (!std::regex_match(cleaned, m, kVer)) return std::nullopt;
    std::array<int, 3> out{0, 0, 0};
    for (size_t i = 0; i < 3; ++i) {
        if (!m[i + 1].matched) continue;
        const std::string s = m[i + 1].str();
        int val = 0;
        const auto r = std::from_chars(s.data(), s.data() + s.size(), val);
        if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
        out[i] = val;
    }
    return out;
}

std::string derive_health_path(const std::string& register_path) {
    static constexpr const char kSuffix[] = "/register";
    const size_t n = sizeof(kSuffix) - 1;
    if (register_path.size() >= n && register_path.compare(register_path.size() - n, n, kSuffix) == 0) {
        return register_path.substr(0, register_path.size() - n) + "/health";
    }
    return "/health";
}

bool is_valid_pseudo(const std::string& pseudo) {
    static const std::regex kPseudo(R"(^[A-Za-z0-9 _.\-]{2,10}$)");
    return std::regex_match(pseudo, kPseudo);
}

bool check_server_version(Transport& transport, const std::string& health_path, const std::string& installed_version,
                          std::string& error_out) {
    error_out.clear();
    const Reply r = transport.request("GET", health_path.empty() ? "/health" : health_path, nullptr);
    if (r.status != 200 || !r.body.is_object()) return true;

    auto minimum = parse_semver(r.body.value("min_client_version", std::string()));
    if (r.body.value("integrity_mode", std::string()) == "enforce") {
        if (auto latest = parse_semver(r.body.value("latest_version", std::string()))) {
            if (!minimum || *latest > *minimum) minimum = latest;
        }
    }
    const auto installed = parse_semver(installed_version);
    if (minimum && installed && *installed < *minimum) {
        error_out = "This server needs version " + format_semver(*minimum) + " or newer. Update the game.";
        return false;
    }
    return true;
}

bool ensure_registered(Transport& transport, const std::string& register_url, const std::string& register_path,
                       const fs::path& game_dir, uint32_t pid, const std::string& password, std::string& error_out) {
    error_out.clear();
    if (register_url.empty()) return true;
    const fs::path marker = game_dir / "online_registered.txt";
    {
        std::ifstream in(marker, std::ios::binary);
        std::string line;
        if (in && std::getline(in, line) && trim(line) == trim(register_url)) return true;
    }
    const nlohmann::json payload = {{"pid", pid_hex(pid)}, {"password", password}};
    const Reply r = transport.request("POST", register_path.empty() ? "/register" : register_path, &payload);
    if (r.status == 0) {
        error_out = "The server cannot be reached. Check your Internet connection.";
        return false;
    }
    if (r.status >= 400) {
        switch (r.status) {
        case 409: error_out = "This account exists with another password. Keep your old online_identity.ini."; break;
        case 429: error_out = "Too many sign-ups from your connection. Try again later."; break;
        case 403: error_out = "This account is banned from the server."; break;
        default: error_out = "Sign-up refused by the server (" + std::to_string(r.status) + ")."; break;
        }
        return false;
    }
    write_atomic(marker, trim(register_url) + "\n");
    return true;
}

bool perform_account_login(Transport& transport, const std::string& register_url, const std::string& register_path,
                           const fs::path& game_dir, uint32_t pid, const std::string& password,
                           const std::string& pseudo, const std::string& installed_version,
                           const nlohmann::json& client_proof, std::string& token_out, std::string& error_out,
                           std::string& detail_out) {
    token_out.clear();
    error_out.clear();
    detail_out.clear();

    if (!is_valid_pseudo(pseudo)) {
        error_out = "Pseudo must be 2 to 10 characters (letters, digits, space, _ - .).";
        return false;
    }

    const std::string health_path = derive_health_path(register_path);
    if (!check_server_version(transport, health_path, installed_version, error_out)) return false;

    if (!register_url.empty() && register_path != "/") {
        if (!ensure_registered(transport, register_url, register_path, game_dir, pid, password, error_out))
            return false;
    }

    const Reply pseudo_reply =
        transport.post("pseudo", {{"pid", pid_hex(pid)}, {"password", password}, {"pseudo", pseudo}});
    if (pseudo_reply.status == 0) {
        error_out = "The server cannot be reached. Check your Internet connection.";
        return false;
    }
    if (pseudo_reply.status == 409) {
        error_out = "This pseudo is already taken by another player. Choose a different one.";
        return false;
    }
    if (pseudo_reply.status == 403) {
        error_out = "This account is banned from the server.";
        return false;
    }
    if (pseudo_reply.status >= 400) {
        error_out = error_text(pseudo_reply.body, "Pseudo refused (" + std::to_string(pseudo_reply.status) + ").");
        return false;
    }

    const Reply session_reply =
        transport.post("session", {{"pid", pid_hex(pid)}, {"password", password}, {"client", client_proof}});
    if (session_reply.status == 0) {
        error_out = "The server cannot be reached. Check your Internet connection.";
        return false;
    }
    if (session_reply.status == 426) {
        const std::string text = error_text(session_reply.body, "");
        if (session_reply.body.is_object() && session_reply.body.value("reason", nlohmann::json()) == "revision" &&
            !text.empty()) {
            error_out = text; // built from another game update: the server says which one it needs
        } else {
            error_out = "Game files not recognised by the server. Update the game.";
            detail_out = text;
        }
        return false;
    }
    if (session_reply.status == 403) {
        error_out = "This account is banned from the server.";
        return false;
    }
    if (session_reply.status != 200 || !session_reply.body.is_object() ||
        !session_reply.body.value("token", nlohmann::json()).is_string()) {
        error_out = error_text(session_reply.body, "Sign-in refused (" + std::to_string(session_reply.status) + ").");
        return false;
    }
    token_out = session_reply.body["token"].get<std::string>();
    if (token_out.empty()) {
        error_out = "Server returned an empty session token.";
        return false;
    }
    return true;
}

void write_login_state(const fs::path& friends_dir, const std::string& message, const std::string& detail) {
    if (friends_dir.empty()) return;
    std::string lines = "msg|" + message + "\n";
    if (!detail.empty()) lines += "detail|" + detail + "\n";
    write_atomic(friends_dir / "state.res", lines);
}

} // namespace rbrew_native

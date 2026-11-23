#pragma once
// Online mode: local identity, pseudo and NEX service token (testable).
//
// Off by default. online_server=ip:port in settings.ini (RBREW_ONLINE_SERVER
// wins) turns it on: the account becomes a network account whose NEX token
// points at that server. No console or Nintendo identity is used: the first
// online launch creates online_identity.ini next to the executable with a
// random principal id and NEX password, which identify the player (the
// pseudo is only a display name, also used as the Mii name in game).
#include "online_settings.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <stdexcept>
#include <limits>
#include <mutex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>

// Shared network trace (nsysnet.cpp, RBREW_NET_TRACE).
void rbrew_net_note(const char* what, const char* detail);

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#ifdef min
#undef min
#undef max
#endif
#pragma comment(lib, "bcrypt.lib")
#endif
namespace rbrew_online {

struct Identity {
    uint32_t pid = 0;     // NEX principal id
    std::string password; // NEX password (Kerberos key material)
};

constexpr uint32_t kNexTokenSize = 0x25C; // ACTNexAuthenticationResult (Cemu; Gambit 02BFC660)
constexpr uint32_t kTokenOffset = 0x000, kTokenMax = 0x200, kPasswordOffset = 0x204, kPasswordMax = 0x40,
                   kHostOffset = 0x248, kHostMax = 0x0F, kPortOffset = 0x258;

inline bool valid(const Identity& id) {
    if (!id.pid || id.pid > 0x7FFFFFFFu || id.password.size() != 16) return false;
    for (char c : id.password)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
    return true;
}
inline std::optional<Identity> parse(std::istream& in) {
    Identity id;
    for (const auto& [k, v] : rbrew_online_cfg::parse(in)) {
        if (k == "pid") {
            uint32_t n = 0;
            const auto result = std::from_chars(v.data(), v.data() + v.size(), n, 16);
            if (!v.empty() && result.ec == std::errc{} && result.ptr == v.data() + v.size()) id.pid = n;
        } else if (k == "password")
            id.password = v;
    }
    return valid(id) ? std::optional<Identity>(id) : std::nullopt;
}
template <class Rng> Identity create(Rng& rng) {
    static constexpr char kAlphabet[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    Identity id;
    id.pid = 0x10000000u + uint32_t(rng() % 0x6FFFFFFFu); // non-zero, positive as int32
    for (int i = 0; i < 16; ++i) id.password += kAlphabet[rng() % 62];
    return id;
}
inline std::string serialize(const Identity& id) {
    char pid[16];
    std::snprintf(pid, sizeof pid, "%08x", id.pid);
    return "# Splatoon PC online identity. Keep this file: it identifies you to friends and rooms.\n"
           "pid=" +
           std::string(pid) + "\npassword=" + id.password + "\n";
}
// Use the OS CSPRNG directly; an MT engine is not suitable for account secrets.
struct SecureRandom {
    using result_type = uint64_t;
    static constexpr result_type min() { return 0; }
    static constexpr result_type max() { return UINT64_MAX; }
    result_type operator()() {
        result_type value = 0;
#if defined(_WIN32)
        if (BCryptGenRandom(nullptr, reinterpret_cast<unsigned char*>(&value), sizeof(value), 2) < 0)
            throw std::runtime_error("OS random generator failed");
#else
        std::random_device device;
        value = (uint64_t(device()) << 32) ^ device();
#endif
        return value;
    }
};
// Existing invalid/unreadable files are preserved. Failure never returns an
// unpersisted identity, which would change the account on the next launch.
inline Identity load_or_create(const std::filesystem::path& file) {
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) {
        std::ifstream in(file);
        if (in)
            if (auto id = parse(in)) return *id;
        throw std::runtime_error("Online identity is unreadable or corrupt");
    }
    if (ec) throw std::runtime_error("Cannot inspect online identity");
    SecureRandom rng;
    const Identity id = create(rng);
    char suffix[48];
    std::snprintf(suffix, sizeof suffix, ".tmp.%016llx", (unsigned long long)rng());
    auto tmp = file;
    tmp += suffix;
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup{tmp};
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out || !(out << serialize(id)) || !out.flush()) throw std::runtime_error("Cannot persist online identity");
        out.close();
        if (!out) throw std::runtime_error("Cannot close online identity");
    }
#if defined(_WIN32)
    const bool published = MoveFileExW(tmp.c_str(), file.c_str(), 8) != 0; // no REPLACE_EXISTING: first creator wins
#else
    std::filesystem::create_hard_link(tmp, file, ec);
    const bool published = !ec;
#endif
    if (published) return id;
    // Another process may have won the atomic publication race.
    std::ifstream existing(file);
    if (existing)
        if (auto winner = parse(existing)) return *winner;
    throw std::runtime_error("Cannot publish online identity");
}
// "a.b.c.d:port" only (no names: DNS is confined).
inline bool parse_server(const std::string& text, std::string& host, uint16_t& port) {
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos || !colon || colon > kHostMax || colon + 1 == text.size()) return false;
    size_t start = 0;
    for (unsigned part = 0; part < 4; ++part) {
        const size_t end = part == 3 ? colon : text.find('.', start);
        if (end == std::string::npos || end >= colon + (part == 3) || end == start) return false;
        unsigned n = 0;
        const auto r = std::from_chars(text.data() + start, text.data() + end, n);
        if (r.ec != std::errc{} || r.ptr != text.data() + end || n > 255) return false;
        start = end + 1;
    }
    unsigned p = 0;
    const auto r = std::from_chars(text.data() + colon + 1, text.data() + text.size(), p);
    if (r.ec != std::errc{} || r.ptr != text.data() + text.size() || p == 0 || p > 65535) return false;
    host = text.substr(0, colon);
    port = uint16_t(p);
    return true;
}
// Pseudo (UTF-8) -> Mii name: at most 10 UTF-16 units, control characters dropped.
inline std::u16string mii_name(const std::string& utf8, std::u16string_view fallback) {
    std::u16string out;
    for (size_t i = 0; i < utf8.size() && out.size() < 10;) {
        const uint8_t first = uint8_t(utf8[i]);
        uint32_t cp = 0;
        size_t n = 0;
        if (first < 0x80) {
            cp = first;
            n = 1;
        } else if (first >= 0xC2 && first <= 0xDF) {
            cp = first & 31;
            n = 2;
        } else if (first >= 0xE0 && first <= 0xEF) {
            cp = first & 15;
            n = 3;
        } else if (first >= 0xF0 && first <= 0xF4) {
            cp = first & 7;
            n = 4;
        } else {
            ++i;
            continue;
        }
        bool good = i + n <= utf8.size();
        for (size_t k = 1; good && k < n; ++k) {
            const uint8_t c = uint8_t(utf8[i + k]);
            good = (c & 0xC0) == 0x80;
            cp = (cp << 6) | (c & 63);
        }
        if (!good) {
            ++i;
            continue;
        }
        i += n;
        if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000)) continue;
        // Also dropped: invisible and direction-changing characters (zero-width, bidi marks and overrides,
        // line/paragraph separators, byte-order mark), which let a pseudo read backwards or look empty.
        const bool invisible = (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202E) ||
                               (cp >= 0x2066 && cp <= 0x2069) || cp == 0xFEFF;
        if (cp >= 0x20 && !(cp >= 0x7F && cp < 0xA0) && cp <= 0xFFFF && !(cp >= 0xD800 && cp <= 0xDFFF) && !invisible)
            out += char16_t(cp);
    }
    while (!out.empty() && out.back() == u' ') out.pop_back();
    if (out.empty()) {
        for (char16_t c : fallback)
            if (out.size() < 10 && c >= 0x20 && c != 0x7F && !(c >= 0xD800 && c <= 0xDFFF)) out += c;
        while (!out.empty() && out.back() == u' ') out.pop_back();
    }
    return out;
}
// Writes ACTNexAuthenticationResult into `out` (kNexTokenSize bytes): every
// byte written, padding zero, strings truncated and NUL-terminated, port big-endian.
inline void fill_nex_token(uint8_t* out, std::string_view token, std::string_view password, std::string_view host,
                           uint16_t port) {
    std::memset(out, 0, kNexTokenSize);
    std::memcpy(out + kTokenOffset, token.data(), std::min<size_t>(token.size(), kTokenMax));
    std::memcpy(out + kPasswordOffset, password.data(), std::min<size_t>(password.size(), kPasswordMax));
    std::memcpy(out + kHostOffset, host.data(), std::min<size_t>(host.size(), kHostMax));
    out[kPortOffset] = uint8_t(port >> 8);
    out[kPortOffset + 1] = uint8_t(port);
}

inline constexpr std::string_view kPublicServer = "173.249.46.125:60000";
inline constexpr std::string_view kPublicNncs1 = "173.249.46.125";
inline constexpr std::string_view kPublicNncs2 = "13.140.129.165";
inline constexpr std::string_view kPublicRegister = "https://splatoonline.duckdns.org/register";

struct SessionToken {
    std::string token;
    std::string server;
    std::string register_url;
    uint32_t pid = 0;
    std::string pseudo;
    int64_t saved_at = 0;
};

inline std::atomic<bool>& runtime_offline_flag() {
    static std::atomic<bool> flag{false};
    return flag;
}
inline void set_session_offline(bool offline) {
    runtime_offline_flag().store(offline, std::memory_order_relaxed);
}
inline bool is_session_offline() {
    return runtime_offline_flag().load(std::memory_order_relaxed);
}

// Current online configuration.
struct Config {
    bool online = false;
    std::string host;
    uint16_t port = 0;
};
inline const Identity& identity();
// Reads the environment and settings.ini (uncached; config() keeps the result until the settings change).
inline Config read_config() {
    Config c;
    // RBREW_OFFLINE=1: offline whatever settings.ini or RBREW_ONLINE_SERVER names.
    if (const char* off = std::getenv("RBREW_OFFLINE"); off && off[0] == '1') return c;
    const std::string server = rbrew_online_cfg::resolved("RBREW_ONLINE_SERVER", "online_server", "");
    c.online = !server.empty() && parse_server(server, c.host, c.port);
    if (c.online && !valid(identity())) c.online = false; // failed persistence stays offline, with no invented account
    return c;
}
inline std::atomic<unsigned>& config_generation() {
    static std::atomic<unsigned> generation{0};
    return generation;
}
// The login screen calls this after writing online_server= and friends to settings.ini.
inline void config_changed() {
    config_generation().fetch_add(1, std::memory_order_release);
}
// Called from network and render code many times a frame: settings.ini is read again only after
// config_changed(). "Play offline" (set_session_offline) wins over the file.
inline Config config() {
    if (is_session_offline()) return {};
    static std::mutex mutex;
    static Config cached;
    static unsigned seen = ~0u;
    const unsigned generation = config_generation().load(std::memory_order_acquire);
    std::lock_guard lock(mutex);
    if (seen != generation) {
        cached = read_config();
        seen = generation;
    }
    return cached;
}

inline std::string pseudo() {
    return rbrew_online_cfg::resolved("RBREW_PSEUDO", "pseudo", "");
}
inline std::string resolve_register_url(const Config& cfg) {
    if (const char* env = std::getenv("RBREW_SOCIAL_URL"); env && *env) return env;
    const std::string configured = rbrew_online_cfg::resolved("RBREW_ONLINE_REGISTER", "online_register", "");
    if (!configured.empty()) return configured;
    if (!cfg.online) return {};
    const std::string server = cfg.host + ":" + std::to_string(cfg.port);
    const std::filesystem::path scripts_dir = rbrew_online_cfg::settings_path().parent_path().parent_path() / "scripts";
    for (const char* name : {"server.txt", "server.public.txt"}) {
        std::ifstream in(scripts_dir / name);
        if (!in) continue;
        const auto kv = rbrew_online_cfg::parse(in);
        const auto srv = kv.find("server");
        const auto reg = kv.find("register");
        if (srv != kv.end() && srv->second == server && reg != kv.end() && !reg->second.empty()) return reg->second;
    }
    if (server == kPublicServer) return std::string(kPublicRegister);
    return {};
}
inline std::string resolve_dns_override(const Config& cfg) {
    if (const char* env = std::getenv("RBREW_DNS_OVERRIDE"); env && *env) return env;
    if (!cfg.online) return {};
    std::string nncs1 = rbrew_online_cfg::resolved("RBREW_ONLINE_NNCS1", "online_nncs1", "");
    std::string nncs2 = rbrew_online_cfg::resolved("RBREW_ONLINE_NNCS2", "online_nncs2", "");
    if (nncs1.empty() || nncs2.empty()) {
        if (cfg.host == "173.249.46.125") {
            if (nncs1.empty()) nncs1 = std::string(kPublicNncs1);
            if (nncs2.empty()) nncs2 = std::string(kPublicNncs2);
        } else if (cfg.host == "127.0.0.1") {
            if (nncs1.empty()) nncs1 = "127.0.0.1";
            if (nncs2.empty()) nncs2 = "127.0.0.2";
        } else {
            if (nncs1.empty()) nncs1 = cfg.host;
            if (nncs2.empty()) nncs2 = cfg.host;
        }
    }
    return "nncs1.app.nintendowifi.net=" + nncs1 + ",nncs2.app.nintendowifi.net=" + nncs2;
}
inline std::filesystem::path friends_dir() {
    if (const char* env = std::getenv("RBREW_FRIENDS_DIR"); env && *env) return std::filesystem::path(env);
    if (!config().online) return {};
    return rbrew_online_cfg::settings_path().parent_path() / "friends";
}
inline std::filesystem::path session_token_path() {
    return rbrew_online_cfg::settings_path().parent_path() / "online_session.ini";
}
inline std::optional<SessionToken> load_session_token(const std::filesystem::path& file = session_token_path()) {
    std::ifstream in(file);
    if (!in) return std::nullopt;
    SessionToken s;
    for (const auto& [k, v] : rbrew_online_cfg::parse(in)) {
        if (k == "token")
            s.token = v;
        else if (k == "server")
            s.server = v;
        else if (k == "register")
            s.register_url = v;
        else if (k == "pseudo")
            s.pseudo = v;
        else if (k == "pid") {
            uint32_t n = 0;
            const auto r = std::from_chars(v.data(), v.data() + v.size(), n, 16);
            if (!v.empty() && r.ec == std::errc{} && r.ptr == v.data() + v.size()) s.pid = n;
        } else if (k == "saved_at") {
            int64_t ts = 0;
            const auto r = std::from_chars(v.data(), v.data() + v.size(), ts);
            if (!v.empty() && r.ec == std::errc{} && r.ptr == v.data() + v.size()) s.saved_at = ts;
        }
    }
    if (s.token.empty() || !s.pid) return std::nullopt;
    return s;
}
inline bool save_session_token(const SessionToken& s, const std::filesystem::path& file = session_token_path()) {
    if (s.token.empty() || !s.pid) return false;
    char pid_buf[16];
    std::snprintf(pid_buf, sizeof pid_buf, "%08x", s.pid);
    std::error_code ec;
    if (!file.parent_path().empty()) std::filesystem::create_directories(file.parent_path(), ec);
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << "# Splatoon PC saved online session token. Keep this file private.\n"
            << "token=" << s.token << "\n"
            << "server=" << s.server << "\n"
            << "register=" << s.register_url << "\n"
            << "pid=" << pid_buf << "\n"
            << "pseudo=" << s.pseudo << "\n"
            << "saved_at=" << s.saved_at << "\n";
        if (!out.flush()) return false;
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}
inline void clear_session_token(const std::filesystem::path& file = session_token_path()) {
    std::error_code ec;
    std::filesystem::remove(file, ec);
}
inline const Identity& identity() {
    static const Identity id = [] {
        try {
            return load_or_create(rbrew_online_cfg::settings_path().parent_path() / "online_identity.ini");
        } catch (const std::exception& e) {
            rbrew_net_note("identity unavailable", e.what());
            return Identity{};
        }
    }();
    return id;
}

} // namespace rbrew_online

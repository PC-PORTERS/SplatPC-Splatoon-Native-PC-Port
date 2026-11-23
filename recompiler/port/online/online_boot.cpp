#include "online_boot.h"
#include "boss_schedule.h"
#include "friends_relay.h"
#include "integrity_proof.h"
#include "online_login.h"
#include "social_http.h"
#include "wg_tunnel.h"

#include "online_identity.h"
#include "rebrewu/logger.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <thread>

namespace fs = std::filesystem;

namespace rbrew_native {
namespace {

constexpr auto kInterval = std::chrono::seconds(5);
constexpr int kTimeoutMs = 6000;
constexpr int64_t kMaxSavedTokenAgeSec = 11 * 3600; // server TOKEN_TTL is 12h

// What the server checks before it opens a session: the integrity proof and the game update.
nlohmann::json client_proof(const Attestation& proof) {
    return {{"version", proof.version}, {"code", proof.code}, {"revision", RBREW_GAME_REVISION}};
}

struct RelayState {
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
};

std::mutex s_mu;
RelayState* s_relay = nullptr;
std::thread s_login_thread;
LoginStatus s_status;

fs::path env_path(const char* name, const fs::path& fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    return fs::path(std::u8string(value, value + std::char_traits<char>::length(value)));
}

fs::path default_integrity_root() {
#ifdef RBREW_DEFAULT_INTEGRITY_ROOT
    const char* value = RBREW_DEFAULT_INTEGRITY_ROOT;
    if (value && *value) return fs::path(std::u8string(value, value + std::char_traits<char>::length(value)));
#endif
    return {};
}

std::string default_app_version() {
#ifdef RBREW_APP_VERSION
    const char* value = RBREW_APP_VERSION;
    if (value && *value) return value;
#endif
    return {};
}

Attestation read_integrity_proof(const fs::path& game_dir) {
    return attest(resolve_integrity_root(game_dir, default_integrity_root()), default_app_version());
}

std::string trim_str(std::string s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    const size_t e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

bool allowed_register_url(const std::string& url) {
    static const std::regex kAllowed(
        R"(^(https://[A-Za-z0-9.\-]+(:\d{1,5})?(/\S*)?|http://(127\.0\.0\.1|localhost)(:\d{1,5})?(/\S*)?)$)");
    return std::regex_match(url, kAllowed);
}

bool valid_ipv4(const std::string& s) {
    static const std::regex kIp(R"(^\d{1,3}(\.\d{1,3}){3}$)");
    if (!std::regex_match(s, kIp)) return false;
    int a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(s.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
    return a <= 255 && b <= 255 && c <= 255 && d <= 255;
}

bool marker_matches(const fs::path& game_dir, const std::string& register_url) {
    if (register_url.empty()) return false;
    std::ifstream in(game_dir / "online_registered.txt", std::ios::binary);
    std::string line;
    return in && std::getline(in, line) && trim_str(line) == trim_str(register_url);
}

void refresh_status_files_locked(const fs::path& game_dir, const std::string& register_url) {
    std::error_code ec;
    s_status.has_identity = fs::exists(game_dir / "online_identity.ini", ec);
    if (s_status.has_identity) {
        const auto& id = rbrew_online::identity();
        if (rbrew_online::valid(id)) s_status.pid = id.pid;
    } else {
        s_status.pid = 0;
    }
    s_status.has_registered = marker_matches(game_dir, register_url);
    s_status.has_saved_token = rbrew_online::load_session_token(game_dir / "online_session.ini").has_value();
}

void stop_relay_locked() {
    if (!s_relay) return;
    {
        std::lock_guard lock(s_relay->mutex);
        s_relay->stop = true;
    }
    s_relay->wake.notify_all();
    s_relay = nullptr; // detached thread cleans up or stops on next wake
}

void start_wg_tunnel_if_enabled(Transport& transport, const std::string& register_path, const std::string& token) {
    const char* disabled = std::getenv("RBREW_WG_DISABLE");
    if (disabled && disabled[0] == '1') return;
    if (token.empty() || WgTunnel::instance().active()) return;

    std::string priv_key, pub_key;
    if (!generate_wg_keypair(priv_key, pub_key)) return;

    WgConfig wg_cfg;
    std::string wg_err;
    if (!fetch_wg_config(transport, derive_wg_path(register_path), token, pub_key, wg_cfg, wg_err)) {
        RBREW_LOG_INFO("wireguard", "%s\n", wg_err.c_str());
        return;
    }
    if (wg_cfg.private_key.empty()) wg_cfg.private_key = priv_key;
    WgTunnel::instance().start(wg_cfg);
}

void relay_thread(RelayState* state, std::string register_url, std::string server, std::string pseudo,
                  fs::path game_dir, fs::path friends_dir, std::string initial_token, bool skip_register) {
    std::string base_url, register_path;
    if (!split_url(register_url, base_url, register_path)) {
        RBREW_LOG_WARN("friends", "unusable server URL, friends stay offline\n");
        return;
    }
    const auto transport = make_http_transport(base_url, kTimeoutMs);
    if (!transport) {
        RBREW_LOG_WARN("friends", "unusable server address, friends stay offline\n");
        return;
    }
    const Attestation proof = read_integrity_proof(game_dir);
    RBREW_LOG_INFO("friends", "relay started, version %s, code %.8s\n", proof.version.c_str(), proof.code.c_str());
    const auto& id = rbrew_online::identity();

    if (!skip_register && register_path != "/") {
        std::string login_error;
        while (true) {
            if (check_server_version(*transport, derive_health_path(register_path), proof.version, login_error) &&
                ensure_registered(*transport, register_url, register_path, game_dir, id.pid, id.password,
                                  login_error)) {
                break;
            }
            RBREW_LOG_WARN("online", "%s\n", login_error.c_str());
            write_login_state(friends_dir, login_error);
            std::unique_lock lock(state->mutex);
            if (state->wake.wait_for(lock, kInterval, [state] { return state->stop; })) return;
        }
    }

    FriendsRelay relay(*transport, id.pid, id.password, pseudo, game_dir, friends_dir, client_proof(proof));
    if (!initial_token.empty()) {
        relay.set_initial_token(initial_token);
        start_wg_tunnel_if_enabled(*transport, register_path, initial_token);
    }
    relay.set_token_callback([game_dir, server, register_url, pseudo, pid = id.pid, &transport = *transport,
                              register_path](const std::string& tok) {
        rbrew_online::save_session_token(
            rbrew_online::SessionToken{tok, server, register_url, pid, pseudo, int64_t(std::time(nullptr))},
            game_dir / "online_session.ini");
        start_wg_tunnel_if_enabled(transport, register_path, tok);
    });

    if (initial_token.empty()) relay.claim_pseudo();
    std::string shown;
    for (;;) {
        relay.step();
        if (relay.message() != shown) {
            shown = relay.message();
            if (shown.empty())
                RBREW_LOG_INFO("friends", "connected\n");
            else
                RBREW_LOG_WARN("friends", "%s\n", shown.c_str());
        }
        std::unique_lock lock(state->mutex);
        if (state->wake.wait_for(lock, kInterval, [state] { return state->stop; })) return;
    }
}

void spawn_relay_locked(const std::string& register_url, const std::string& server, const std::string& pseudo,
                        const fs::path& game_dir, const fs::path& friends_dir, const std::string& initial_token,
                        bool skip_register) {
    stop_relay_locked();
    auto* state = new RelayState();
    s_relay = state;
    std::thread(relay_thread, state, register_url, server, pseudo, game_dir, friends_dir, initial_token, skip_register)
        .detach();
}

bool has_reusable_session(const rbrew_online::Config& cfg, const std::string& register_url, const std::string& pseudo,
                          const fs::path& game_dir, std::string& token_out) {
    token_out.clear();
    if (!cfg.online || register_url.empty() || !is_valid_pseudo(pseudo)) return false;
    if (!marker_matches(game_dir, register_url)) return false;
    const auto saved = rbrew_online::load_session_token(game_dir / "online_session.ini");
    if (!saved) return false;
    const auto& id = rbrew_online::identity();
    const std::string server = cfg.host + ":" + std::to_string(cfg.port);
    if (saved->pid != id.pid || saved->server != server || saved->register_url != register_url ||
        saved->pseudo != pseudo)
        return false;
    const int64_t now = int64_t(std::time(nullptr));
    if (saved->saved_at <= 0 || now - saved->saved_at > kMaxSavedTokenAgeSec) return false;
    token_out = saved->token;
    return true;
}

} // namespace

void online_boot() {
    const auto cfg = rbrew_online::config();
    if (!cfg.online) return;
    const fs::path game_dir = rbrew_online_cfg::settings_path().parent_path();

    for (const auto& problem : write_boss_files(env_path("RBREW_BOSS_DIR", game_dir / "boss"), unix_micros_now()))
        RBREW_LOG_WARN("boss", "%s\n", problem.c_str());

    const std::string url = rbrew_online::resolve_register_url(cfg);
    if (url.empty() || !allowed_register_url(url)) {
        if (!url.empty()) RBREW_LOG_WARN("friends", "online_register must be https://host or a local http address\n");
        return;
    }

    const fs::path friends_dir = rbrew_online::friends_dir();
    if (friends_dir.empty()) return;

    const std::string server = cfg.host + ":" + std::to_string(cfg.port);
    const std::string pseudo = rbrew_online::pseudo();
    std::string saved_token;

    std::lock_guard lock(s_mu);
    refresh_status_files_locked(game_dir, url);
    // RBREW_SOCIAL_URL is passed by test harnesses (non-interactive) -> start relay immediately.
    if (const char* env_social = std::getenv("RBREW_SOCIAL_URL"); env_social && *env_social) {
        s_status.logged_in = true;
        s_status.message = "Connected";
        spawn_relay_locked(url, server, pseudo, game_dir, friends_dir, "", false);
        return;
    }
    if (has_reusable_session(cfg, url, pseudo, game_dir, saved_token)) {
        s_status.logged_in = true;
        s_status.has_saved_token = true;
        s_status.message = "Signed in with saved session token";
        RBREW_LOG_INFO("online", "reusing saved session token for pid %08x (%s)\n", s_status.pid, pseudo.c_str());
        spawn_relay_locked(url, server, pseudo, game_dir, friends_dir, saved_token, true);
    }
}

void online_shutdown() {
    WgTunnel::instance().stop();
    {
        std::lock_guard lock(s_mu);
        stop_relay_locked();
    }
    if (s_login_thread.joinable()) s_login_thread.join();
}

bool needs_startup_login() {
    if (rbrew_online::is_session_offline()) return false;
    const std::string raw_server = rbrew_online_cfg::resolved("RBREW_ONLINE_SERVER", "online_server", "");
    if (raw_server.empty()) return false;
    std::lock_guard lock(s_mu);
    return !s_status.logged_in;
}

LoginFormValues current_login_form() {
    LoginFormValues form;
    form.pseudo = rbrew_online_cfg::resolved("RBREW_PSEUDO", "pseudo", "");
    if (form.pseudo.empty()) form.pseudo = rbrew_online_cfg::resolved("RBREW_PLAYER_NAME", "player_name", "");
    if (form.pseudo == "default") form.pseudo.clear();
    form.server =
        rbrew_online_cfg::resolved("RBREW_ONLINE_SERVER", "online_server", std::string(rbrew_online::kPublicServer));

    rbrew_online::Config cfg;
    cfg.online = rbrew_online::parse_server(form.server, cfg.host, cfg.port);
    form.register_url = rbrew_online::resolve_register_url(cfg);
    if (form.register_url.empty()) form.register_url = std::string(rbrew_online::kPublicRegister);
    form.nncs1 = rbrew_online_cfg::resolved("RBREW_ONLINE_NNCS1", "online_nncs1", "");
    form.nncs2 = rbrew_online_cfg::resolved("RBREW_ONLINE_NNCS2", "online_nncs2", "");
    if (form.nncs1.empty() || form.nncs2.empty()) {
        if (cfg.host == "173.249.46.125" || form.server == rbrew_online::kPublicServer) {
            if (form.nncs1.empty()) form.nncs1 = std::string(rbrew_online::kPublicNncs1);
            if (form.nncs2.empty()) form.nncs2 = std::string(rbrew_online::kPublicNncs2);
        } else if (cfg.host == "127.0.0.1") {
            if (form.nncs1.empty()) form.nncs1 = "127.0.0.1";
            if (form.nncs2.empty()) form.nncs2 = "127.0.0.2";
        } else if (!cfg.host.empty()) {
            if (form.nncs1.empty()) form.nncs1 = cfg.host;
            if (form.nncs2.empty()) form.nncs2 = cfg.host;
        }
    }
    return form;
}

LoginStatus login_status() {
    std::lock_guard lock(s_mu);
    const fs::path game_dir = rbrew_online_cfg::settings_path().parent_path();
    const auto cfg = rbrew_online::config();
    refresh_status_files_locked(game_dir, rbrew_online::resolve_register_url(cfg));
    return s_status;
}

void start_interactive_login(const LoginFormValues& raw_form) {
    if (s_login_thread.joinable()) s_login_thread.join();

    LoginFormValues form{trim_str(raw_form.pseudo), trim_str(raw_form.server), trim_str(raw_form.register_url),
                         trim_str(raw_form.nncs1), trim_str(raw_form.nncs2)};

    {
        std::lock_guard lock(s_mu);
        if (s_status.connecting) return;
        s_status.connecting = true;
        s_status.has_error = false;
        s_status.detail.clear();
        s_status.message = "Connecting to server...";
    }

    s_login_thread = std::thread([form]() {
        auto fail = [](const std::string& msg, const std::string& detail = "") {
            std::lock_guard lock(s_mu);
            s_status.connecting = false;
            s_status.has_error = true;
            s_status.message = msg;
            s_status.detail = detail;
        };

        if (!is_valid_pseudo(form.pseudo)) {
            fail("Pseudo must be 2 to 10 characters (letters, digits, space, _ - .).");
            return;
        }
        std::string host;
        uint16_t port = 0;
        if (!rbrew_online::parse_server(form.server, host, port)) {
            fail("Server must be IPv4:port (for example 173.249.46.125:60000).");
            return;
        }
        if (!allowed_register_url(form.register_url)) {
            fail("Register URL must start with https:// (or http://127.0.0.1 / localhost).");
            return;
        }
        if ((!form.nncs1.empty() && !valid_ipv4(form.nncs1)) || (!form.nncs2.empty() && !valid_ipv4(form.nncs2))) {
            fail("NNCS1 and NNCS2 must be valid IPv4 addresses.");
            return;
        }

        std::string base_url, register_path;
        if (!split_url(form.register_url, base_url, register_path)) {
            fail("Invalid Register URL.");
            return;
        }
        const auto transport = make_http_transport(base_url, kTimeoutMs);
        if (!transport) {
            fail("Cannot initialize HTTP transport.");
            return;
        }

        const fs::path settings_file = rbrew_online_cfg::settings_path();
        const fs::path game_dir = settings_file.parent_path();
        rbrew_online::set_session_offline(false);

        const auto& id = rbrew_online::identity();
        if (!rbrew_online::valid(id)) {
            fail("Could not create or read online_identity.ini.");
            return;
        }

        const Attestation proof = read_integrity_proof(game_dir);
        std::string token, err, detail;
        if (!perform_account_login(*transport, form.register_url, register_path, game_dir, id.pid, id.password,
                                   form.pseudo, proof.version, client_proof(proof), token, err, detail)) {
            fail(err, detail);
            return;
        }

        rbrew_online_cfg::Values updates{{"online_server", form.server},
                                         {"online_register", form.register_url},
                                         {"pseudo", form.pseudo},
                                         {"player_name", form.pseudo}};
        if (!form.nncs1.empty()) updates["online_nncs1"] = form.nncs1;
        if (!form.nncs2.empty()) updates["online_nncs2"] = form.nncs2;
        rbrew_online_cfg::update_settings(updates, settings_file);
        rbrew_online::config_changed();

        rbrew_online::save_session_token(rbrew_online::SessionToken{token, form.server, form.register_url, id.pid,
                                                                    form.pseudo, int64_t(std::time(nullptr))},
                                         game_dir / "online_session.ini");

        for (const auto& problem : write_boss_files(env_path("RBREW_BOSS_DIR", game_dir / "boss"), unix_micros_now()))
            RBREW_LOG_WARN("boss", "%s\n", problem.c_str());

        const fs::path friends_dir = rbrew_online::friends_dir();
        {
            std::lock_guard lock(s_mu);
            s_status.connecting = false;
            s_status.logged_in = true;
            s_status.has_error = false;
            s_status.message = "Connected & session token saved!";
            s_status.detail.clear();
            refresh_status_files_locked(game_dir, form.register_url);
            if (!friends_dir.empty()) {
                spawn_relay_locked(form.register_url, form.server, form.pseudo, game_dir, friends_dir, token, true);
            }
        }
    });
}

void clear_saved_tokens() {
    const fs::path game_dir = rbrew_online_cfg::settings_path().parent_path();
    rbrew_online::clear_session_token(game_dir / "online_session.ini");
    std::error_code ec;
    fs::remove(game_dir / "online_registered.txt", ec);
    std::lock_guard lock(s_mu);
    stop_relay_locked();
    s_status.logged_in = false;
    s_status.has_saved_token = false;
    s_status.has_registered = false;
    s_status.has_error = false;
    s_status.message = "Saved tokens cleared.";
}

} // namespace rbrew_native

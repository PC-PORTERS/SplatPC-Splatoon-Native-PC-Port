#include "online_boot.h"
#include "boss_schedule.h"
#include "friends_relay.h"
#include "integrity_proof.h"
#include "social_http.h"

#include "online_identity.h"
#include "rebrewu/logger.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <regex>
#include <thread>

namespace fs = std::filesystem;

namespace rbrew_native {
namespace {

constexpr auto kInterval = std::chrono::seconds(5);
constexpr int kTimeoutMs = 6000;

// Shared with the detached relay thread; never freed, so the thread can outlive static destruction at exit.
struct RelayState {
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
};
RelayState* s_state = nullptr;

fs::path env_path(const wchar_t* name, const fs::path& fallback) {
    const wchar_t* value = _wgetenv(name);
    return value && *value ? fs::path(value) : fallback;
}

void relay_thread(RelayState* state, std::string url, fs::path game_dir, fs::path friends_dir) {
    const auto transport = make_winhttp_transport(url, kTimeoutMs);
    if (!transport) {
        RBREW_LOG_WARN("friends", "unusable server address, friends stay offline\n");
        return;
    }
    // The proof is computed once, from the files of this install (same as scripts/integrity.py): the folder
    // above Game, or RBREW_INTEGRITY_ROOT for test launchers that run copies of the game elsewhere.
    const Attestation proof = attest(env_path(L"RBREW_INTEGRITY_ROOT", game_dir.parent_path()));
    RBREW_LOG_INFO("friends", "relay started, version %s, code %.8s\n", proof.version.c_str(), proof.code.c_str());
    const auto& id = rbrew_online::identity();
    // settings.ini only: the launcher's RBREW_PSEUDO=Player fallback is a display name, not a pseudo to claim.
    const std::string pseudo = rbrew_online_cfg::resolved(nullptr, "pseudo", "");
    FriendsRelay relay(*transport, id.pid, id.password, pseudo, game_dir, friends_dir,
                       {{"version", proof.version}, {"code", proof.code}});
    relay.claim_pseudo();
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

} // namespace

void online_boot() {
    if (!rbrew_online::config().online) return;
    const fs::path game_dir = rbrew_online_cfg::settings_path().parent_path();

    for (const auto& problem : write_boss_files(env_path(L"RBREW_BOSS_DIR", game_dir / "boss"), unix_micros_now()))
        RBREW_LOG_WARN("boss", "%s\n", problem.c_str());

    const char* url = std::getenv("RBREW_SOCIAL_URL");
    if (!url || !*url) return;
    static const std::regex kAllowed(
        R"(^(https://[A-Za-z0-9.\-]+(:\d{1,5})?|http://(127\.0\.0\.1|localhost)(:\d{1,5})?)$)");
    if (!std::regex_match(url, kAllowed)) {
        RBREW_LOG_WARN("friends", "RBREW_SOCIAL_URL must be https://host or a local http address\n");
        return;
    }
    if (s_state) return;
    s_state = new RelayState();
    std::thread(relay_thread, s_state, std::string(url), game_dir, env_path(L"RBREW_FRIENDS_DIR", game_dir / "friends"))
        .detach();
}

void online_shutdown() {
    if (!s_state) return;
    {
        std::lock_guard lock(s_state->mutex);
        s_state->stop = true;
    }
    s_state->wake.notify_all();
}

} // namespace rbrew_native

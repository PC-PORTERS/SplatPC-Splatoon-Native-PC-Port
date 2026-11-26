// online_boot.h - native online startup, interactive ImGui login & saved tokens,
// SpotPass files (Game/boss), and the friends relay (Windows & Linux).
#pragma once
#include <cstdint>
#include <string>

namespace rbrew_native {

struct LoginFormValues {
    std::string pseudo;
    std::string server;
    std::string register_url;
    std::string nncs1;
    std::string nncs2;
};

struct LoginStatus {
    bool connecting = false;
    bool logged_in = false;
    bool has_error = false;
    std::string message;
    std::string detail;
    uint32_t pid = 0;
    bool has_identity = false;
    bool has_registered = false;
    bool has_saved_token = false;
};

void online_boot();     // at start-up, before the game reads SpotPass data; does nothing offline
void online_shutdown(); // stops the relay thread (it never blocks the exit)

// True when online_server is configured and no valid saved session token is active yet.
bool needs_startup_login();

LoginFormValues current_login_form();
LoginStatus login_status();
void start_interactive_login(const LoginFormValues& form);
void clear_saved_tokens();

// Account server mode (online_account): asks <account_url> for a NEX token for this PC account (the server
// registers it on first use). Blocking, a few seconds at most. False with error_out set on failure.
struct NexToken {
    std::string host, nex_password, token;
    uint16_t port = 0;
};
bool fetch_account_nex_token(const std::string& account_url, uint32_t game_server_id, uint32_t pid,
                             const std::string& password, NexToken& out, std::string& error_out);

} // namespace rbrew_native

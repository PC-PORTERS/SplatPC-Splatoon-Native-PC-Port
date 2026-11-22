#include "discord_rpc.h"
#include "native_control_defaults.h"
#include "rebrewu/logger.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace rbrew {

#if defined(_WIN32)

#pragma pack(push, 1)
struct IpcHeader {
    uint32_t opcode;
    uint32_t length;
};
#pragma pack(pop)

// No built-in application ID: a fallback to another developer's Discord
// application would show this port as that app to everyone. Rich Presence
// runs only with the player's own ID (RBREW_DISCORD_APP_ID or
// discord_app_id.txt next to Splatoon.exe).
static const char* kDefaultWorkingClientId = "";

static std::atomic<bool> s_running{false};
static std::thread s_worker;
static std::mutex s_mutex;
static std::condition_variable s_cv;
static std::string s_state = "Title Screen";
static std::string s_details = "In Menus";
static std::string s_large_image = "default";
static std::string s_large_text = "Splatoon Native PC Port";
static bool s_dirty = true;
static int64_t s_start_time = 0;
static std::string s_configured_client_id;

static std::string escape_json(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        if (c == '"')
            out += "\\\"";
        else if (c == '\\')
            out += "\\\\";
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += "\\t";
        else
            out += c;
    }
    return out;
}

static std::string resolve_client_id(const char* explicit_id) {
    if (explicit_id && *explicit_id) return explicit_id;

    if (const char* env_id = getenv("RBREW_DISCORD_APP_ID")) {
        if (*env_id) return env_id;
    }

    // Try reading discord_app_id.txt from the working directory
    std::ifstream file("discord_app_id.txt");
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            while (!line.empty() &&
                   (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(line.begin());
            if (line.empty() || line[0] == '#' || (line.size() >= 2 && line[0] == '/' && line[1] == '/')) continue;
            if (line != "390198031590785024") {
                return line;
            }
        }
    }

    return kDefaultWorkingClientId; // empty: Rich Presence stays off
}

static HANDLE open_discord_pipe() {
    for (int i = 0; i < 10; ++i) {
        char pipe_name[64];
        snprintf(pipe_name, sizeof(pipe_name), "\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE pipe = CreateFileA(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_BYTE;
            SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
            return pipe;
        }
    }
    return INVALID_HANDLE_VALUE;
}

static bool pipe_write_all(HANDLE pipe, const void* data, uint32_t bytes) {
    const char* ptr = static_cast<const char*>(data);
    DWORD remaining = bytes;
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(pipe, ptr, remaining, &written, nullptr) || written == 0) {
            return false;
        }
        remaining -= written;
        ptr += written;
    }
    return true;
}

static bool pipe_read_all(HANDLE pipe, void* data, uint32_t bytes, unsigned timeout_ms = 2000) {
    char* ptr = static_cast<char*>(data);
    DWORD remaining = bytes;
    auto start = std::chrono::steady_clock::now();
    while (remaining > 0) {
        DWORD bytes_avail = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &bytes_avail, nullptr)) {
            return false;
        }
        if (bytes_avail > 0) {
            DWORD to_read = (std::min)(remaining, bytes_avail);
            DWORD bytes_read = 0;
            if (!ReadFile(pipe, ptr, to_read, &bytes_read, nullptr) || bytes_read == 0) {
                return false;
            }
            remaining -= bytes_read;
            ptr += bytes_read;
        } else {
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            if (elapsed > timeout_ms) return false;
            Sleep(20);
        }
    }
    return true;
}

static bool send_handshake(HANDLE pipe, const std::string& client_id) {
    std::string payload = "{\"v\":1,\"client_id\":\"" + client_id + "\"}";
    IpcHeader hdr{0, static_cast<uint32_t>(payload.size())};
    if (!pipe_write_all(pipe, &hdr, sizeof(hdr))) return false;
    if (!pipe_write_all(pipe, payload.data(), static_cast<uint32_t>(payload.size()))) return false;

    // Read Handshake response (opcode 1, FRAME)
    IpcHeader resp_hdr{};
    if (!pipe_read_all(pipe, &resp_hdr, sizeof(resp_hdr), 3000)) return false;
    if (resp_hdr.opcode != 1) {
        if (resp_hdr.length > 0 && resp_hdr.length < 4096) {
            std::vector<char> buffer(resp_hdr.length + 1, 0);
            pipe_read_all(pipe, buffer.data(), resp_hdr.length, 1000);
            RBREW_LOG_ERROR("Discord RPC", "Handshake error (opcode %u): %s\n", resp_hdr.opcode, buffer.data());
        }
        return false;
    }
    if (resp_hdr.length > 0) {
        std::vector<char> buffer(resp_hdr.length + 1, 0);
        if (!pipe_read_all(pipe, buffer.data(), resp_hdr.length, 3000)) return false;
        if (strstr(buffer.data(), "DISPATCH") == nullptr && strstr(buffer.data(), "READY") == nullptr) {
            RBREW_LOG_INFO("Discord RPC", "Unexpected handshake response: %s\n", buffer.data());
            return false;
        }
    }
    return true;
}

static bool send_activity(HANDLE pipe, uint32_t nonce, const std::string& state, const std::string& details,
                          const std::string& large_image, const std::string& large_text, int64_t start_time) {
    const DWORD pid = GetCurrentProcessId();
    std::string payload = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid) +
                          ",\"activity\":{\"state\":\"" + escape_json(state) + "\",\"details\":\"" +
                          escape_json(details) + "\",\"timestamps\":{\"start\":" + std::to_string(start_time) + "}";

    if (!large_image.empty()) {
        payload += ",\"assets\":{\"large_image\":\"" + escape_json(large_image) + "\"";
        if (!large_text.empty()) {
            payload += ",\"large_text\":\"" + escape_json(large_text) + "\"";
        }
        payload += "}";
    }

    payload += "}},\"nonce\":\"" + std::to_string(nonce) + "\"}";

    IpcHeader hdr{1, static_cast<uint32_t>(payload.size())};
    if (!pipe_write_all(pipe, &hdr, sizeof(hdr))) return false;
    if (!pipe_write_all(pipe, payload.data(), static_cast<uint32_t>(payload.size()))) return false;

    // Consume response frame so the pipe buffer doesn't overflow
    IpcHeader resp_hdr{};
    if (pipe_read_all(pipe, &resp_hdr, sizeof(resp_hdr), 1500)) {
        if (resp_hdr.length > 0 && resp_hdr.length < (1024 * 1024)) {
            std::vector<char> buffer(resp_hdr.length);
            pipe_read_all(pipe, buffer.data(), resp_hdr.length, 1500);
        }
    }
    return true;
}

static void discord_worker() {
    uint32_t nonce = 1;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::string current_id = s_configured_client_id;
    bool fallback_tried = false;

    while (s_running.load()) {
        if (pipe == INVALID_HANDLE_VALUE) {
            pipe = open_discord_pipe();
            if (pipe != INVALID_HANDLE_VALUE) {
                if (!send_handshake(pipe, current_id)) {
                    CloseHandle(pipe);
                    pipe = INVALID_HANDLE_VALUE;
                    if (!fallback_tried && *kDefaultWorkingClientId && current_id != kDefaultWorkingClientId) {
                        RBREW_LOG_ERROR("Discord RPC", "Configured ID %s failed handshake, falling back to %s\n",
                                        current_id.c_str(), kDefaultWorkingClientId);
                        current_id = kDefaultWorkingClientId;
                        fallback_tried = true;
                        continue;
                    }
                } else {
                    RBREW_LOG_INFO("Discord RPC", "Connected and active on Discord! (Client ID: %s)\n",
                                   current_id.c_str());
                    std::lock_guard<std::mutex> lock(s_mutex);
                    s_dirty = true;
                }
            }
        }

        if (pipe != INVALID_HANDLE_VALUE) {
            std::string cur_state, cur_details, cur_img, cur_text;
            bool need_send = false;
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                if (s_dirty) {
                    cur_state = s_state;
                    cur_details = s_details;
                    cur_img = s_large_image;
                    cur_text = s_large_text;
                    s_dirty = false;
                    need_send = true;
                }
            }

            if (need_send) {
                if (!send_activity(pipe, nonce++, cur_state, cur_details, cur_img, cur_text, s_start_time)) {
                    CloseHandle(pipe);
                    pipe = INVALID_HANDLE_VALUE;
                }
            }
        }

        // Wait up to 15 seconds or until signaled
        std::unique_lock<std::mutex> lock(s_mutex);
        s_cv.wait_for(lock, std::chrono::seconds(pipe != INVALID_HANDLE_VALUE ? 15 : 5),
                      [&] { return !s_running.load() || s_dirty; });
    }

    if (pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE;
    }
}

void discord_rpc_init(const char* custom_client_id) {
    if (s_running.load()) return;

    s_configured_client_id = resolve_client_id(custom_client_id);
    if (s_configured_client_id.empty()) return;
    s_start_time =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    s_running.store(true);
    s_dirty = true;
    s_worker = std::thread(discord_worker);
}

void discord_rpc_update_activity(const char* state, const char* details, const char* large_image,
                                 const char* large_text) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (state && *state) s_state = state;
    if (details && *details) s_details = details;
    if (large_image && *large_image) s_large_image = large_image;
    if (large_text && *large_text) s_large_text = large_text;
    s_dirty = true;
    s_cv.notify_one();
}

void discord_rpc_poll_game_state(uint8_t* mem, uint32_t frame_count) {
    if (!s_running.load() || !mem) return;
    // Check game state periodically (~1 second at 60 FPS)
    if (frame_count % 60 != 0) return;

    int scene_type = rbrew_control_defaults::active_scene_type(mem);
    static int s_last_scene_type = -999;
    if (scene_type == s_last_scene_type) return;
    s_last_scene_type = scene_type;

    const char* state = "Main Menu";
    const char* details = "Title Screen";
    const char* asset = "default";

    switch (scene_type) {
    case 1:
        state = "Octo Valley";
        details = "World Map";
        break;
    case 2:
        state = "Octo Valley";
        details = "Single Player Mission";
        break;
    case 3:
        state = "Inkopolis Plaza";
        details = "Exploring Inkopolis";
        break;
    case 4:
        state = "Battle Lobby";
        details = "Preparing for Match";
        asset = "turf_war";
        break;
    case 5:
        state = "Tutorial";
        details = "Welcome to Inkopolis";
        break;
    case 6:
        state = "Tutorial";
        details = "Learning the Controls";
        break;
    case 7:
        state = "Online Battle";
        details = "Turf War";
        asset = "turf_war";
        break;
    case 8:
        state = "Battle Dojo";
        details = "Local 2-Player Battle";
        break;
    case 9:
        state = "Booyah Base";
        details = "Shopping for Gear";
        break;
    case 10:
        state = "Shooting Range";
        details = "Testing Weapons";
        break;
    case 11:
        state = "Customize Screen";
        details = "Changing Gear & Weapons";
        break;
    default:
        if (frame_count < 300) {
            state = "Booting Game";
            details = "Loading...";
        } else {
            state = "Title Screen";
            details = "In Menus";
        }
        break;
    }

    discord_rpc_update_activity(state, details, asset, "Splatoon Native PC Port");
}

void discord_rpc_shutdown() {
    if (!s_running.exchange(false)) return;

    s_cv.notify_all();
    if (s_worker.joinable()) {
        s_worker.join();
    }
}

#else

// Non-Windows stubs
void discord_rpc_init(const char*) {}
void discord_rpc_update_activity(const char*, const char*, const char*, const char*) {}
void discord_rpc_poll_game_state(uint8_t*, uint32_t) {}
void discord_rpc_shutdown() {}

#endif

} // namespace rbrew

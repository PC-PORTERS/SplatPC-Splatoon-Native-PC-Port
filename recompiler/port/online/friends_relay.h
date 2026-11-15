// friends_relay.h - links the game's friend list to the server, inside the game (scripts/friends_relay.py does the
// same as a separate Python program, still used on Linux).
//
// The game reads friends from <friends dir> (one <pid>.txt per ONLINE friend: pid, name, mode) and writes its own
// <pid>.txt (its game mode). Each cycle sends that file to the server (the heartbeat) with the friends the player
// added, writes back one file per friend online who added this player too, and answers the F4 window through files
// in the same folder:
//     add.cmd / remove.cmd / accept.cmd / decline.cmd  ->  add.res / remove.res / accept.res / decline.res
//     state.res: "friend|<pid>|<pseudo>|<online 0/1>", "request|<pid>|<pseudo>" and "msg|<text>" lines, and
//                "detail|<the server's own words>" after a refusal of the game files (for the launcher's log)
// The player's list stays in <game dir>/friends.txt ("<pid> <pseudo>" per line); the server stores no list.
#pragma once
#include "social_http.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace rbrew_native {

class FriendsRelay {
public:
    FriendsRelay(Transport& transport, uint32_t pid, std::string password, std::string pseudo,
                 std::filesystem::path game_dir, std::filesystem::path friends_dir, nlohmann::json client);

    void claim_pseudo(); // a pseudo taken by another account shows in state.res
    void step();         // one cycle; never throws

    const std::string& message() const { return message_; }
    // After the server refuses the game files (426) the next sign-in waits this long: the files do not change
    // while the game runs.
    void set_refusal_pause(std::chrono::steady_clock::duration pause) { refusal_pause_ = pause; }

private:
    using Friends = std::vector<std::pair<std::string, std::string>>; // (pid hex, pseudo), in file order

    Reply call(const std::string& route, nlohmann::json body);
    bool ensure_session();
    Friends load_list() const;
    void save_list(const Friends& friends) const;
    bool take_command(const char* name, std::string& text) const;
    void answer(const char* name, const std::string& code, const std::string& pseudo, const std::string& text) const;
    void handle_add(const std::string& text, Friends& friends);
    std::pair<std::string, std::string> request_friend(const std::string& pid);
    void handle_remove(const std::string& text, Friends& friends);
    void handle_answer(const std::string& text, Friends& friends, bool accept);
    void read_requests(const nlohmann::json& answer, const Friends& friends);
    void cycle();
    void write_friend_files();

    Transport& transport_;
    uint32_t pid_;
    std::string password_, pseudo_;
    std::filesystem::path dir_, list_path_;
    nlohmann::json client_; // the integrity proof sent with each sign-in
    std::string token_, message_, detail_;
    std::chrono::steady_clock::duration refusal_pause_ = std::chrono::minutes(5);
    std::chrono::steady_clock::time_point refused_until_{};
    std::map<std::string, std::pair<std::string, std::string>> online_; // pid -> (name, mode), last good heartbeat
    Friends requests_;                                                  // friend requests received, not answered yet
};

// Text helpers shared with the tests.
std::string sanitize_name(const std::string& utf8); // characters outside [A-Za-z0-9 _.-] -> '?', 10 at most

} // namespace rbrew_native

#include "friends_relay.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <process.h>
#include <regex>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace rbrew_native {
namespace {

// The relay thread can still run while the program ends: these are never destroyed.
const std::string& kZeroMode = *new std::string(88, '0');
const std::regex& kPid = *new std::regex("[0-9a-f]{8}");
const std::regex& kMode = *new std::regex("[0-9a-f]{88}");
const std::regex& kPseudo = *new std::regex("[A-Za-z0-9 _.\\-]{2,10}");
constexpr size_t kMaxFriends = 100;

std::string strip(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n\v\f"), e = s.find_last_not_of(" \t\r\n\v\f");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// state.res is read line by line: text from the server never brings a line break or a '|' of its own.
std::string one_line(std::string s) {
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 0x20 || c == '|') c = ' ';
    return s;
}

bool is_pid(const std::string& s) {
    return std::regex_match(s, kPid);
}

// Python str() of a JSON value.
std::string text_of(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return "None";
    if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
    return v.dump();
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::map<std::string, std::string> read_kv(const fs::path& path) {
    std::map<std::string, std::string> values;
    const std::string text = read_text(path);
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq != std::string::npos) values[lower(strip(line.substr(0, eq)))] = strip(line.substr(eq + 1));
        start = end + 1;
    }
    return values;
}

void write_atomic(const fs::path& path, const std::string& text) {
    fs::path temporary = path;
    temporary += "." + std::to_string(_getpid()) + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << text;
    }
    std::error_code ec;
    fs::rename(temporary, path, ec);
    if (ec) fs::remove(temporary, ec);
}

} // namespace

std::string sanitize_name(const std::string& utf8) {
    std::string out;
    for (size_t i = 0; i < utf8.size() && out.size() < 10;) {
        const unsigned char c = utf8[i];
        size_t length = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + length > utf8.size()) length = 1;
        const bool allowed = length == 1 && (std::isalnum(c) || c == ' ' || c == '_' || c == '.' || c == '-');
        out += allowed ? char(c) : '?'; // one '?' per character, as Python's re.sub on str
        i += length;
    }
    return out;
}

FriendsRelay::FriendsRelay(Transport& transport, uint32_t pid, std::string password, std::string pseudo,
                           fs::path game_dir, fs::path friends_dir, json client)
    : transport_(transport), pid_(pid), password_(std::move(password)), pseudo_(std::move(pseudo)),
      dir_(std::move(friends_dir)), list_path_(game_dir / "friends.txt"), client_(std::move(client)) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
}

Reply FriendsRelay::call(const std::string& route, json body) {
    return transport_.post(route, body);
}

static std::string pid_text(uint32_t pid) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08x", pid);
    return buf;
}

bool FriendsRelay::ensure_session() {
    if (!token_.empty()) return true;
    if (std::chrono::steady_clock::now() < refused_until_) return false; // the refusal stays shown
    detail_.clear();
    const Reply r = call("session", {{"pid", pid_text(pid_)}, {"password", password_}, {"client", client_}});
    const json token = r.body.value("token", json());
    if (r.status == 200 && token.is_string() && !token.get<std::string>().empty()) {
        token_ = token.get<std::string>();
        return true;
    }
    if (r.status == 426) { // the server only takes official, up-to-date game files
        const json error = r.body.value("error", json());
        message_ = "Game files not recognised by the server. Update the game.";
        detail_ = one_line(error.is_string() ? error.get<std::string>() : "").substr(0, 200);
        refused_until_ = std::chrono::steady_clock::now() + refusal_pause_;
    } else {
        message_ = r.status == 0 ? "server unreachable" : "sign-in refused";
    }
    return false;
}

void FriendsRelay::claim_pseudo() {
    if (pseudo_.empty()) return;
    const Reply r = call("pseudo", {{"pid", pid_text(pid_)}, {"password", password_}, {"pseudo", pseudo_}});
    if (r.status == 200) return;
    const std::string fallback = r.status == 0 ? "server unreachable" : "error " + std::to_string(r.status);
    message_ = "pseudo: " + (r.body.contains("error") ? text_of(r.body["error"]) : fallback);
}

FriendsRelay::Friends FriendsRelay::load_list() const {
    Friends friends;
    const std::string text = read_text(list_path_);
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = strip(text.substr(start, end - start));
        const size_t space = line.find(' ');
        if (space != std::string::npos && is_pid(line.substr(0, space))) {
            const std::string pid = line.substr(0, space), name = strip(line.substr(space + 1));
            auto it = std::find_if(friends.begin(), friends.end(), [&](const auto& f) { return f.first == pid; });
            if (it == friends.end())
                friends.emplace_back(pid, name);
            else
                it->second = name;
        }
        start = end + 1;
    }
    return friends;
}

void FriendsRelay::save_list(const Friends& friends) const {
    Friends sorted = friends;
    std::sort(sorted.begin(), sorted.end());
    std::string text;
    for (const auto& [pid, name] : sorted) text += pid + " " + name + "\n";
    write_atomic(list_path_, text);
}

bool FriendsRelay::take_command(const char* name, std::string& text) const {
    const fs::path path = dir_ / name;
    std::error_code ec;
    if (!fs::exists(path, ec)) return false;
    text = strip(read_text(path));
    fs::remove(path, ec);
    return true;
}

void FriendsRelay::answer(const char* name, const std::string& code, const std::string& pseudo,
                          const std::string& text) const {
    write_atomic(dir_ / name, code + "|" + pseudo + "|" + text + "\n");
}

void FriendsRelay::handle_add(const std::string& text, Friends& friends) {
    const std::string pseudo = strip(text);
    if (!std::regex_match(pseudo, kPseudo))
        return answer("add.res", "invalid", pseudo, "2 to 10 characters: letters, digits, space, _ - .");
    if (!ensure_session()) return answer("add.res", "error", pseudo, message_);
    const Reply found = call("lookup", {{"token", token_}, {"pseudo", pseudo}});
    if (found.status == 401) {
        token_.clear();
        return answer("add.res", "error", pseudo, "session expired, try again");
    }
    if (found.status == 429) return answer("add.res", "error", pseudo, "too many searches, wait a moment");
    if (found.status != 200)
        return answer("add.res", "error", pseudo,
                      found.status == 0 ? "server unreachable" : "error " + std::to_string(found.status));
    const json& body = found.body;
    const auto flag = [&](const char* key) {
        return body.contains(key) && body[key].is_boolean() ? body[key].get<bool>()
                                                            : body.contains(key) && !body[key].is_null();
    };
    if (!flag("found")) return answer("add.res", "notfound", pseudo, "no player has this pseudo");
    const std::string name = text_of(body.value("pseudo", json(pseudo)));
    if (flag("self")) return answer("add.res", "self", name, "this is you");
    const std::string pid = text_of(body.value("pid", json()));
    if (std::any_of(friends.begin(), friends.end(), [&](const auto& f) { return f.first == pid; }))
        return answer("add.res", "already", name, "already in your list");
    if (friends.size() >= kMaxFriends) return answer("add.res", "full", name, "the list holds 100 friends");
    const auto [result, problem] = request_friend(pid);
    if (result == "error") return answer("add.res", "error", name, problem);
    friends.emplace_back(pid, name);
    save_list(friends);
    const std::string state = flag("online") ? "online now" : "not online now";
    const std::string follow_up = result == "sent"      ? "request sent, they must accept"
                                  : result == "friends" ? "they had asked you too: you are friends"
                                                        : "they must add you too";
    answer("add.res", "ok", name, "added (" + state + "); " + follow_up);
}

// ("sent" | "friends", "") when the server took the request, ("old", "") for a server without friend requests,
// ("error", text) when the player must not be added.
std::pair<std::string, std::string> FriendsRelay::request_friend(const std::string& pid) {
    const Reply r = call("request", {{"token", token_}, {"pid", pid}});
    const json state = r.body.value("state", json());
    if (r.status == 200 && state.is_string() && (state == "sent" || state == "friends"))
        return {state.get<std::string>(), ""};
    if (r.status == 401) {
        token_.clear();
        return {"error", "session expired, try again"};
    }
    if (r.status == 429) {
        const json error = r.body.value("error", json());
        const std::string text = error.is_string() && !error.get<std::string>().empty()
                                     ? error.get<std::string>()
                                     : "too many requests, wait a moment";
        return {"error", text.substr(0, 80)};
    }
    return {"old", ""};
}

void FriendsRelay::handle_remove(const std::string& text, Friends& friends) {
    const std::string wanted = lower(strip(text));
    for (auto it = friends.begin(); it != friends.end(); ++it) {
        if (wanted != it->first && wanted != lower(it->second)) continue;
        const auto [pid, name] = *it;
        friends.erase(it);
        save_list(friends);
        if (ensure_session()) call("cancel", {{"token", token_}, {"pid", pid}}); // withdraws a request still waiting
        return answer("remove.res", "ok", name, "removed");
    }
    answer("remove.res", "notfound", wanted, "not in your list");
}

void FriendsRelay::handle_answer(const std::string& text, Friends& friends, bool accept) {
    const char* result = accept ? "accept.res" : "decline.res";
    const std::string wanted = lower(strip(text));
    const auto match = std::find_if(requests_.begin(), requests_.end(),
                                    [&](const auto& r) { return wanted == r.first || wanted == lower(r.second); });
    if (match == requests_.end()) return answer(result, "notfound", wanted, "no such request");
    const auto [pid, name] = *match;
    const bool known = std::any_of(friends.begin(), friends.end(), [&](const auto& f) { return f.first == pid; });
    if (accept && !known && friends.size() >= kMaxFriends)
        return answer(result, "full", name, "the list holds 100 friends");
    if (!ensure_session()) return answer(result, "error", name, message_);
    const Reply r = call("answer", {{"token", token_}, {"pid", pid}, {"accept", accept}});
    if (r.status == 401) {
        token_.clear();
        return answer(result, "error", name, "session expired, try again");
    }
    if (r.status != 200 && r.status != 404)
        return answer(result, "error", name,
                      r.status == 0 ? "server unreachable" : "error " + std::to_string(r.status));
    requests_.erase(std::find(requests_.begin(), requests_.end(), std::make_pair(pid, name)));
    if (r.status == 404) return answer(result, "gone", name, "this request is gone"); // the sender withdrew it
    if (accept) {
        if (!known)
            friends.emplace_back(pid, name);
        else
            for (auto& f : friends)
                if (f.first == pid) f.second = name;
        save_list(friends);
    }
    answer(result, "ok", name, accept ? "accepted, you are friends now" : "declined");
}

// The requests received, as the server lists them; one from a player already in the list is accepted at once.
void FriendsRelay::read_requests(const json& body, const Friends& friends) {
    Friends received;
    const json requests = body.value("requests", json::array());
    if (requests.is_array())
        for (const auto& item : requests) {
            if (!item.is_object()) continue;
            const std::string pid = text_of(item.value("pid", json("")));
            if (!is_pid(pid)) continue;
            const std::string name = sanitize_name(text_of(item.value("name", json("Ami"))));
            if (std::any_of(friends.begin(), friends.end(), [&](const auto& f) { return f.first == pid; }))
                call("answer", {{"token", token_}, {"pid", pid}, {"accept", true}});
            else
                received.emplace_back(pid, name);
        }
    requests_ = received;
}

void FriendsRelay::step() {
    try {
        cycle();
    } catch (const std::exception&) { // never stop on a bad cycle
        message_ = "relay error";
    }
}

void FriendsRelay::cycle() {
    Friends friends = load_list();
    std::string command;
    if (take_command("add.cmd", command)) handle_add(command, friends);
    if (take_command("remove.cmd", command)) handle_remove(command, friends);
    if (take_command("accept.cmd", command)) handle_answer(command, friends, true);
    if (take_command("decline.cmd", command)) handle_answer(command, friends, false);
    char own[16];
    std::snprintf(own, sizeof own, "%08x.txt", pid_);
    const auto values = read_kv(dir_ / own);
    std::string mode = values.count("mode") ? values.at("mode") : "";
    if (!std::regex_match(mode, kMode)) mode.clear();
    json pids = json::array();
    for (const auto& f : friends) pids.push_back(f.first);
    bool reachable = false;
    if (ensure_session()) {
        Reply r = call("presence", {{"token", token_}, {"mode", mode}, {"friends", pids}});
        if (r.status == 401) { // the server forgot the session (a restart): sign in again at once, which
            token_.clear();    // also sends the integrity proof back before the game logs in again
            if (ensure_session()) r = call("presence", {{"token", token_}, {"mode", mode}, {"friends", pids}});
        }
        if (r.status == 200) {
            reachable = true;
            message_.clear();
            online_.clear();
            const json list = r.body.value("friends", json::array());
            if (list.is_array())
                for (const auto& item : list) {
                    if (!item.is_object()) continue;
                    const std::string pid = text_of(item.value("pid", json("")));
                    if (!is_pid(pid)) continue;
                    const json m = item.value("mode", json());
                    const std::string item_mode =
                        m.is_string() && !m.get<std::string>().empty() ? m.get<std::string>() : kZeroMode;
                    online_[pid] = {text_of(item.value("name", json("Ami"))), item_mode};
                }
            read_requests(r.body, friends);
        } else if (r.status == 401) {
            token_.clear();
            message_ = "session expired";
        } else {
            message_ = r.status == 0 ? "server unreachable" : "error " + std::to_string(r.status);
        }
    }
    if (!reachable) {
        online_.clear();
        requests_.clear();
    }
    write_friend_files();
    Friends by_name = friends;
    std::stable_sort(by_name.begin(), by_name.end(),
                     [](const auto& a, const auto& b) { return lower(a.second) < lower(b.second); });
    std::string lines;
    for (const auto& [pid, name] : by_name)
        lines += "friend|" + pid + "|" + name + "|" + (online_.count(pid) ? "1" : "0") + "\n";
    for (const auto& [pid, name] : requests_) lines += "request|" + pid + "|" + name + "\n";
    lines += "msg|" + message_ + "\n";
    if (!detail_.empty()) lines += "detail|" + detail_ + "\n";
    write_atomic(dir_ / "state.res", lines);
}

// One <pid>.txt per friend who is online and mutual; the files of everybody else are removed.
void FriendsRelay::write_friend_files() {
    char own[16];
    std::snprintf(own, sizeof own, "%08x.txt", pid_);
    for (const auto& [pid, entry] : online_)
        write_atomic(dir_ / (pid + ".txt"),
                     "pid=" + pid + "\nname=" + sanitize_name(entry.first) + "\nmode=" + entry.second + "\n");
    std::error_code ec;
    for (const auto& file : fs::directory_iterator(dir_, ec)) {
        const std::string entry = file.path().filename().string();
        if (entry.size() != 12 || entry.compare(8, 4, ".txt") != 0 || !is_pid(entry.substr(0, 8)) || entry == own)
            continue;
        if (!online_.count(entry.substr(0, 8))) fs::remove(file.path(), ec);
    }
}

} // namespace rbrew_native

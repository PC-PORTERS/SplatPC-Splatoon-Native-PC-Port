// Native online mode (recompiler/port/online): unit tests, and commands for Python cross-checks
// (tools/tests/test_online_native.py):
//     gambit_online_tests                      unit tests
//     gambit_online_tests attest <root> <out>  writes {"version", "code"} of <root> to <out>
//     gambit_online_tests boss <dir> <now>     writes the SpotPass files of <dir> for the ISO date <now>
//     gambit_online_tests relay-once <url> <game dir> <pid hex> <password>   one relay cycle over HTTP
#include "online/boss_schedule.h"
#include "online/byaml.h"
#include "online/friends_relay.h"
#include "online/hashing.h"
#include "online/integrity_proof.h"
#include "online/online_login.h"
#include "online/py_random.h"
#include "rebrewu/logger.hpp"

#include <cstdlib>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace rbrew_native;

namespace {

int s_failures = 0;

void check(bool ok, const char* what) {
    if (ok) return;
    RBREW_LOG_ERROR("online_tests", "FAIL: %s\n", what);
    ++s_failures;
}

std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

fs::path scratch(const char* name) {
    const fs::path dir = fs::temp_directory_path() / ("gambit_online_" + std::string(name));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void test_python_random() {
    // Values printed by CPython 3.12: random.Random("splatoon-rotation-0").getrandbits(32) x3, then shuffle(range(16)).
    PyRandom r("splatoon-rotation-0");
    check(r.next32() == 360046726u && r.next32() == 815164114u && r.next32() == 2351482403u,
          "MT19937 seeded like CPython");
    std::vector<int> cards(16);
    for (int i = 0; i < 16; ++i) cards[i] = i;
    PyRandom("splatoon-rotation-0").shuffle(cards);
    check(cards == std::vector<int>{7, 11, 0, 1, 10, 12, 15, 5, 9, 13, 14, 6, 4, 8, 3, 2}, "shuffle like CPython");
    std::vector<int> five{0, 1, 2, 3, 4};
    PyRandom("abc").shuffle(five);
    check(five == std::vector<int>{0, 1, 2, 3, 4}, "shuffle of 'abc' like CPython");
}

void test_byaml() {
    // byaml.dump({'A': [1, 'x', True, None, 1.5], 'B': {'c': 'x'}}): 124 bytes, sha256 003dcbf2cb3000ce...
    const nlohmann::ordered_json data = {{"A", {1, "x", true, nullptr, 1.5}}, {"B", {{"c", "x"}}}};
    const std::string blob = byaml_dump(data);
    check(blob.size() == 124 && sha256_hex(blob).rfind("003dcbf2cb3000ce", 0) == 0, "BYAML bytes like byaml.py");
}

void test_dates() {
    check(format_date(parse_date("2026-11-01T10:00:00Z")) == "2026-11-01T10:00:00Z", "UTC date round trip");
    check(format_date(parse_date("2026-11-01T10:00:00+01:00")) == "2026-11-01T09:00:00Z", "offset applied");
    check(format_date(parse_date("2015-05-28T00:00:00Z")) == "2015-05-28T00:00:00Z", "epoch");
    bool refused = false;
    try {
        parse_date("2026-02-30T00:00:00Z");
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    check(refused, "impossible date refused");
}

void test_attestation() {
    const fs::path root = scratch("attest");
    write(root / "scripts" / "a.txt", "one\r\ntwo\r\n");
    const std::string a = sha256_hex("one\ntwo\n");
    write(root / "scripts" / "integrity-manifest.txt", a + " scripts/a.txt\r\n" + a + " scripts/missing.txt\n");
    const Attestation proof = attest(root, "0.1.5");
    check(proof.version == "0.1.5", "version propagated from release tag");
    const std::string expected = a + " scripts/a.txt\n" + std::string(64, '0') + " scripts/missing.txt\n";
    check(proof.code == sha256_hex(expected), "code: CR removed, a missing file hashes to zeros");
    // Builder (standalone build-env) -> separate OutputDir: falls back to default_root when game_dir has no manifest.
    const fs::path output_game = scratch("output") / "Game";
    fs::create_directories(output_game);
    check(resolve_integrity_root(root / "Game", scratch("unused")) == root, "git clone layout uses parent dir");
    check(resolve_integrity_root(output_game, root) == root, "standalone builder layout falls back to default_root");
    check(attest(resolve_integrity_root(output_game, root), "0.1.5").code == proof.code,
          "standalone builder output proves the same code hash");
    fs::remove(root / "scripts" / "integrity-manifest.txt");
    check(attest(root).code.empty(), "no manifest, no code");
}

// Answers each route with the replies queued for it, and records what the relay sent.
class FakeTransport : public Transport {
public:
    std::map<std::string, std::deque<Reply>> replies;
    std::vector<std::pair<std::string, json>> sent;
    Reply post(const std::string& route, const json& body) override {
        sent.emplace_back(route, body);
        auto& queue = replies[route];
        if (queue.empty()) return Reply{200, json::object()};
        Reply r = queue.front();
        queue.pop_front();
        return r;
    }
    Reply request(const std::string& method, const std::string& abs_path, const json* body) override {
        sent.emplace_back(method + " " + abs_path, body ? *body : json());
        auto& queue = replies[method + " " + abs_path];
        if (queue.empty()) return Reply{200, json::object()};
        Reply r = queue.front();
        queue.pop_front();
        return r;
    }
    int count(const std::string& route) const {
        int n = 0;
        for (const auto& s : sent) n += s.first == route;
        return n;
    }
};

void test_relay() {
    const fs::path game = scratch("relay"), dir = game / "friends";
    FakeTransport server;
    const json proof = {{"version", "0.1.5"}, {"code", "ab"}};
    FriendsRelay relay(server, 0x12345678, "pw", "Ana", game, dir, proof);
    server.replies["session"] = {Reply{200, {{"token", "t1"}}}, Reply{200, {{"token", "t2"}}}};
    // The server restarted: the first presence is refused, the relay signs in again in the same cycle.
    server.replies["presence"] = {Reply{401, json::object()},
                                  Reply{200,
                                        {{"friends", {{{"pid", "0000000a"}, {"name", "Bob\xC3\xA9"}, {"mode", ""}}}},
                                         {"requests", {{{"pid", "0000000b"}, {"name", "Cy"}}}}}}};
    write(game / "friends.txt", "0000000a Bob\n");
    write(dir / "0000000c.txt", "pid=0000000c\n");
    relay.step();
    check(server.count("session") == 2 && server.count("presence") == 2, "401 reopens the session in the same cycle");
    check(server.sent.front().second["client"] == proof && server.sent.front().second["pid"] == "0x12345678",
          "sign-in carries the integrity proof");
    check(read(dir / "state.res") == "friend|0000000a|Bob|1\nrequest|0000000b|Cy\nmsg|\n",
          "state.res lists friends and requests");
    check(read(dir / "0000000a.txt") == "pid=0000000a\nname=Bob?\nmode=" + std::string(88, '0') + "\n",
          "online friend file");
    check(!fs::exists(dir / "0000000c.txt"), "file of a friend gone offline removed");

    write(dir / "accept.cmd", "cy\n");
    server.replies["answer"] = {Reply{200, json::object()}};
    server.replies["presence"] = {Reply{200, json::object()}};
    relay.step();
    check(read(dir / "accept.res") == "ok|Cy|accepted, you are friends now\n", "request accepted");
    check(read(game / "friends.txt") == "0000000a Bob\n0000000b Cy\n", "accepted friend saved");

    write(dir / "add.cmd", "Dee");
    server.replies["lookup"] = {
        Reply{200, {{"found", true}, {"pid", "0000000d"}, {"pseudo", "Dee"}, {"online", false}}}};
    server.replies["request"] = {Reply{200, {{"state", "sent"}}}};
    relay.step();
    check(read(dir / "add.res") == "ok|Dee|added (not online now); request sent, they must accept\n",
          "friend request sent");

    write(dir / "add.cmd", "x");
    relay.step();
    check(read(dir / "add.res").rfind("invalid|x|", 0) == 0, "short pseudo refused");

    FakeTransport closed;
    closed.replies["session"] = {Reply{426, {{"error", "update the game to 0.1.5"}}}};
    const fs::path old_game = scratch("relay_old"), old_dir = old_game / "friends";
    FriendsRelay old(closed, 1, "pw", "", old_game, old_dir, proof);
    old.step();
    old.step();
    check(old.message() == "Game files not recognised by the server. Update the game.", "426 shows a short reason");
    check(read(old_dir / "state.res") ==
              "msg|Game files not recognised by the server. Update the game.\ndetail|update the game to 0.1.5\n",
          "426 keeps the server's words for the launcher log");
    check(closed.count("session") == 1, "no new sign-in right after a 426");
    FakeTransport other_update;
    other_update.replies["session"] = {
        Reply{426, {{"error", "Online needs Splatoon update 288"}, {"reason", "revision"}}}};
    const fs::path v16_game = scratch("relay_v16"), v16_dir = v16_game / "friends";
    FriendsRelay v16(other_update, 1, "pw", "", v16_game, v16_dir, proof);
    v16.step();
    check(v16.message() == "Online needs Splatoon update 288", "a wrong game update shows the server's words");
    check(read(v16_dir / "state.res") == "msg|Online needs Splatoon update 288\n", "no detail for a wrong update");
    FakeTransport reopened;
    reopened.replies["session"] = {Reply{426, json::object()}, Reply{200, {{"token", "t"}}}};
    FriendsRelay back(reopened, 1, "pw", "", scratch("relay_back"), scratch("relay_back") / "friends", proof);
    back.set_refusal_pause(std::chrono::seconds(0));
    back.step();
    back.step();
    check(reopened.count("session") == 2 && back.message().empty(), "sign-in tried again after the pause");
}

void test_schedule_errors() {
    bool refused = false;
    try {
        build_schedule({{"phase_hours", 30}}, 0);
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    check(refused, "phase_hours out of range refused");
    const auto data = build_schedule(Plan::object(), parse_date("2026-11-10T13:00:00Z"));
    check(data["DateTime"] == "2026-11-10T12:00:00Z" && data["Phases"].size() == 84, "now: current 4 h phase, 14 days");
}

void test_login_and_url_split() {
    std::string base, path;
    check(split_url("https://splatoonline.duckdns.org/register", base, path) &&
              base == "https://splatoonline.duckdns.org" && path == "/register",
          "split https URL with path");
    check(split_url("http://127.0.0.1:60002", base, path) && base == "http://127.0.0.1:60002" && path == "/",
          "split http URL without path");
    check(!split_url("ftp://example.com/a", base, path), "reject non-http URL");

    check(parse_semver("0.2") == parse_semver("v0.2.0"), "0.2 equals 0.2.0");

    FakeTransport srv;
    std::string err, detail, token;
    srv.replies["GET /health"] = {
        Reply{200, {{"min_client_version", "0.1.5"}, {"integrity_mode", "enforce"}, {"latest_version", "0.2.0"}}}};
    check(!check_server_version(srv, "/health", "0.1.5", err) && err.find("0.2.0") != std::string::npos,
          "enforce mode requires latest_version");

    srv.replies["GET /health"] = {Reply{200, {{"min_client_version", "0.1.5"}, {"integrity_mode", "log"}}}};
    check(check_server_version(srv, "/health", "0.1.5", err) && err.empty(), "matching version passes");

    const fs::path game = scratch("login");
    srv.replies["POST /register"] = {Reply{409, json::object()}, Reply{200, json::object()}};
    check(!ensure_registered(srv, "https://example.com/register", "/register", game, 0x10000001, "SecretPass123456",
                             err) &&
              err.find("online_identity.ini") != std::string::npos,
          "409 conflict reports identity mismatch");
    check(ensure_registered(srv, "https://example.com/register", "/register", game, 0x10000001, "SecretPass123456",
                            err) &&
              err.empty(),
          "200 writes registration marker");
    check(read(game / "online_registered.txt") == "https://example.com/register\n", "marker file written");
    const size_t before = srv.sent.size();
    check(ensure_registered(srv, "https://example.com/register", "/register", game, 0x10000001, "SecretPass123456",
                            err) &&
              srv.sent.size() == before,
          "registered marker skips repeated POST /register");

    // Full interactive login sequence returning session token
    srv.replies["GET /health"] = {Reply{200, {{"min_client_version", "0.1.5"}}}};
    srv.replies["pseudo"] = {Reply{409, json::object()}, Reply{200, {{"pseudo", "Inkling"}}}};
    srv.replies["session"] = {Reply{200, {{"token", "tok_saved_123"}}}};
    check(!perform_account_login(srv, "https://example.com/register", "/register", game, 0x10000001, "SecretPass123456",
                                 "Taken", "0.1.5", {{"version", "0.1.5"}, {"code", "ab"}}, token, err, detail) &&
              err.find("already taken") != std::string::npos,
          "409 pseudo reports taken");
    check(perform_account_login(srv, "https://example.com/register", "/register", game, 0x10000001, "SecretPass123456",
                                "Inkling", "0.1.5", {{"version", "0.1.5"}, {"code", "ab"}}, token, err, detail) &&
              token == "tok_saved_123",
          "full login sequence acquires token");
    srv.replies["session"] = {Reply{426, {{"error", "Online needs Splatoon update 288"}, {"reason", "revision"}}}};
    srv.replies["GET /health"] = {Reply{200, {{"min_client_version", "0.1.5"}}}};
    check(!perform_account_login(srv, "https://example.com/register", "/register", game, 0x10000001, "SecretPass123456",
                                 "Inkling", "0.1.5", {{"version", "0.1.5"}, {"code", "ab"}}, token, err, detail) &&
              err == "Online needs Splatoon update 288" && detail.empty(),
          "the login screen shows the update the server needs");
}

int run_tests() {
    test_python_random();
    test_byaml();
    test_dates();
    test_attestation();
    test_relay();
    test_schedule_errors();
    test_login_and_url_split();
    if (s_failures) return 1;
    RBREW_LOG_INFO("online_tests", "all online tests passed\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "attest") {
            const Attestation proof = attest(fs::path(argv[2]));
            write(fs::path(argv[3]), json({{"version", proof.version}, {"code", proof.code}}).dump());
            return 0;
        }
        if (argc == 6 && std::string(argv[1]) == "relay-once") { // <url> <game dir> <pid hex> <password>, over HTTP
            const auto transport = make_http_transport(argv[2], 6000);
            if (!transport) return 4;
            const fs::path game(argv[3]);
            FriendsRelay relay(*transport, uint32_t(std::stoul(argv[4], nullptr, 16)), argv[5], "Ana", game,
                               game / "friends", {{"version", "0.1.5"}, {"code", "ab"}});
            relay.claim_pseudo();
            relay.step();
            return relay.message().empty() ? 0 : 5;
        }
        if (argc == 4 && std::string(argv[1]) == "boss") {
            const auto problems = write_boss_files(fs::path(argv[2]), parse_date(argv[3]));
            for (const auto& p : problems) RBREW_LOG_ERROR("online_tests", "%s\n", p.c_str());
            return problems.empty() ? 0 : 2;
        }
        return run_tests();
    } catch (const std::exception& error) {
        RBREW_LOG_ERROR("online_tests", "%s\n", error.what());
        return 3;
    }
}

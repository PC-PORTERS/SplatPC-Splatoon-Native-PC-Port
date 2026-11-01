// Online identity, server address, pseudo -> Mii name and NEX token layout.
#include "runtime/online_identity.h"
#include <cstdio>
#include <stdexcept>
#include <vector>

void rbrew_net_note(const char*, const char*) {}   // nsysnet trace, unused here
static void check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }

int main() {
    try {
        std::mt19937_64 rng(7);
        const auto id = rbrew_online::create(rng);
        check(rbrew_online::valid(id) && id.pid >= 0x10000000u && int32_t(id.pid) > 0, "created identity is valid");
        std::istringstream text(rbrew_online::serialize(id));
        const auto back = rbrew_online::parse(text);
        check(back && back->pid == id.pid && back->password == id.password, "identity round trip");
        std::istringstream broken("pid=0\npassword=short\n");
        check(!rbrew_online::parse(broken), "invalid identity is rejected");

        const auto file = std::filesystem::temp_directory_path() / "rbrew_identity_test.ini";
        std::filesystem::remove(file);
        const auto first = rbrew_online::load_or_create(file);
        const auto second = rbrew_online::load_or_create(file);
        check(std::filesystem::exists(file) && first.pid == second.pid && first.password == second.password, "identity is created once, then stable");
        std::filesystem::remove(file);

        std::string host; uint16_t port = 0;
        check(rbrew_online::parse_server("127.0.0.1:60000", host, port) && host == "127.0.0.1" && port == 60000, "server ip:port");
        for (const char* bad : {"", "127.0.0.1", "127.0.0.1:", "localhost:60000", "1.2.3:60000", "127.0.0.1:0", "127.0.0.1:70000", "127.0.0.1:6x"})
            check(!rbrew_online::parse_server(bad, host, port), "malformed servers are rejected");

        check(rbrew_online::mii_name("", u"default") == u"default", "empty pseudo keeps the default name");
        check(rbrew_online::mii_name("Inkling", u"default") == u"Inkling", "ASCII pseudo");
        const std::u16string pepe{u'P', char16_t(0xE9), u'p', char16_t(0xE9)};   // explicit: MSVC reads this file as cp1252
        check(rbrew_online::mii_name("P\xC3\xA9p\xC3\xA9", u"default") == pepe, "accents (UTF-8) become UTF-16");
        check(rbrew_online::mii_name("ABCDEFGHIJKLMN", u"default") == u"ABCDEFGHIJ", "at most 10 characters");
        check(rbrew_online::mii_name("a\tb\x7f" "c  ", u"default") == u"abc", "controls dropped, trailing spaces trimmed");
        check(rbrew_online::mii_name("   ", u"default") == u"default", "blank pseudo keeps the default name");
        check(rbrew_online::mii_name("a\xE2\x80\xAE" "b\xE2\x80\x8B" "c\xEF\xBB\xBF", u"default") == u"abc", "bidi override, zero-width space and BOM dropped");
        check(rbrew_online::mii_name("\xE2\x80\xAE\xE2\x80\x8B", u"default") == u"default", "a pseudo made only of invisible characters keeps the default name");

        std::vector<uint8_t> token(rbrew_online::kNexTokenSize + 8, 0xCC);
        rbrew_online::fill_nex_token(token.data(), "rbrew1:12345678:10162b00", id.password, "127.0.0.1", 60000);
        check(std::string(reinterpret_cast<char*>(token.data())) == "rbrew1:12345678:10162b00", "token at +0");
        check(std::string(reinterpret_cast<char*>(token.data() + 0x204)) == id.password, "password at +0x204");
        check(std::string(reinterpret_cast<char*>(token.data() + 0x248)) == "127.0.0.1", "host at +0x248");
        check(token[0x258] == 0xEA && token[0x259] == 0x60, "port big-endian at +0x258");
        check(token[0x201] == 0 && token[0x203] == 0 && token[0x245] == 0 && token[0x25A] == 0 && token[0x25B] == 0, "padding zeroed");
        check(token[rbrew_online::kNexTokenSize] == 0xCC, "nothing written past 0x25C");
        rbrew_online::fill_nex_token(token.data(), std::string(600, 'x'), std::string(100, 'p'), "255.255.255.255:1", 1);
        check(token[0x1FF] == 'x' && token[0x200] == 0 && token[0x243] == 'p' && token[0x244] == 0 && token[0x256] == '5' && token[0x257] == 0,
              "long fields are truncated and stay NUL-terminated");
        std::puts("online identity: identity file, server, pseudo and NEX token layout passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}

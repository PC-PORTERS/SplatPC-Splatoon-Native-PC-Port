// Network confinement and guest socket descriptors (os/nsysnet/net_policy.h).
#include "os/nsysnet/net_policy.h"
#include <cstdio>
#include <stdexcept>

static void check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
using rbrew_net::ipv4;

int main() {
    try {
        const rbrew_net::Policy none;
        check(none.allow_destination(ipv4(127, 0, 0, 1)) && none.allow_destination(ipv4(127, 4, 5, 6)), "loopback is allowed");
        for (uint32_t a : {ipv4(8, 8, 8, 8), ipv4(192, 168, 1, 10), ipv4(10, 0, 0, 1), ipv4(0, 0, 0, 0), ipv4(255, 255, 255, 255)})
            check(!none.allow_destination(a), "internet, LAN, wildcard and broadcast are blocked");
        check(none.bind_address(0) == rbrew_net::kLoopback, "a wildcard bind lands on loopback");
        check(none.bind_address(ipv4(192, 168, 1, 10)) == rbrew_net::kLoopback, "a LAN bind lands on loopback");
        check(none.bind_address(ipv4(127, 0, 0, 5)) == ipv4(127, 0, 0, 5), "a loopback bind is kept");
        for (const char* name : {"nncs1.app.nintendowifi.net", "account.nintendo.net", "google.com", "8.8.8.8", ""})
            check(!none.resolve(name), "without overrides no name resolves (no real DNS)");
        check(none.resolve("127.0.0.1") == rbrew_net::kLoopback, "a loopback literal resolves");

        const rbrew_net::Policy lab("nncs1.app.nintendowifi.net=127.0.0.1, bad, x=300.1.1.1,Lab.Local=127.0.0.2,=1.2.3.4");
        check(lab.overrides() == 2, "only well-formed overrides are kept");
        check(lab.resolve("NNCS1.app.nintendowifi.net") == rbrew_net::kLoopback, "override names are case-insensitive");
        check(lab.resolve("lab.local") == ipv4(127, 0, 0, 2), "second override");
        check(!lab.resolve("nncs2.app.nintendowifi.net"), "names not listed stay blocked");
        uint32_t v = 0;
        check(!rbrew_net::Policy::parse_ipv4("1.2.3", v) && !rbrew_net::Policy::parse_ipv4("1.2.3.4.5", v) &&
              !rbrew_net::Policy::parse_ipv4("1..3.4", v) && !rbrew_net::Policy::parse_ipv4("1.2.3.1000", v), "malformed IPs are rejected");

        rbrew_net::FdTable<unsigned long long, ~0ull> fds;
        check(fds.add(0x2A4) == 0 && fds.add(0x9C10) == 1, "large host handles get small guest descriptors");
        check(fds.get(1) == 0x9C10 && fds.get(5) == ~0ull && fds.get(-1) == ~0ull && fds.get(32) == ~0ull, "lookups");
        check(fds.remove(0) == 0x2A4 && fds.get(0) == ~0ull && fds.add(0x77) == 0, "a closed descriptor is reused");
        for (int i = 2; i < 32; ++i) check(fds.add(0x1000 + i) == i, "32 descriptors fit the guest fd_set");
        check(fds.add(0x5555) == -1, "a 33rd socket is refused, not aliased");
        std::puts("net policy: loopback-only confinement, DNS overrides and guest descriptors passed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}

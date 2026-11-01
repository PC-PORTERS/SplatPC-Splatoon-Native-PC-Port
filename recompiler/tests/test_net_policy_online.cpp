// Online allow-list (bench only, no nsysnet): os/nsysnet/net_policy.h.
//
// Proves the step-5 contract before nsysnet is touched (see docs/PATCH-ETAPE5-nsysnet.diff):
// lab mode is unchanged, online mode opens only loopback + addresses announced at startup +
// peers explicitly announced by the caller (bounded, expiring), and hostile destinations stay
// blocked. Inbound datagrams never widen the list: nothing here learns from a datagram source.
#include "os/nsysnet/net_policy.h"
#include <cstdio>
#include <stdexcept>

static void check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
using rbrew_net::ipv4;
using rbrew_net::Policy;

static double g_now = 1000.0;
static double fake_clock() { return g_now; }

int main() {
    try {
        // --- lab mode: strictly the previous behaviour --------------------------------
        Policy lab(nullptr, false, fake_clock);
        check(lab.allow_destination(ipv4(127, 0, 0, 1)), "lab allows loopback");
        check(!lab.allow_destination(ipv4(203, 0, 113, 10)), "lab refuses a public address");
        check(lab.bind_address(0) == rbrew_net::kLoopback, "lab wildcard bind lands on loopback");
        check(lab.bind_address(ipv4(192, 168, 1, 10)) == rbrew_net::kLoopback, "lab LAN bind lands on loopback");
        check(lab.bind_address(ipv4(127, 0, 0, 5)) == ipv4(127, 0, 0, 5), "lab loopback bind kept");
        check(lab.add_peer(ipv4(203, 0, 113, 10)), "an announced entry is stored");
        check(!lab.allow_destination(ipv4(203, 0, 113, 10)), "lab ignores the announced list");
        check(!lab.online(), "lab reports offline policy");

        // --- online: announced addresses can seed the policy at construction ----------
        Policy seeded(nullptr, true, fake_clock,
                      {ipv4(198, 51, 100, 7), ipv4(203, 0, 113, 11)});
        check(seeded.allow_destination(ipv4(198, 51, 100, 7)), "seeded server address is allowed");
        check(seeded.allow_destination(ipv4(203, 0, 113, 11)), "seeded nncs address is allowed");
        check(!seeded.allow_destination(ipv4(203, 0, 113, 12)), "an unseeded address stays refused");

        // --- online: unknown refused, announced address accepted ----------------------
        Policy online(nullptr, true, fake_clock);
        check(online.online(), "online reports online policy");
        check(online.allow_destination(ipv4(127, 0, 0, 1)), "online keeps loopback");
        check(!online.allow_destination(ipv4(203, 0, 113, 10)), "online refuses an unknown address");
        check(online.allow(ipv4(198, 51, 100, 7)), "server address is accepted");
        check(online.allow_destination(ipv4(198, 51, 100, 7)), "server address is allowed");
        g_now += 10000.0;
        check(online.allow_destination(ipv4(198, 51, 100, 7)), "fixed entry never expires");

        // --- online: an announced peer is allowed, then expires --------------------------
        g_now = 1000.0;
        const uint32_t peer = ipv4(192, 0, 2, 44);
        check(online.add_peer(peer), "peer announced");
        check(online.allow_destination(peer), "announced peer allowed");
        g_now += rbrew_net::kLearnedTtl - 1.0;
        check(online.allow_destination(peer), "announced peer still valid before the TTL");
        g_now += 2.0;
        check(!online.allow_destination(peer), "expired peer refused");
        check(online.add_peer(peer), "peer announced again");
        check(online.allow_destination(peer), "re-announced peer allowed");

        // --- hostile or unusable destinations ----------------------------------------
        const uint32_t hostile[] = {ipv4(0, 0, 0, 0), ipv4(0, 1, 2, 3), ipv4(255, 255, 255, 255),
                                    ipv4(224, 0, 0, 1), ipv4(239, 255, 255, 255), ipv4(240, 0, 0, 1)};
        for (uint32_t address : hostile) {
            check(!Policy::acceptable(address), "hostile address is not acceptable");
            check(!online.add_peer(address), "hostile address is not announced");
            check(!online.allow_destination(address), "hostile address is not allowed");
        }
        // Loopback is always allowed, but it is not a announced peer entry.
        check(!Policy::acceptable(ipv4(127, 0, 0, 1)), "loopback is not a peer entry");
        check(!online.add_peer(ipv4(127, 0, 0, 1)), "loopback is not announced");
        check(online.allow_destination(ipv4(127, 0, 0, 1)), "loopback stays allowed");
        check(Policy::acceptable(ipv4(1, 1, 1, 1)), "public unicast is acceptable");
        check(Policy::acceptable(ipv4(169, 254, 1, 2)), "link-local is acceptable");

        // --- the list is bounded ------------------------------------------------------
        Policy bounded(nullptr, true, fake_clock);
        size_t accepted = 0;
        for (uint32_t i = 0; i < rbrew_net::kMaxAllowed + 64; ++i)
            if (bounded.add_peer(ipv4(1, uint8_t(i >> 8), uint8_t(i), 2))) ++accepted;
        check(accepted == rbrew_net::kMaxAllowed, "the allow-list cap is enforced");
        check(bounded.allowed() == rbrew_net::kMaxAllowed, "the count matches the cap");
        check(!bounded.add_peer(ipv4(2, 2, 2, 2)), "a full list refuses a new peer");
        g_now += rbrew_net::kLearnedTtl + 1.0;
        check(bounded.allowed() == 0, "expired entries free the list");
        check(bounded.add_peer(ipv4(2, 2, 2, 2)), "room is available once entries expire");

        // --- online bind: every interface is allowed, an address is honoured ----------
        check(online.bind_address(0) == 0, "online wildcard bind stays wildcard");
        check(online.bind_address(ipv4(192, 168, 1, 10)) == ipv4(192, 168, 1, 10), "online LAN bind kept");
        check(online.bind_address(ipv4(127, 0, 0, 1)) == ipv4(127, 0, 0, 1), "online loopback bind kept");

        // --- announced names can be installed at runtime ------------------------------
        Policy named("nncs1.app.nintendowifi.net=127.0.0.1", true, fake_clock);
        check(named.resolve("NNCS1.app.nintendowifi.net") == rbrew_net::kLoopback, "override is case-insensitive");
        named.set_override("nncs2.app.nintendowifi.net", ipv4(198, 51, 100, 8));
        check(named.resolve("nncs2.app.nintendowifi.net") == ipv4(198, 51, 100, 8), "runtime override resolves");
        bool seen = false;
        for (uint32_t a : named.addresses()) seen = seen || a == ipv4(198, 51, 100, 8);
        check(seen, "addresses() exposes the announced addresses");
        for (uint32_t a : named.addresses())
            if (!Policy::is_loopback(a)) check(named.allow(a), "announced addresses are allow-listed");

        std::puts("net policy online: lab unchanged, allow-list bounded and expiring, hostile addresses refused");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}

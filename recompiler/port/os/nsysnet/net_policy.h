#pragma once
// Network confinement, online allow-list and guest socket descriptors for nsysnet
// (testable, no sockets).
//
// Lab mode (default, online_server empty or loopback): confinement is always on. Until a
// multiplayer room explicitly adds its peers, the game may only talk to this PC. Datagrams
// go to 127.0.0.0/8 only, sockets bind to 127.0.0.1 instead of every interface, and names
// resolve only through RBREW_DNS_OVERRIDE (nothing reaches a real DNS server, and no
// Nintendo service is ever contacted). Blocking happens at sendto/bind/DNS, where the
// destination is known, not by host name (a direct IP would pass).
//
// Online mode (online_server is a non-loopback address): the policy stops rewriting binds.
// The allow-list holds loopback, the addresses the server announced at startup (NEX host and
// the nncs1/nncs2 values of RBREW_DNS_OVERRIDE) and peers (bounded, expiring). In practice
// nsysnet.cpp's sendto adds EVERY unicast destination the game sends to as a peer, because the
// matchmaking station URLs are inside NEX messages this layer cannot read; so outgoing
// datagrams are not confined to the announced addresses. Incoming datagrams are not filtered by
// default either; RBREW_STRICT_UDP_IN=1 delivers only datagrams from this PC, the announced
// server or a peer already sent to (off by default until tested between two real networks).
// Names still resolve only through the overrides. Not reachable: a real DNS server and any
// Nintendo service.
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rbrew_net {

constexpr uint32_t ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return uint32_t(a) << 24 | uint32_t(b) << 16 | uint32_t(c) << 8 | d;
}
constexpr uint32_t kLoopback = ipv4(127, 0, 0, 1);
// Allow-list limits (online mode only). Bounded so a hostile peer cannot grow it forever.
constexpr size_t kMaxAllowed = 256;
constexpr double kLearnedTtl = 300.0;      // seconds an announced peer stays allowed

inline double steady_seconds() {           // monotonic, injection point for tests
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// Runtime hook (implemented in nsysnet.cpp by PATCH-ETAPE5): allow a peer whose address came
// from matchmaking station URLs. Never call it with a datagram source address.
void allow_peer(uint32_t address);

class Policy {
public:
    using Clock = double (*)();

    // overrides: "name=a.b.c.d,name2=a.b.c.d" (RBREW_DNS_OVERRIDE); names compare
    // case-insensitively. Malformed entries are ignored. `fixed` seeds the allow-list with
    // the addresses announced at startup (NEX host, nncs1/nncs2), used online only.
    explicit Policy(const char* overrides = nullptr, bool online = false,
                    Clock clock = steady_seconds,
                    const std::vector<uint32_t>& fixed = std::vector<uint32_t>())
        : online_(online), clock_(clock ? clock : steady_seconds) {
        if (overrides) {
            std::string text(overrides), item;
            for (size_t start = 0; start <= text.size();) {
                const size_t end = text.find(',', start);
                item = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
                const size_t eq = item.find('=');
                uint32_t addr = 0;
                if (eq != std::string::npos && eq && parse_ipv4(item.substr(eq + 1), addr))
                    names_.push_back({lower(item.substr(0, eq)), addr});
                if (end == std::string::npos) break;
                start = end + 1;
            }
        }
        for (uint32_t address : fixed) allow(address);
    }
    // Host-order IPv4 destination of a datagram.
    bool allow_destination(uint32_t addr) const {
        if (is_loopback(addr)) return true;      // lab and online: this PC always works
        return online_ && contains(addr);
    }
    // Address a socket may bind. Lab: wildcard and foreign addresses become loopback.
    // Online: the request is honoured (0 = every interface), since the server must be
    // reachable; an explicit address is left to the OS.
    uint32_t bind_address(uint32_t requested) const {
        if (online_) return requested;
        return allow_destination(requested) ? requested : kLoopback;
    }
    // Name resolution: only overrides answer; a literal loopback IP is accepted.
    std::optional<uint32_t> resolve(const std::string& name) const {
        const std::string key = lower(name);
        std::lock_guard lock(mutex_);
        for (const auto& [n, addr] : names_) if (n == key) return addr;
        uint32_t literal = 0;
        if (parse_ipv4(name, literal) && is_loopback(literal)) return literal;
        return std::nullopt;
    }
    // Runtime override, used online to map the server's announced names to its addresses.
    void set_override(const std::string& name, uint32_t addr) {
        const std::string key = lower(name);
        std::lock_guard lock(mutex_);
        for (auto& [n, value] : names_) if (n == key) { value = addr; return; }
        names_.push_back({key, addr});
    }
    // Addresses carried by the overrides (the caller allow-lists them online).
    std::vector<uint32_t> addresses() const {
        std::lock_guard lock(mutex_);
        std::vector<uint32_t> out;
        for (const auto& [n, addr] : names_) out.push_back(addr);
        return out;
    }
    // Fixed entry: an address announced at startup (server, nncs). No expiry.
    bool allow(uint32_t addr) {
        if (!acceptable(addr)) return false;
        std::lock_guard lock(mutex_);
        for (const auto& entry : fixed_) if (entry.addr == addr) return true;
        if (fixed_.size() + learned_.size() >= kMaxAllowed) return false;
        fixed_.push_back({addr, 0.0});
        return true;
    }
    // Announced peer (a station URL received from matchmaking), with expiry. Never call this
    // with the source address of an arbitrary datagram: that would let any host add itself.
    // A full list first drops expired entries; if it is still full the peer is refused.
    bool add_peer(uint32_t addr) const {
        if (!acceptable(addr)) return false;
        std::lock_guard lock(mutex_);
        const double expires = clock_() + kLearnedTtl;
        for (auto& entry : learned_) if (entry.addr == addr) { entry.expires = expires; return true; }
        if (fixed_.size() + learned_.size() >= kMaxAllowed) {
            prune();
            if (fixed_.size() + learned_.size() >= kMaxAllowed) return false;
        }
        learned_.push_back({addr, expires});
        return true;
    }
    size_t overrides() const {
        std::lock_guard lock(mutex_);
        return names_.size();
    }
    // Unexpired entries (fixed + announced peers) currently allowed.
    size_t allowed() const {
        std::lock_guard lock(mutex_);
        return fixed_.size() + live_learned();
    }
    bool online() const { return online_; }

    // Hostile or unusable destinations: 0/8, loopback (always allowed separately),
    // multicast 224/4 and reserved 240/4 (which includes the broadcast address).
    // IPv4-mapped IPv6 cannot reach this API: callers pass IPv4 only, and the mapped
    // forms (0.0.0.0/8, 127/8) are rejected here too.
    static bool acceptable(uint32_t addr) {
        const uint32_t first = addr >> 24;
        return first != 0 && first != 127 && first < 224;
    }
    static bool is_loopback(uint32_t addr) { return (addr >> 24) == 127; }

    static bool parse_ipv4(const std::string& s, uint32_t& out) {
        uint32_t parts[4] = {}, n = 0, value = 0, digits = 0;
        for (char c : s) {
            if (c >= '0' && c <= '9') { value = value * 10 + uint32_t(c - '0'); if (++digits > 3 || value > 255) return false; }
            else if (c == '.' && digits && n < 3) { parts[n++] = value; value = digits = 0; }
            else return false;
        }
        if (n != 3 || !digits) return false;
        parts[3] = value;
        out = parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3];
        return true;
    }
private:
    struct Entry { uint32_t addr; double expires; };

    size_t live_learned() const {          // caller holds mutex_
        const double now = clock_();
        size_t count = 0;
        for (const auto& entry : learned_) if (entry.expires > now) ++count;
        return count;
    }
    bool contains(uint32_t addr) const {
        std::lock_guard lock(mutex_);
        for (const auto& entry : fixed_) if (entry.addr == addr) return true;
        const double now = clock_();
        for (const auto& entry : learned_)
            if (entry.addr == addr && entry.expires > now) return true;
        return false;
    }
    void prune() const {                   // caller holds mutex_
        const double now = clock_();
        size_t kept = 0;
        for (const auto& entry : learned_)
            if (entry.expires > now) learned_[kept++] = entry;
        learned_.resize(kept);
    }
    static std::string lower(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a'); return s; }

    bool online_ = false;
    Clock clock_ = steady_seconds;
    mutable std::mutex mutex_;
    std::vector<std::pair<std::string, uint32_t>> names_;
    mutable std::vector<Entry> fixed_;     // mutable: peers are added through the const policy
    mutable std::vector<Entry> learned_;
};

// Guest socket descriptors. Wii U descriptors are small integers and the guest's fd_set is
// one 32-bit word, while Windows SOCKET values are large handles: select() could never see
// them. Guest fds 0..31 map to host handles.
template <class Handle, Handle Invalid>
class FdTable {
public:
    static constexpr int kSize = 32;
    int add(Handle h) {
        std::lock_guard lock(mutex_);
        for (int fd = 0; fd < kSize; ++fd) if (slots_[fd] == Invalid) { slots_[fd] = h; return fd; }
        return -1;
    }
    Handle get(int fd) const {
        std::lock_guard lock(mutex_);
        return fd >= 0 && fd < kSize ? slots_[fd] : Invalid;
    }
    Handle remove(int fd) {
        std::lock_guard lock(mutex_);
        if (fd < 0 || fd >= kSize) return Invalid;
        const Handle h = slots_[fd]; slots_[fd] = Invalid; return h;
    }
private:
    mutable std::mutex mutex_;
    std::array<Handle, kSize> slots_ = [] { std::array<Handle, kSize> a; a.fill(Invalid); return a; }();
};

}  // namespace rbrew_net

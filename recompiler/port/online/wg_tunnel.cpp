#include "wg_tunnel.h"

#include "rebrewu/logger.hpp"

#ifdef RBREW_HAS_BORINGTUN
#include "boringtun_client.h"
#endif

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#define CLOSE_SOCK(s) closesocket((SOCKET)(s))
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSE_SOCK(s) close((int)(s))
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>

namespace rbrew_native {
namespace {

bool parse_ipv4_host(const std::string& text, uint32_t& out) {
    struct in_addr addr{};
    if (text.empty() || inet_pton(AF_INET, text.c_str(), &addr) != 1) return false;
    out = ntohl(addr.s_addr);
    return true;
}

bool parse_cidr(const std::string& cidr, uint32_t& net_out, uint32_t& mask_out) {
    const size_t slash = cidr.find('/');
    if (slash == std::string::npos) return false;
    uint32_t ip = 0;
    if (!parse_ipv4_host(cidr.substr(0, slash), ip)) return false;
    int bits = -1;
    const std::string bits_str = cidr.substr(slash + 1);
    const auto r = std::from_chars(bits_str.data(), bits_str.data() + bits_str.size(), bits);
    if (r.ec != std::errc{} || r.ptr != bits_str.data() + bits_str.size() || bits < 0 || bits > 32) return false;
    mask_out = bits == 0 ? 0u : (~0u << (32 - bits));
    net_out = ip & mask_out;
    return true;
}

bool parse_endpoint(const std::string& endpoint, std::string& host_out, std::string& port_out) {
    const size_t colon = endpoint.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= endpoint.size()) return false;
    host_out = endpoint.substr(0, colon);
    port_out = endpoint.substr(colon + 1);
    return true;
}

uint16_t ipv4_checksum(const uint8_t* hdr, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        sum += (uint32_t(hdr[i]) << 8) | hdr[i + 1];
    }
    if (len & 1) sum += uint32_t(hdr[len - 1]) << 8;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return uint16_t(~sum & 0xFFFFu);
}

uint16_t udp_checksum(uint32_t src_ip, uint32_t dst_ip, const uint8_t* udp_pkt, size_t udp_len) {
    uint32_t sum = 0;
    sum += (src_ip >> 16) & 0xFFFFu;
    sum += src_ip & 0xFFFFu;
    sum += (dst_ip >> 16) & 0xFFFFu;
    sum += dst_ip & 0xFFFFu;
    sum += 17u;
    sum += uint32_t(udp_len);
    for (size_t i = 0; i + 1 < udp_len; i += 2) {
        sum += (uint32_t(udp_pkt[i]) << 8) | udp_pkt[i + 1];
    }
    if (udp_len & 1) sum += uint32_t(udp_pkt[udp_len - 1]) << 8;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    const uint16_t res = uint16_t(~sum & 0xFFFFu);
    return res == 0 ? 0xFFFFu : res;
}

void send_wg_datagram(intptr_t sock, const uint8_t* buf, int len) {
    if (sock < 0 || len <= 0) return;
#if defined(_WIN32)
    send((SOCKET)sock, reinterpret_cast<const char*>(buf), len, 0);
#else
    const ssize_t ignored = send((int)sock, buf, size_t(len), 0);
    (void)ignored;
#endif
}

bool create_wake_pair(intptr_t& read_fd, intptr_t& write_fd) {
#if defined(_WIN32)
    SOCKET r = socket(AF_INET, SOCK_DGRAM, 0);
    SOCKET w = socket(AF_INET, SOCK_DGRAM, 0);
    if (r == INVALID_SOCKET || w == INVALID_SOCKET) {
        if (r != INVALID_SOCKET) closesocket(r);
        if (w != INVALID_SOCKET) closesocket(w);
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(r, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(r);
        closesocket(w);
        return false;
    }
    int len = sizeof(addr);
    if (getsockname(r, reinterpret_cast<sockaddr*>(&addr), &len) != 0 ||
        connect(w, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(r);
        closesocket(w);
        return false;
    }
    u_long nb = 1;
    ioctlsocket(r, FIONBIO, &nb);
    ioctlsocket(w, FIONBIO, &nb);
    read_fd = intptr_t(r);
    write_fd = intptr_t(w);
    return true;
#else
    int fds[2]{-1, -1};
    if (pipe(fds) != 0) return false;
    for (int fd : fds) {
        const int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    read_fd = fds[0];
    write_fd = fds[1];
    return true;
#endif
}

void close_wake_pair(intptr_t read_fd, intptr_t write_fd) {
    if (read_fd >= 0) CLOSE_SOCK(read_fd);
    if (write_fd >= 0) CLOSE_SOCK(write_fd);
}

void signal_wake(intptr_t write_fd) {
    if (write_fd < 0) return;
    const char b = 1;
#if defined(_WIN32)
    send((SOCKET)write_fd, &b, 1, 0);
#else
    const ssize_t ignored = write((int)write_fd, &b, 1);
    (void)ignored;
#endif
}

void drain_wake(intptr_t read_fd) {
    if (read_fd < 0) return;
    char buf[64];
#if defined(_WIN32)
    while (recv((SOCKET)read_fd, buf, sizeof(buf), 0) > 0) {}
#else
    while (read((int)read_fd, buf, sizeof(buf)) > 0) {}
#endif
}

} // namespace

std::string derive_wg_path(const std::string& register_path) {
    static constexpr const char kSuffix[] = "/register";
    const size_t n = sizeof(kSuffix) - 1;
    if (register_path.size() >= n && register_path.compare(register_path.size() - n, n, kSuffix) == 0) {
        return register_path.substr(0, register_path.size() - n) + "/social/wg";
    }
    return "/social/wg";
}

bool generate_wg_keypair(std::string& priv_b64_out, std::string& pub_b64_out) {
#ifdef RBREW_HAS_BORINGTUN
    char priv_buf[64]{};
    char pub_buf[64]{};
    if (boringtun_generate_keypair(priv_buf, sizeof(priv_buf), pub_buf, sizeof(pub_buf)) != 0) return false;
    priv_b64_out = priv_buf;
    pub_b64_out = pub_buf;
    return true;
#else
    priv_b64_out.clear();
    pub_b64_out.clear();
    return false;
#endif
}

bool fetch_wg_config(Transport& transport, const std::string& wg_path, const std::string& token,
                     const std::string& public_key, WgConfig& out, std::string& error_out, int* out_http_status) {
    error_out.clear();
    nlohmann::json body = {{"token", token}};
    if (!public_key.empty()) body["public_key"] = public_key;

    const Reply r = transport.request("POST", wg_path.empty() ? "/social/wg" : wg_path, &body);
    if (out_http_status) *out_http_status = r.status;
    if (r.status == 0) {
        error_out = "WireGuard key request failed: server unreachable.";
        return false;
    }
    if (r.status == 403) {
        error_out = "This account is banned from WireGuard P2P.";
        return false;
    }
    if (r.status == 404 || r.status == 501) {
        error_out = "WireGuard P2P is not enabled on this server.";
        return false;
    }
    if (r.status != 200 || !r.body.is_object()) {
        error_out = r.body.is_object() ? r.body.value("error", "WireGuard key request refused.")
                                       : "WireGuard key request refused.";
        return false;
    }
    out.client_ip = r.body.value("client_ip", std::string());
    out.subnet = r.body.value("subnet", std::string());
    out.server_ip = r.body.value("server_ip", std::string());
    out.nncs2_ip = r.body.value("nncs2_ip", out.server_ip);
    out.server_public_key = r.body.value("server_public_key", std::string());
    out.endpoint = r.body.value("endpoint", std::string());
    out.persistent_keepalive = uint16_t(r.body.value("persistent_keepalive", 25));
    out.mtu = uint16_t(r.body.value("mtu", 1420));
    if (r.body.contains("private_key") && r.body["private_key"].is_string()) {
        out.private_key = r.body["private_key"].get<std::string>();
    }
    if (out.client_ip.empty() || out.subnet.empty() || out.server_public_key.empty() || out.endpoint.empty()) {
        error_out = "Incomplete WireGuard configuration from server.";
        return false;
    }
    return true;
}

WgTunnel& WgTunnel::instance() {
    static WgTunnel s_instance;
    return s_instance;
}

WgTunnel::~WgTunnel() {
    stop();
}

bool WgTunnel::start(const WgConfig& cfg) {
#ifndef RBREW_HAS_BORINGTUN
    (void)cfg;
    RBREW_LOG_WARN("wireguard", "boringtun support is not compiled in\n");
    return false;
#else
    stop();

    uint32_t client_ip = 0, net = 0, mask = 0;
    if (!parse_ipv4_host(cfg.client_ip, client_ip) || !parse_cidr(cfg.subnet, net, mask)) {
        RBREW_LOG_WARN("wireguard", "invalid client_ip (%s) or subnet (%s)\n", cfg.client_ip.c_str(),
                       cfg.subnet.c_str());
        return false;
    }
    uint32_t server_ip = 0, nncs2_ip = 0;
    if (!cfg.server_ip.empty()) parse_ipv4_host(cfg.server_ip, server_ip);
    if (!cfg.nncs2_ip.empty()) parse_ipv4_host(cfg.nncs2_ip, nncs2_ip);
    if (nncs2_ip == 0) nncs2_ip = server_ip;

    uint32_t pub_srv = 0, pub_n1 = 0, pub_n2 = 0;
    parse_ipv4_host(cfg.public_server_ip, pub_srv);
    parse_ipv4_host(cfg.public_nncs1_ip, pub_n1);
    parse_ipv4_host(cfg.public_nncs2_ip, pub_n2);

    std::string ep_host, ep_port;
    if (!parse_endpoint(cfg.endpoint, ep_host, ep_port)) {
        RBREW_LOG_WARN("wireguard", "invalid endpoint %s\n", cfg.endpoint.c_str());
        return false;
    }

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(ep_host.c_str(), ep_port.c_str(), &hints, &res) != 0 || !res) {
        RBREW_LOG_WARN("wireguard", "could not resolve endpoint %s\n", cfg.endpoint.c_str());
        return false;
    }

    intptr_t sock = -1;
#if defined(_WIN32)
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        if (connect(s, res->ai_addr, int(res->ai_addrlen)) == 0) {
            u_long nb = 1;
            ioctlsocket(s, FIONBIO, &nb);
            sock = intptr_t(s);
        } else {
            closesocket(s);
        }
    }
#else
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) {
        if (connect(s, res->ai_addr, res->ai_addrlen) == 0) {
            const int flags = fcntl(s, F_GETFL, 0);
            if (flags >= 0) fcntl(s, F_SETFL, flags | O_NONBLOCK);
            fcntl(s, F_SETFD, FD_CLOEXEC);
            sock = s;
        } else {
            close(s);
        }
    }
#endif
    freeaddrinfo(res);
    if (sock < 0) {
        RBREW_LOG_WARN("wireguard", "failed to connect UDP socket to %s\n", cfg.endpoint.c_str());
        return false;
    }

    BtTunnel* tun =
        boringtun_tunnel_new(cfg.private_key.c_str(), cfg.server_public_key.c_str(), cfg.persistent_keepalive);
    if (!tun) {
        CLOSE_SOCK(sock);
        RBREW_LOG_WARN("wireguard", "failed to initialize boringtun tunnel (invalid keys)\n");
        return false;
    }

    client_ip_ = client_ip;
    server_ip_ = server_ip;
    nncs2_ip_ = nncs2_ip;
    subnet_net_ = net;
    subnet_mask_ = mask;
    public_server_ip_ = pub_srv;
    public_nncs1_ip_ = pub_n1;
    public_nncs2_ip_ = pub_n2;
    server_ip_str_ = cfg.server_ip;
    nncs2_ip_str_ = cfg.nncs2_ip.empty() ? cfg.server_ip : cfg.nncs2_ip;
    bt_tun_ = tun;
    udp_sock_ = sock;
    stop_requested_.store(false, std::memory_order_release);
    active_.store(true, std::memory_order_release);

    // Initiate the WireGuard handshake right away before game sockets start talking.
    std::array<uint8_t, 512> hs_buf{};
    const int hs_len = boringtun_force_handshake(tun, hs_buf.data(), hs_buf.size());
    if (hs_len > 0) send_wg_datagram(sock, hs_buf.data(), hs_len);

    io_thread_ = std::thread(&WgTunnel::io_loop, this);

    RBREW_LOG_INFO("wireguard", "boringtun tunnel started: client=%s subnet=%s endpoint=%s\n", cfg.client_ip.c_str(),
                   cfg.subnet.c_str(), cfg.endpoint.c_str());
    return true;
#endif
}

void WgTunnel::stop() {
    stop_requested_.store(true, std::memory_order_release);
    active_.store(false, std::memory_order_release);
    rx_cv_.notify_all();
    if (io_thread_.joinable()) io_thread_.join();

    if (udp_sock_ >= 0) {
        CLOSE_SOCK(udp_sock_);
        udp_sock_ = -1;
    }
#ifdef RBREW_HAS_BORINGTUN
    if (bt_tun_) {
        boringtun_tunnel_free(static_cast<BtTunnel*>(bt_tun_));
        bt_tun_ = nullptr;
    }
#endif
    std::lock_guard lock(rx_mu_);
    for (auto& [fd, q] : sockets_) {
        close_wake_pair(q.wake_read, q.wake_write);
    }
    sockets_.clear();
}

bool WgTunnel::contains_ip(uint32_t ip_host) const {
    return active() && ((ip_host & subnet_mask_) == subnet_net_);
}

std::string WgTunnel::server_ip_str() const {
    return server_ip_str_;
}

std::string WgTunnel::nncs2_ip_str() const {
    return nncs2_ip_str_;
}

uint32_t WgTunnel::rewrite_destination(uint32_t dst_ip_host) const {
    if (!active()) return dst_ip_host;
    if (server_ip_ != 0 && ((public_server_ip_ != 0 && dst_ip_host == public_server_ip_) ||
                            (public_nncs1_ip_ != 0 && dst_ip_host == public_nncs1_ip_))) {
        return server_ip_;
    }
    if (nncs2_ip_ != 0 && public_nncs2_ip_ != 0 && dst_ip_host == public_nncs2_ip_) {
        return nncs2_ip_;
    }
    return dst_ip_host;
}

void WgTunnel::register_udp_socket(int guest_fd, uint16_t local_port_host) {
    if (!active() || local_port_host == 0) return;
    std::lock_guard lock(rx_mu_);
    auto it = sockets_.find(guest_fd);
    if (it != sockets_.end()) {
        it->second.local_port = local_port_host;
        return;
    }
    intptr_t rfd = -1, wfd = -1;
    if (!create_wake_pair(rfd, wfd)) return;
    SocketQueue q;
    q.local_port = local_port_host;
    q.wake_read = rfd;
    q.wake_write = wfd;
    sockets_.emplace(guest_fd, std::move(q));
}

void WgTunnel::unregister_socket(int guest_fd) {
    std::lock_guard lock(rx_mu_);
    auto it = sockets_.find(guest_fd);
    if (it != sockets_.end()) {
        close_wake_pair(it->second.wake_read, it->second.wake_write);
        sockets_.erase(it);
    }
}

intptr_t WgTunnel::wake_fd(int guest_fd) const {
    std::lock_guard lock(rx_mu_);
    auto it = sockets_.find(guest_fd);
    return it == sockets_.end() ? -1 : it->second.wake_read;
}

bool WgTunnel::has_pending_rx(int guest_fd) const {
    std::lock_guard lock(rx_mu_);
    auto it = sockets_.find(guest_fd);
    return it != sockets_.end() && !it->second.rx.empty();
}

int WgTunnel::send_udp(int guest_fd, uint16_t local_port_host, uint32_t dst_ip_host, uint16_t dst_port_host,
                       const uint8_t* data, size_t len) {
#ifndef RBREW_HAS_BORINGTUN
    (void)guest_fd;
    (void)local_port_host;
    (void)dst_ip_host;
    (void)dst_port_host;
    (void)data;
    (void)len;
    return -1;
#else
    if (!active() || !bt_tun_ || udp_sock_ < 0 || len > 1472) return -1;
    if (local_port_host != 0) register_udp_socket(guest_fd, local_port_host);

    const size_t total = 20 + 8 + len;
    std::array<uint8_t, 1600> ip_pkt{};
    ip_pkt[0] = 0x45;
    ip_pkt[1] = 0x00;
    ip_pkt[2] = uint8_t(total >> 8);
    ip_pkt[3] = uint8_t(total & 0xFF);
    const uint16_t id = ip_id_.fetch_add(1, std::memory_order_relaxed);
    ip_pkt[4] = uint8_t(id >> 8);
    ip_pkt[5] = uint8_t(id & 0xFF);
    ip_pkt[6] = 0x40;
    ip_pkt[7] = 0x00;
    ip_pkt[8] = 64;
    ip_pkt[9] = 17;
    ip_pkt[12] = uint8_t(client_ip_ >> 24);
    ip_pkt[13] = uint8_t((client_ip_ >> 16) & 0xFF);
    ip_pkt[14] = uint8_t((client_ip_ >> 8) & 0xFF);
    ip_pkt[15] = uint8_t(client_ip_ & 0xFF);
    ip_pkt[16] = uint8_t(dst_ip_host >> 24);
    ip_pkt[17] = uint8_t((dst_ip_host >> 16) & 0xFF);
    ip_pkt[18] = uint8_t((dst_ip_host >> 8) & 0xFF);
    ip_pkt[19] = uint8_t(dst_ip_host & 0xFF);
    const uint16_t ip_csum = ipv4_checksum(ip_pkt.data(), 20);
    ip_pkt[10] = uint8_t(ip_csum >> 8);
    ip_pkt[11] = uint8_t(ip_csum & 0xFF);

    uint8_t* udp = ip_pkt.data() + 20;
    const size_t udp_len = 8 + len;
    udp[0] = uint8_t(local_port_host >> 8);
    udp[1] = uint8_t(local_port_host & 0xFF);
    udp[2] = uint8_t(dst_port_host >> 8);
    udp[3] = uint8_t(dst_port_host & 0xFF);
    udp[4] = uint8_t(udp_len >> 8);
    udp[5] = uint8_t(udp_len & 0xFF);
    if (len > 0) std::memcpy(udp + 8, data, len);
    const uint16_t udp_csum = udp_checksum(client_ip_, dst_ip_host, udp, udp_len);
    udp[6] = uint8_t(udp_csum >> 8);
    udp[7] = uint8_t(udp_csum & 0xFF);

    std::array<uint8_t, 2048> wg_buf{};
    int enc_len = 0;
    {
        std::lock_guard lock(tun_mu_);
        if (!bt_tun_) return -1;
        enc_len =
            boringtun_encapsulate(static_cast<BtTunnel*>(bt_tun_), ip_pkt.data(), total, wg_buf.data(), wg_buf.size());
    }
    if (enc_len < 0) return -1;
    if (enc_len > 0) send_wg_datagram(udp_sock_, wg_buf.data(), enc_len);
    return int(len);
#endif
}

int WgTunnel::recv_udp(int guest_fd, uint8_t* out_buf, size_t cap, uint32_t* out_src_ip_host,
                       uint16_t* out_src_port_host, bool nonblocking) {
    std::unique_lock lock(rx_mu_);
    auto find_ready = [&]() -> SocketQueue* {
        auto it = sockets_.find(guest_fd);
        if (it == sockets_.end() || it->second.rx.empty()) return nullptr;
        return &it->second;
    };

    SocketQueue* q = find_ready();
    if (!q && !nonblocking) {
        rx_cv_.wait(lock, [&]() {
            if (stop_requested_.load(std::memory_order_acquire)) return true;
            auto it = sockets_.find(guest_fd);
            return it != sockets_.end() && !it->second.rx.empty();
        });
        q = find_ready();
    }
    if (!q) return -1;

    WgUdpPacket pkt = std::move(q->rx.front());
    q->rx.pop_front();
    if (q->rx.empty()) drain_wake(q->wake_read);

    const size_t n = std::min(cap, pkt.payload.size());
    if (n > 0) std::memcpy(out_buf, pkt.payload.data(), n);
    if (out_src_ip_host) *out_src_ip_host = pkt.src_ip_host;
    if (out_src_port_host) *out_src_port_host = pkt.src_port_host;
    return int(n);
}

void WgTunnel::deliver_ipv4_packet(const uint8_t* pkt, size_t len) {
    if (len < 28 || (pkt[0] >> 4) != 4) return;
    const size_t ihl = size_t(pkt[0] & 0x0F) * 4;
    if (ihl < 20 || len < ihl + 8 || pkt[9] != 17) return;

    const uint32_t src_ip = (uint32_t(pkt[12]) << 24) | (uint32_t(pkt[13]) << 16) | (uint32_t(pkt[14]) << 8) | pkt[15];
    const uint8_t* udp = pkt + ihl;
    const uint16_t src_port = (uint16_t(udp[0]) << 8) | udp[1];
    const uint16_t dst_port = (uint16_t(udp[2]) << 8) | udp[3];
    const uint16_t udp_len = (uint16_t(udp[4]) << 8) | udp[5];
    if (udp_len < 8 || ihl + udp_len > len) return;

    const uint8_t* payload = udp + 8;
    const size_t payload_len = udp_len - 8;

    std::lock_guard lock(rx_mu_);
    for (auto& [fd, q] : sockets_) {
        if (q.local_port == dst_port) {
            if (q.rx.size() < 256) {
                q.rx.push_back(WgUdpPacket{src_ip, src_port, std::vector<uint8_t>(payload, payload + payload_len)});
                signal_wake(q.wake_write);
                rx_cv_.notify_all();
            }
            break;
        }
    }
}

void WgTunnel::io_loop() {
#ifdef RBREW_HAS_BORINGTUN
    std::array<uint8_t, 2048> net_buf{};
    std::array<uint8_t, 2048> tun_buf{};

    while (!stop_requested_.load(std::memory_order_acquire)) {
#if defined(_WIN32)
        WSAPOLLFD pfd{SOCKET(udp_sock_), POLLIN, 0};
        WSAPoll(&pfd, 1, 50);
#else
        struct pollfd pfd{int(udp_sock_), POLLIN, 0};
        poll(&pfd, 1, 50);
#endif
        if (pfd.revents & POLLIN) {
            for (;;) {
#if defined(_WIN32)
                const int r = recv((SOCKET)udp_sock_, reinterpret_cast<char*>(net_buf.data()), int(net_buf.size()), 0);
#else
                const int r = int(recv((int)udp_sock_, net_buf.data(), net_buf.size(), 0));
#endif
                if (r <= 0) break;

                int action = BT_ACTION_DONE;
                int dec_len = 0;
                {
                    std::lock_guard lock(tun_mu_);
                    if (!bt_tun_) break;
                    dec_len = boringtun_decapsulate(static_cast<BtTunnel*>(bt_tun_), net_buf.data(), size_t(r),
                                                    tun_buf.data(), tun_buf.size(), &action);
                    if (dec_len > 0 && action == BT_ACTION_WRITE_TO_NETWORK) {
                        send_wg_datagram(udp_sock_, tun_buf.data(), dec_len);
                        // Drain any packets queued while the handshake was in progress.
                        for (;;) {
                            int q_action = BT_ACTION_DONE;
                            const int q_len = boringtun_decapsulate(static_cast<BtTunnel*>(bt_tun_), nullptr, 0,
                                                                    tun_buf.data(), tun_buf.size(), &q_action);
                            if (q_len <= 0 || q_action != BT_ACTION_WRITE_TO_NETWORK) break;
                            send_wg_datagram(udp_sock_, tun_buf.data(), q_len);
                        }
                        continue;
                    }
                }
                if (dec_len > 0 && action == BT_ACTION_WRITE_TO_TUNNEL_IPV4) {
                    deliver_ipv4_packet(tun_buf.data(), size_t(dec_len));
                }
            }
        }

        int timer_len = 0;
        {
            std::lock_guard lock(tun_mu_);
            if (!bt_tun_) break;
            timer_len = boringtun_update_timers(static_cast<BtTunnel*>(bt_tun_), tun_buf.data(), tun_buf.size());
        }
        if (timer_len > 0) send_wg_datagram(udp_sock_, tun_buf.data(), timer_len);
    }
#endif
}

} // namespace rbrew_native

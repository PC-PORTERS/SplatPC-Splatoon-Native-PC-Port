// wg_tunnel.h - Userspace WireGuard P2P tunnel client powered by boringtun.
#pragma once

#include "social_http.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rbrew_native {

struct WgConfig {
    std::string client_ip;
    std::string subnet;
    std::string server_ip;
    std::string nncs2_ip;
    std::string server_public_key;
    std::string endpoint;
    std::string private_key;
    std::string public_server_ip;
    std::string public_nncs1_ip;
    std::string public_nncs2_ip;
    uint16_t persistent_keepalive = 25;
    uint16_t mtu = 1420;
};

// Derives the WireGuard registration path from the register URL path (e.g. "/register" -> "/social/wg").
std::string derive_wg_path(const std::string& register_path);

// Generates a Curve25519 keypair using boringtun. Returns false if boringtun is not compiled in.
bool generate_wg_keypair(std::string& priv_b64_out, std::string& pub_b64_out);

// Requests a WireGuard peer allocation and server endpoint config from POST <wg_path>.
// Sets *out_http_status (0 if unreachable) when non-null.
bool fetch_wg_config(Transport& transport, const std::string& wg_path, const std::string& token,
                     const std::string& public_key, WgConfig& out, std::string& error_out,
                     int* out_http_status = nullptr);

struct WgUdpPacket {
    uint32_t src_ip_host = 0;
    uint16_t src_port_host = 0;
    std::vector<uint8_t> payload;
};

class WgTunnel {
public:
    static WgTunnel& instance();

    bool start(const WgConfig& cfg);
    void stop();

    bool active() const { return active_.load(std::memory_order_acquire); }
    bool contains_ip(uint32_t ip_host) const;
    uint32_t client_ip_host() const { return client_ip_; }
    uint32_t server_ip_host() const { return server_ip_; }
    uint32_t nncs2_ip_host() const { return nncs2_ip_; }
    uint32_t subnet_mask_host() const { return subnet_mask_; }
    std::string server_ip_str() const;
    std::string nncs2_ip_str() const;

    // Rewrites configured public NEX/NNCS destinations to their WireGuard tunnel endpoints.
    uint32_t rewrite_destination(uint32_t dst_ip_host) const;

    void register_udp_socket(int guest_fd, uint16_t local_port_host);
    void unregister_socket(int guest_fd);

    // OS descriptor that becomes readable when the virtual RX queue for guest_fd has data (-1 if none).
    intptr_t wake_fd(int guest_fd) const;
    bool has_pending_rx(int guest_fd) const;

    int send_udp(int guest_fd, uint16_t local_port_host, uint32_t dst_ip_host, uint16_t dst_port_host,
                 const uint8_t* data, size_t len);
    int recv_udp(int guest_fd, uint8_t* out_buf, size_t cap, uint32_t* out_src_ip_host, uint16_t* out_src_port_host,
                 bool nonblocking);

private:
    WgTunnel() = default;
    ~WgTunnel();
    WgTunnel(const WgTunnel&) = delete;
    WgTunnel& operator=(const WgTunnel&) = delete;

    void io_loop();
    void deliver_ipv4_packet(const uint8_t* pkt, size_t len);

    struct SocketQueue {
        uint16_t local_port = 0;
        intptr_t wake_read = -1;
        intptr_t wake_write = -1;
        std::deque<WgUdpPacket> rx;
    };

    std::atomic<bool> active_{false};
    std::atomic<bool> stop_requested_{false};

    uint32_t client_ip_ = 0;
    uint32_t server_ip_ = 0;
    uint32_t nncs2_ip_ = 0;
    uint32_t subnet_net_ = 0;
    uint32_t subnet_mask_ = 0;
    uint32_t public_server_ip_ = 0;
    uint32_t public_nncs1_ip_ = 0;
    uint32_t public_nncs2_ip_ = 0;
    std::string server_ip_str_;
    std::string nncs2_ip_str_;
    std::atomic<uint16_t> ip_id_{1};

    void* bt_tun_ = nullptr; // BtTunnel*
    intptr_t udp_sock_ = -1;
    std::thread io_thread_;

    mutable std::mutex tun_mu_;
    mutable std::mutex rx_mu_;
    std::condition_variable rx_cv_;
    std::unordered_map<int, SocketQueue> sockets_;
};

} // namespace rbrew_native

#pragma once
// How two players find each other: this PC's addresses, opening the host's
// port on the router (UPnP), the public address, and starting the guest
// window from a running game.
#include <cstdint>
#include <string>

namespace rbrew_net {

// The LAN address this PC uses to reach the internet ("" when offline).
std::string local_ip();

// Automatic port forwarding (UPnP Internet Gateway Device), on its own thread.
struct RouterStatus {
    enum class State { Off, Working, Open, Failed };
    State state = State::Off;
    uint16_t port = 0;
    std::string external_ip; // the router's internet address
    std::string local_ip;    // where the router forwards to
    std::string router;      // the router's address (its web page)
    std::string error;
};
void router_open(uint16_t port);
void router_close();    // removes the forwarding in the background
void router_shutdown(); // removes it and waits (at most 2 s); call before the process ends
RouterStatus router_status();

// The address friends on the internet see (asks api.ipify.org, only when called).
void public_ip_lookup();
bool public_ip_busy();
std::string public_ip(); // "" before a successful lookup

// LAN, loopback and carrier-grade NAT (100.64/10) addresses: unreachable from the internet.
bool is_private_ipv4(const std::string& ip);

// "host[:port]" reduced to host-name characters; false when unusable.
bool clean_address(const std::string& text, std::string& out);

// Starts "<this exe> --join <address>" as a new process.
bool launch_guest(const std::string& address);

// Validates an http:// or https:// URL without spaces or quotes, and opens it or a folder.
bool is_valid_http_url(const std::string& url);
bool open_url(const std::string& url);
bool open_folder(const std::string& path);

namespace upnp {
std::string ssdp_location(const std::string& response);
std::string xml_value(const std::string& xml, const std::string& tag);
bool find_wan_service(const std::string& description, const std::string& location, std::string& service_type,
                      std::string& control_url);
// Forwards TCP `port` to this PC (lease in seconds, 0 = until removed).
bool add_mapping(const std::string& location, uint16_t port, uint32_t lease, std::string& external_ip,
                 std::string& local_ip, std::string& error);
bool delete_mapping(const std::string& location, uint16_t port);
// LOCATION of the first gateway that answers, "" when none does.
std::string discover(int timeout_ms);
} // namespace upnp

} // namespace rbrew_net

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
using socket_t = SOCKET;
constexpr socket_t kNoSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <climits>
extern char** environ;
using socket_t = int;
constexpr socket_t kNoSocket = -1;
#endif

#include "netplay_connect.h"
#include "rebrewu/logger.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace rbrew_net {
namespace {

void close_sock(socket_t s) {
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
}

bool sockets_up() {
#if defined(_WIN32)
    static const bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
#else
    return true;
#endif
}

void set_timeout(socket_t s, int ms) {
#if defined(_WIN32)
    const DWORD t = DWORD(ms);
#else
    const timeval t{ms / 1000, (ms % 1000) * 1000};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
}

void set_blocking(socket_t s, bool blocking) {
#if defined(_WIN32)
    u_long mode = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK);
#endif
}

std::string ip_text(const sockaddr_in& addr) {
    char text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr.sin_addr, text, sizeof(text));
    return text;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string xml_unescape(std::string s) {
    static const char* const kEntities[][2] = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
    for (const auto& e : kEntities)
        for (size_t at = s.find(e[0]); at != std::string::npos; at = s.find(e[0], at + 1))
            s.replace(at, std::strlen(e[0]), e[1]);
    return s;
}

// A blocking TCP connection, but connecting gives up after `ms`.
socket_t connect_timeout(const std::string& host, const std::string& port, int ms) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0) return kNoSocket;
    socket_t result = kNoSocket;
    for (addrinfo* a = found; a && result == kNoSocket; a = a->ai_next) {
        const socket_t s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == kNoSocket) continue;
        set_blocking(s, false);
        connect(s, a->ai_addr, int(a->ai_addrlen));
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(s, &writable);
        FD_SET(s, &failed);
        timeval wait{ms / 1000, (ms % 1000) * 1000};
        int error = 1;
        socklen_t length = sizeof(error);
        if (select(int(s + 1), nullptr, &writable, &failed, &wait) > 0 && FD_ISSET(s, &writable) &&
            getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) == 0 && error == 0) {
            set_blocking(s, true);
            set_timeout(s, ms);
            result = s;
        } else {
            close_sock(s);
        }
    }
    freeaddrinfo(found);
    return result;
}

struct Url {
    std::string host, port = "80", path = "/";
};

bool parse_url(const std::string& url, Url& out) {
    if (lower(url.substr(0, 7)) != "http://") return false;
    const std::string rest = url.substr(7);
    const size_t slash = rest.find('/');
    const std::string authority = rest.substr(0, slash);
    out = Url{};
    if (slash != std::string::npos) out.path = rest.substr(slash);
    const size_t colon = authority.rfind(':');
    out.host = authority.substr(0, colon);
    if (colon != std::string::npos) out.port = authority.substr(colon + 1);
    return !out.host.empty() && !out.port.empty();
}

std::string resolve_url(const std::string& base, const std::string& url) {
    if (lower(url.substr(0, 7)) == "http://") return url;
    Url b;
    if (!parse_url(base, b)) return url;
    const std::string root = "http://" + b.host + ":" + b.port;
    if (!url.empty() && url[0] == '/') return root + url;
    return root + b.path.substr(0, b.path.rfind('/') + 1) + url;
}

// Case-insensitive header value from an HTTP head ("" when absent).
std::string header_value(const std::string& head, const std::string& name) {
    const std::string low = lower(head), key = "\n" + lower(name) + ":";
    size_t at = low.find(key);
    if (at == std::string::npos) return "";
    at += key.size();
    const size_t end = head.find_first_of("\r\n", at);
    return trim(head.substr(at, end == std::string::npos ? std::string::npos : end - at));
}

std::string dechunk(const std::string& body) {
    std::string out;
    size_t at = 0;
    while (at < body.size()) {
        const size_t eol = body.find("\r\n", at);
        if (eol == std::string::npos) break;
        const unsigned long size = std::strtoul(body.substr(at, eol - at).c_str(), nullptr, 16);
        if (size == 0) break;
        at = eol + 2;
        out.append(body, at, size);
        at += size + 2;
    }
    return out;
}

struct Reply {
    int status = 0;
    std::string body;
    std::string local_ip; // this PC's address on the connection
};

bool http(const std::string& url, const char* method, const std::string& headers, const std::string& body, Reply& reply,
          int ms = 4000) {
    Url u;
    if (!sockets_up() || !parse_url(url, u)) return false;
    const socket_t s = connect_timeout(u.host, u.port, ms);
    if (s == kNoSocket) return false;
    sockaddr_in self{};
    socklen_t self_length = sizeof(self);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&self), &self_length) == 0) reply.local_ip = ip_text(self);
    std::string request = std::string(method) + " " + u.path + " HTTP/1.1\r\nHost: " + u.host +
                          (u.port == "80" ? "" : ":" + u.port) + "\r\nConnection: close\r\nUser-Agent: SplatPC\r\n" +
                          headers;
    if (std::strcmp(method, "POST") == 0) request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    request += "\r\n" + body;
#ifdef MSG_NOSIGNAL
    constexpr int kSendFlags = MSG_NOSIGNAL;
#else
    constexpr int kSendFlags = 0;
#endif
    bool ok = true;
    for (size_t sent = 0; ok && sent < request.size();) {
        const int n = send(s, request.data() + sent, int(request.size() - sent), kSendFlags);
        ok = n > 0;
        sent += ok ? size_t(n) : 0;
    }
    std::string data;
    size_t split = std::string::npos, wanted = std::string::npos;
    char buffer[4096];
    while (ok && data.size() < (1u << 20)) {
        const int n = recv(s, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        data.append(buffer, size_t(n));
        if (split == std::string::npos && (split = data.find("\r\n\r\n")) != std::string::npos) {
            const std::string length = header_value(data.substr(0, split + 2), "content-length");
            if (!length.empty()) wanted = split + 4 + std::strtoul(length.c_str(), nullptr, 10);
        }
        if (data.size() >= wanted) break; // some routers keep the connection open
    }
    close_sock(s);
    split = data.find("\r\n\r\n");
    const size_t space = data.find(' ');
    if (!ok || split == std::string::npos || data.compare(0, 5, "HTTP/") != 0 || space > split) return false;
    const std::string head = data.substr(0, split + 2);
    reply.status = std::atoi(data.c_str() + space + 1);
    reply.body = data.substr(split + 4);
    if (lower(header_value(head, "transfer-encoding")).find("chunked") != std::string::npos)
        reply.body = dechunk(reply.body);
    return true;
}

bool soap(const std::string& control_url, const std::string& service, const std::string& action,
          const std::string& args, Reply& reply) {
    const std::string body =
        "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" +
        action + " xmlns:u=\"" + service + "\">" + args + "</u:" + action + "></s:Body></s:Envelope>\r\n";
    const std::string headers =
        "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + service + "#" + action + "\"\r\n";
    return http(control_url, "POST", headers, body, reply);
}

struct Gateway {
    std::string service, control, local_ip;
};

bool open_gateway(const std::string& location, Gateway& gateway, std::string& error) {
    Reply description;
    if (!http(location, "GET", "", "", description) || description.status != 200) {
        error = "The router did not send its UPnP description.";
        return false;
    }
    if (!upnp::find_wan_service(description.body, location, gateway.service, gateway.control)) {
        error = "The router does not offer port forwarding over UPnP.";
        return false;
    }
    gateway.local_ip = description.local_ip;
    return true;
}

std::string mapping_key(uint16_t port) {
    return "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) +
           "</NewExternalPort><NewProtocol>TCP</NewProtocol>";
}
} // namespace

namespace upnp {

std::string ssdp_location(const std::string& response) {
    if (response.compare(0, 5, "HTTP/") != 0 && response.compare(0, 6, "NOTIFY") != 0) return "";
    const std::string location = header_value(response, "location");
    return lower(location.substr(0, 7)) == "http://" ? location : "";
}

// Text of the first <tag>, <tag ...> or <prefix:tag> element.
std::string xml_value(const std::string& xml, const std::string& tag) {
    for (size_t at = xml.find(tag); at != std::string::npos; at = xml.find(tag, at + 1)) {
        const size_t after = at + tag.size();
        if (at == 0 || after >= xml.size() || (xml[after] != '>' && xml[after] != ' ')) continue;
        if (xml[at - 1] == ':') {
            const size_t open = xml.rfind('<', at);
            if (open == std::string::npos || xml[open + 1] == '/' || xml.find('>', open) < at) continue;
        } else if (xml[at - 1] != '<') {
            continue;
        }
        const size_t close = xml.find('>', after);
        if (close == std::string::npos) break;
        if (xml[close - 1] == '/') return "";
        const size_t end = xml.find('<', close + 1);
        if (end == std::string::npos) break;
        return xml_unescape(trim(xml.substr(close + 1, end - close - 1)));
    }
    return "";
}

bool find_wan_service(const std::string& description, const std::string& location, std::string& service_type,
                      std::string& control_url) {
    const std::string url_base = xml_value(description, "URLBase");
    const std::string base = url_base.empty() ? location : url_base;
    std::string ppp_type, ppp_url;
    for (size_t at = description.find("<service>"); at != std::string::npos;
         at = description.find("<service>", at + 1)) {
        const size_t end = description.find("</service>", at);
        if (end == std::string::npos) break;
        const std::string block = description.substr(at, end - at);
        const std::string type = xml_value(block, "serviceType"), url = xml_value(block, "controlURL");
        if (url.empty()) continue;
        if (type.find(":service:WANIPConnection:") != std::string::npos) {
            service_type = type;
            control_url = resolve_url(base, url);
            return true;
        }
        if (type.find(":service:WANPPPConnection:") != std::string::npos && ppp_type.empty()) {
            ppp_type = type;
            ppp_url = url;
        }
    }
    if (ppp_type.empty()) return false;
    service_type = ppp_type;
    control_url = resolve_url(base, ppp_url);
    return true;
}

bool add_mapping(const std::string& location, uint16_t port, uint32_t lease, std::string& external_ip,
                 std::string& local_ip, std::string& error) {
    Gateway g;
    if (!open_gateway(location, g, error)) return false;
    local_ip = g.local_ip;
    const std::string p = std::to_string(port);
    for (const uint32_t attempt : {lease, 0u}) {
        const std::string args = mapping_key(port) + "<NewInternalPort>" + p + "</NewInternalPort><NewInternalClient>" +
                                 g.local_ip +
                                 "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>Splatoon "
                                 "co-op</NewPortMappingDescription><NewLeaseDuration>" +
                                 std::to_string(attempt) + "</NewLeaseDuration>";
        Reply reply;
        if (!soap(g.control, g.service, "AddPortMapping", args, reply)) {
            error = "The router did not answer.";
            return false;
        }
        const std::string code = xml_value(reply.body, "errorCode");
        bool mapped = reply.status == 200;
        if (code == "718") { // already forwarded: fine when it points at this PC
            Reply entry;
            mapped = soap(g.control, g.service, "GetSpecificPortMappingEntry", mapping_key(port), entry) &&
                     entry.status == 200 && xml_value(entry.body, "NewInternalClient") == g.local_ip &&
                     xml_value(entry.body, "NewInternalPort") == p;
        }
        if (mapped) {
            Reply ip;
            if (soap(g.control, g.service, "GetExternalIPAddress", "", ip) && ip.status == 200)
                external_ip = xml_value(ip.body, "NewExternalIPAddress");
            error.clear();
            return true;
        }
        if (code == "725" && attempt != 0) continue; // the router only keeps permanent forwardings
        if (code == "718")
            error = "Port " + p + " is already forwarded to another device. Choose another port.";
        else if (code == "606" || code == "401")
            error = "The router does not let programs forward ports.";
        else
            error = "The router refused the forwarding (" +
                    (code.empty() ? "HTTP " + std::to_string(reply.status) : "UPnP error " + code) + ").";
        return false;
    }
    return false;
}

bool delete_mapping(const std::string& location, uint16_t port) {
    Gateway g;
    std::string error;
    Reply reply;
    return open_gateway(location, g, error) &&
           soap(g.control, g.service, "DeletePortMapping", mapping_key(port), reply) && reply.status == 200;
}

std::string discover(int timeout_ms) {
    if (!sockets_up()) return "";
    const socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) return "";
    // Ask on the adapter that reaches the internet, not on a VPN or virtual one.
    sockaddr_in self{};
    self.sin_family = AF_INET;
    if (const std::string mine = local_ip(); !mine.empty() && inet_pton(AF_INET, mine.c_str(), &self.sin_addr) == 1) {
        const in_addr adapter = self.sin_addr;
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&adapter), sizeof(adapter));
    }
    bind(s, reinterpret_cast<sockaddr*>(&self), sizeof(self));
    const int ttl = 2;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
    set_timeout(s, 250);
    sockaddr_in group{};
    group.sin_family = AF_INET;
    group.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &group.sin_addr);
    static const char* const kTargets[] = {
        "urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
        "urn:schemas-upnp-org:service:WANIPConnection:1", "urn:schemas-upnp-org:service:WANPPPConnection:1"};
    std::string location;
    const auto start = std::chrono::steady_clock::now();
    auto next_search = start;
    while (location.empty() && std::chrono::steady_clock::now() - start < std::chrono::milliseconds(timeout_ms)) {
        if (std::chrono::steady_clock::now() >= next_search) { // again every second: UDP gets lost
            for (const char* target : kTargets) {
                const std::string search = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: "
                                                       "\"ssdp:discover\"\r\nMX: 2\r\nST: ") +
                                           target + "\r\n\r\n";
                sendto(s, search.data(), int(search.size()), 0, reinterpret_cast<const sockaddr*>(&group),
                       sizeof(group));
            }
            next_search += std::chrono::seconds(1);
        }
        char buffer[2048];
        const int n = recvfrom(s, buffer, sizeof(buffer), 0, nullptr, nullptr);
        if (n <= 0) continue;
        // Only gateways: other UPnP devices (TVs, NAS) answer M-SEARCH too.
        const std::string reply = lower(std::string(buffer, size_t(n)));
        if (reply.find("internetgatewaydevice") != std::string::npos ||
            reply.find("wanipconnection") != std::string::npos || reply.find("wanpppconnection") != std::string::npos)
            location = ssdp_location(std::string(buffer, size_t(n)));
    }
    close_sock(s);
    return location;
}
} // namespace upnp

std::string local_ip() {
    if (!sockets_up()) return "";
    const socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) return "";
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &target.sin_addr); // nothing is sent; this only picks the route
    std::string ip;
    if (connect(s, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) {
        sockaddr_in self{};
        socklen_t length = sizeof(self);
        if (getsockname(s, reinterpret_cast<sockaddr*>(&self), &length) == 0) ip = ip_text(self);
    }
    close_sock(s);
    return ip == "0.0.0.0" ? "" : ip;
}

namespace {
struct Router {
    std::mutex mutex;
    RouterStatus status;
    std::string location;
    uint64_t generation = 0;
};
Router& router() {
    static Router* r = new Router; // detached threads may outlive static destruction
    return *r;
}
constexpr uint32_t kLeaseSeconds = 3600;

void forward_port(uint64_t generation, uint16_t port) {
    Router& r = router();
    const auto current = [&] {
        std::lock_guard lock(r.mutex);
        return r.generation == generation;
    };
    const std::string location = upnp::discover(3000);
    if (location.empty()) {
        std::lock_guard lock(r.mutex);
        if (r.generation != generation) return;
        r.status.state = RouterStatus::State::Failed;
        r.status.error = "No router answered. UPnP may be turned off in the router's settings.";
        return;
    }
    std::string external, local, error;
    const bool ok = upnp::add_mapping(location, port, kLeaseSeconds, external, local, error);
    {
        std::lock_guard lock(r.mutex);
        if (r.generation == generation) {
            Url u;
            parse_url(location, u);
            r.location = location;
            r.status.router = u.host;
            r.status.local_ip = local;
            r.status.external_ip = external;
            r.status.error = error;
            r.status.state = ok ? RouterStatus::State::Open : RouterStatus::State::Failed;
        }
    }
    if (ok && !current()) {
        upnp::delete_mapping(location, port); // closed while it was opening
        return;
    }
    RBREW_LOG_INFO("net", "router port forwarding %s\n", ok ? "open" : error.c_str());
    while (ok) {
        for (uint32_t i = 0; i < kLeaseSeconds / 2; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!current()) return;
        }
        upnp::add_mapping(location, port, kLeaseSeconds, external, local, error);
    }
}

// The forwarding goes away with the game (the lease also ends it after a crash).
void close_at_exit() {
    Router& r = router();
    std::string location;
    uint16_t port = 0;
    {
        std::lock_guard lock(r.mutex);
        ++r.generation;
        if (r.status.state != RouterStatus::State::Open) return;
        location = r.location;
        port = r.status.port;
        r.status.state = RouterStatus::State::Off;
    }
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread([location, port, done] {
        upnp::delete_mapping(location, port);
        done->store(true);
    }).detach();
    for (int i = 0; i < 200 && !done->load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

struct PublicIp {
    std::mutex mutex;
    std::string ip;
    std::atomic<bool> busy{false};
};
PublicIp& public_address() {
    static PublicIp* p = new PublicIp;
    return *p;
}
} // namespace

void router_open(uint16_t port) {
    static std::once_flag registered;
    std::call_once(registered, [] { std::atexit(close_at_exit); });
    Router& r = router();
    uint64_t generation;
    {
        std::lock_guard lock(r.mutex);
        generation = ++r.generation;
        r.status = RouterStatus{};
        r.status.state = RouterStatus::State::Working;
        r.status.port = port;
    }
    std::thread(forward_port, generation, port).detach();
}

void router_close() {
    Router& r = router();
    std::string location;
    uint16_t port = 0;
    {
        std::lock_guard lock(r.mutex);
        ++r.generation;
        if (r.status.state == RouterStatus::State::Open) {
            location = r.location;
            port = r.status.port;
        }
        r.status = RouterStatus{};
    }
    if (!location.empty()) std::thread([location, port] { upnp::delete_mapping(location, port); }).detach();
}

void router_shutdown() {
    close_at_exit();
}

RouterStatus router_status() {
    Router& r = router();
    std::lock_guard lock(r.mutex);
    return r.status;
}

void public_ip_lookup() {
    PublicIp& p = public_address();
    if (p.busy.exchange(true)) return;
    std::thread([&p] {
        Reply reply;
        std::string ip;
        in_addr parsed{};
        if (http("http://api.ipify.org/", "GET", "", "", reply) && reply.status == 200) ip = trim(reply.body);
        if (inet_pton(AF_INET, ip.c_str(), &parsed) != 1) ip.clear();
        {
            std::lock_guard lock(p.mutex);
            p.ip = ip;
        }
        p.busy.store(false);
    }).detach();
}

bool public_ip_busy() {
    return public_address().busy.load();
}

std::string public_ip() {
    PublicIp& p = public_address();
    std::lock_guard lock(p.mutex);
    return p.ip;
}

bool is_private_ipv4(const std::string& ip) {
    in_addr parsed{};
    if (inet_pton(AF_INET, ip.c_str(), &parsed) != 1) return false;
    const uint32_t v = ntohl(parsed.s_addr);
    const auto in = [v](uint32_t net, int bits) { return (v >> (32 - bits)) == (net >> (32 - bits)); };
    return in(0x0A000000u, 8) || in(0xAC100000u, 12) || in(0xC0A80000u, 16) || in(0x64400000u, 10) ||
           in(0x7F000000u, 8) || in(0xA9FE0000u, 16);
}

bool clean_address(const std::string& text, std::string& out) {
    std::string s;
    for (const char c : text)
        if (!std::isspace(static_cast<unsigned char>(c))) s += c;
    if (lower(s.substr(0, 7)) == "http://") s = s.substr(7);
    while (!s.empty() && s.back() == '/') s.pop_back();
    if (s.empty() || s.size() > 253) return false;
    for (const char c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != ':') return false;
    if (const size_t colon = s.find(':'); colon != std::string::npos) {
        const std::string port = s.substr(colon + 1);
        if (colon == 0 || port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos)
            return false;
        const unsigned long number = std::strtoul(port.c_str(), nullptr, 10);
        if (number == 0 || number > 65535) return false;
    }
    out = s;
    return true;
}

bool launch_guest(const std::string& address) {
    std::string clean;
    if (!clean_address(address, clean)) return false;
#if defined(_WIN32)
    std::wstring exe(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size()));
    if (!length || length >= exe.size()) return false;
    exe.resize(length);
    std::wstring command = L"\"" + exe + L"\" --join " + std::wstring(clean.begin(), clean.end());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
        return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#else
    char exe[PATH_MAX] = {};
    const ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (length <= 0) return false;
    char join[] = "--join";
    char* argv[] = {exe, join, clean.data(), nullptr};
    pid_t pid;
    return posix_spawn(&pid, exe, nullptr, nullptr, argv, environ) == 0;
#endif
}

bool is_valid_http_url(const std::string& url) {
    const std::string low = lower(url);
    const size_t prefix = low.rfind("https://", 0) == 0 ? 8 : (low.rfind("http://", 0) == 0 ? 7 : 0);
    if (!prefix || url.size() <= prefix || url.size() > 512) return false;
    for (const char c : url)
        if (std::isspace(static_cast<unsigned char>(c)) || c == '"' || c == '\'' ||
            static_cast<unsigned char>(c) < 0x20)
            return false;
    return true;
}

namespace {
bool shell_open(const std::string& target) {
#if defined(_WIN32)
    const int n = MultiByteToWideChar(CP_UTF8, 0, target.c_str(), -1, nullptr, 0);
    if (n <= 0) return false;
    std::wstring wide(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, target.c_str(), -1, wide.data(), n);
    return reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) >
           32;
#else
    char cmd[] = "xdg-open";
    std::string copy = target;
    char* argv[] = {cmd, copy.data(), nullptr};
    pid_t pid;
    return posix_spawn(&pid, "/usr/bin/xdg-open", nullptr, nullptr, argv, environ) == 0 ||
           posix_spawnp(&pid, cmd, nullptr, nullptr, argv, environ) == 0;
#endif
}
} // namespace

bool open_url(const std::string& url) {
    return is_valid_http_url(url) && shell_open(url);
}

bool open_folder(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    const std::filesystem::path p(std::u8string(path.begin(), path.end()));
    std::filesystem::create_directories(p, ec);
    const std::filesystem::path abs = std::filesystem::weakly_canonical(p, ec);
    const auto utf8 = (ec ? p : abs).u8string();
    return shell_open(std::string(utf8.begin(), utf8.end()));
}

} // namespace rbrew_net

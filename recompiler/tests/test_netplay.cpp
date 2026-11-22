#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kNoSocket = INVALID_SOCKET;
static void close_socket(socket_t s) {
    closesocket(s);
}
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kNoSocket = -1;
static void close_socket(socket_t s) {
    close(s);
}
#endif

#define SDL_MAIN_HANDLED
#include "runtime/netplay.h"
#include "runtime/netplay_connect.h"
#include "runtime/netplay_video.h"
#include "os/padscore/pro_pad.h"
#include "rebrewu/logger.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <vector>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace rbrew_net;

namespace {

void require(bool ok, const char* what) {
    if (ok) return;
    RBREW_LOG_ERROR("netplay", "FAIL: %s\n", what);
    std::exit(1);
}

ProPad sample_pad() {
    ProPad pad;
    pad.hold = 0x00002450u;
    pad.lx = 0.25f;
    pad.ly = -1.0f;
    pad.rx = 0.0f;
    pad.ry = 0.75f;
    return pad;
}

void test_wire_format() {
    uint8_t header[kHeaderSize];
    encode_header(Message::Pad, kPadSize, header);
    Message type;
    size_t size = 0;
    require(decode_header(header, type, size) && type == Message::Pad && size == kPadSize, "header round trip");

    uint8_t hello[kHelloSize];
    encode_hello(hello);
    require(decode_hello(hello, kHelloSize), "hello accepted");
    hello[4] ^= 1;
    require(!decode_hello(hello, kHelloSize), "other protocol version rejected");

    uint8_t payload[kPadSize];
    encode_pad(sample_pad(), payload);
    ProPad decoded;
    require(decode_pad(payload, kPadSize, decoded) && decoded == sample_pad(), "pad round trip");
    require(!decode_pad(payload, kPadSize - 1, decoded), "short pad rejected");
    const float out_of_range = 2.0f;
    std::memcpy(payload + 4, &out_of_range, 4);
    require(!decode_pad(payload, kPadSize, decoded), "stick outside -1..1 rejected");
}

socket_t connect_local(uint16_t port) {
    const socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    require(connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "connect to host");
    return s;
}

void send_bytes(socket_t s, const uint8_t* data, size_t size) {
    require(send(s, reinterpret_cast<const char*>(data), int(size), 0) == int(size), "send");
}

void send_message(socket_t s, Message type, const uint8_t* payload, size_t size) {
    uint8_t header[kHeaderSize];
    encode_header(type, size, header);
    send_bytes(s, header, kHeaderSize);
    send_bytes(s, payload, size);
}

template <class F> bool wait_for(F condition) {
    for (int i = 0; i < 300; ++i) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void test_host_receives_pad() {
    require(host_start(0) && host_port() != 0, "host listens on a free port");
    ProPad pad;
    require(!host_remote_pad(pad), "no remote pad before a guest joins");

    const socket_t s = connect_local(host_port());
    uint8_t hello[kHelloSize];
    encode_hello(hello);
    send_message(s, Message::Hello, hello, kHelloSize);
    uint8_t reply[kHeaderSize + kHelloSize];
    int got = 0;
    while (got < int(sizeof(reply))) {
        const int n = recv(s, reinterpret_cast<char*>(reply) + got, int(sizeof(reply)) - got, 0);
        require(n > 0, "host answers the handshake");
        got += n;
    }
    require(decode_hello(reply + kHeaderSize, kHelloSize), "host hello valid");
    require(wait_for([] { return host_guest_connected(); }), "guest counted as connected");

    uint8_t payload[kPadSize];
    encode_pad(sample_pad(), payload);
    send_message(s, Message::Pad, payload, kPadSize);
    require(wait_for([&] { return host_remote_pad(pad) && pad == sample_pad(); }), "host sees the guest's pad");

    close_socket(s);
    require(wait_for([] { return !host_guest_connected(); }), "disconnect noticed");
    require(!host_remote_pad(pad), "no remote pad after the guest left");
}

void test_wrong_version_rejected() {
    const socket_t s = connect_local(host_port());
    uint8_t hello[kHelloSize];
    encode_hello(hello);
    hello[4] ^= 1;
    send_message(s, Message::Hello, hello, kHelloSize);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    require(!host_guest_connected(), "guest with another version is not Player 2");
    close_socket(s);
    host_stop();
    require(host_port() == 0, "host stopped");
}

void test_video_round_trip(uint32_t w, uint32_t h, uint32_t kbps) {
#if defined(_WIN32)
    H264Encoder encoder;
    H264Decoder decoder;
    require(encoder.open(w, h, 60, kbps), "H.264 encoder opens");
    require(decoder.open(), "H.264 decoder opens");
    std::vector<uint8_t> rgba(size_t(w) * h * 4), nv12(size_t(w) * h * 3 / 2), picture;
    uint32_t pw = 0, ph = 0;
    int pictures = 0, encoded = 0;
    double error = 0, convert_ms = 0, encode_ms = 0, decode_ms = 0;
    for (int frame = 0; frame < 30; ++frame) {
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
                p[0] = uint8_t((x + frame * 4) / 6);
                p[1] = uint8_t(y / 3);
                p[2] = uint8_t(128 + ((x / 64 + y / 64) % 2) * 60);
                p[3] = 255;
            }
        const auto t0 = std::chrono::steady_clock::now();
        rgba_to_nv12(rgba.data(), w, h, w * 4, nv12.data());
        const auto t1 = std::chrono::steady_clock::now();
        std::vector<uint8_t> bytes;
        require(encoder.encode(nv12.data(), frame == 0, bytes), "frame encodes");
        const auto t2 = std::chrono::steady_clock::now();
        if (bytes.empty()) continue;
        ++encoded;
        require(bytes.size() < nv12.size() / 4, "frame is compressed");
        const bool got = decoder.decode(bytes.data(), bytes.size(), picture, pw, ph);
        const auto t3 = std::chrono::steady_clock::now();
        using ms = std::chrono::duration<double, std::milli>;
        convert_ms += ms(t1 - t0).count();
        encode_ms += ms(t2 - t1).count();
        decode_ms += ms(t3 - t2).count();
        if (got) {
            ++pictures;
            if (pw != w || ph != h)
                RBREW_LOG_ERROR("netplay", "size mismatch: got %ux%u expected %ux%u\n", pw, ph, w, h);
            require(pw == w && ph == h, "decoded size matches");
            double sum = 0;
            for (size_t i = 0; i < size_t(w) * h; ++i) sum += std::abs(int(picture[i]) - int(nv12[i]));
            error = sum / (double(w) * h);
        }
    }
    RBREW_LOG_INFO("netplay", "video: %d of 30 frames encoded, %d pictures decoded\n", encoded, pictures);
    require(encoded >= 29 && pictures >= encoded - 1, "decoder keeps up with the encoder");
    require(error < 8.0, "decoded luma close to the source");
    RBREW_LOG_INFO(
        "netplay",
        "video: %d pictures, last luma error %.2f; per frame: convert %.1f ms, encode %.1f ms, decode %.1f ms\n",
        pictures, error, convert_ms / 30, encode_ms / 30, decode_ms / 30);
#endif
}

void test_keyboard_mapping() {
    int keys_bound[rbrew_settings::kActionCount], pad_bound[rbrew_settings::kActionCount];
    rbrew_settings::default_bindings(keys_bound, pad_bound);
    uint8_t keys[512] = {};
    ProPad pad = pro_pad_from_keyboard(keys, 512, 0, 0, 0, keys_bound, 1.0f);
    require(pad == ProPad{}, "nothing pressed gives a neutral pad");

    keys[26] = 1; // W
    pad = pro_pad_from_keyboard(keys, 512, 0, 0, 0, keys_bound, 1.0f);
    require(pad.ly == 1.0f && pad.lx == 0.0f && pad.hold == 0, "W pushes the left stick up");
    keys[7] = 1; // D
    pad = pro_pad_from_keyboard(keys, 512, 0, 0, 0, keys_bound, 1.0f);
    require(std::fabs(pad.lx - 0.7071f) < 1e-4f && std::fabs(pad.ly - 0.7071f) < 1e-4f, "W+D moves diagonally");
    keys[26] = keys[7] = 0;

    keys[44] = 1;                                                            // Space = B
    pad = pro_pad_from_keyboard(keys, 512, 1u << 0, 0, 0, keys_bound, 1.0f); // left mouse = ZR
    require(pad.hold == (0x40u | 0x04u), "Space presses B, left click presses ZR");
    keys[44] = 0;

    pad = pro_pad_from_keyboard(keys, 512, 0, 6000.0f, -3000.0f, keys_bound, 1.0f);
    require(std::fabs(pad.rx - 0.5f) < 1e-4f && std::fabs(pad.ry - 0.25f) < 1e-4f, "mouse right/up aims right/up");
    pad = pro_pad_from_keyboard(keys, 512, 0, 1e6f, 0, keys_bound, 1.0f);
    require(pad.rx == 1.0f, "fast mouse is clamped to a full stick");
    pad = pro_pad_from_keyboard(keys, 512, 0, 6000.0f, 0, keys_bound, 2.0f);
    require(std::fabs(pad.rx - 1.0f) < 1e-4f, "mouse sensitivity scales the stick");

    ProPad controller;
    controller.hold = 0x10u;
    controller.lx = -0.3f;
    controller.rx = 0.8f;
    ProPad keyboard;
    keyboard.hold = 0x40u;
    keyboard.ly = 1.0f;
    keyboard.rx = 0.5f;
    const ProPad merged = merge_pads(controller, keyboard);
    require(merged.hold == 0x50u, "controller and keyboard buttons add up");
    require(merged.lx == 0.0f && merged.ly == 1.0f, "moving by key wins over the left stick");
    require(merged.rx == 1.0f, "mouse adds to the right stick, clamped");
    keyboard = ProPad{};
    require(merge_pads(controller, keyboard).lx == -0.3f, "idle keyboard keeps the controller's stick");
}

bool recv_exact(socket_t s, uint8_t* data, size_t size);

void test_audio_wire_format() {
    const float stereo[8] = {0.0f, -0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 3.0f, -3.0f};
    std::vector<uint8_t> bytes;
    encode_audio(stereo, 4, bytes);
    require(bytes.size() == 16, "4 stereo frames are 16 bytes");
    std::vector<int16_t> samples;
    require(decode_audio(bytes.data(), bytes.size(), samples) && samples.size() == 8, "audio round trip");
    require(samples[0] == 0 && samples[2] == 16384 && samples[3] == -16384, "half scale");
    require(samples[4] == 32767 && samples[5] == -32767, "full scale");
    require(samples[6] == 32767 && samples[7] == -32767, "overload is clamped");
    require(!decode_audio(bytes.data(), 15, samples), "partial frame rejected");
}

// Sound reaches a connected guest; nothing is queued while nobody is connected.
void test_host_streams_audio() {
    require(host_start(0), "host starts for sound");
    const float silence_before[64] = {};
    host_submit_audio(silence_before, 32); // no guest yet: dropped
    const socket_t s = connect_local(host_port());
    uint8_t hello[kHelloSize];
    encode_hello(hello);
    send_message(s, Message::Hello, hello, kHelloSize);
    uint8_t header[kHeaderSize], reply[kHelloSize];
    require(recv_exact(s, header, kHeaderSize) && recv_exact(s, reply, kHelloSize), "handshake reply");
    require(wait_for([] { return host_guest_connected(); }), "guest connected");

    float tone[2 * 320];
    for (int i = 0; i < 320; ++i) tone[i * 2] = tone[i * 2 + 1] = 0.25f * float(std::sin(i * 0.2));
    host_submit_audio(tone, 320);
    Message type;
    size_t size = 0;
    require(recv_exact(s, header, kHeaderSize) && decode_header(header, type, size), "audio header");
    require(type == Message::Audio && size == 320 * 4, "first message is exactly the submitted sound");
    std::vector<uint8_t> payload(size);
    require(recv_exact(s, payload.data(), size), "audio payload");
    std::vector<int16_t> samples;
    require(decode_audio(payload.data(), size, samples), "guest decodes sound");
    require(std::abs(samples[20] - int(std::lround(0.25 * std::sin(10 * 0.2) * 32767))) <= 1, "samples survive");
    close_socket(s);
    host_stop();
}

bool recv_exact(socket_t s, uint8_t* data, size_t size) {
    while (size) {
        const int n = recv(s, reinterpret_cast<char*>(data), int(size), 0);
        if (n <= 0) return false;
        data += n;
        size -= size_t(n);
    }
    return true;
}

// Host frames reach a connected guest as H.264 it can decode.
void test_host_streams_video() {
#if defined(_WIN32)
    require(host_start(0), "host starts for streaming");
    const socket_t s = connect_local(host_port());
    uint8_t hello[kHelloSize];
    encode_hello(hello);
    send_message(s, Message::Hello, hello, kHelloSize);
    uint8_t header[kHeaderSize], reply[kHelloSize];
    require(recv_exact(s, header, kHeaderSize) && recv_exact(s, reply, kHelloSize), "handshake reply");
    require(wait_for([] { return host_wants_video(); }), "host wants video once a guest is connected");

    std::vector<uint8_t> rgba(size_t(kStreamWidth) * kStreamHeight * 4, 0);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = 200;
        rgba[i + 1] = 40;
        rgba[i + 2] = 90;
    }
    H264Decoder decoder;
    require(decoder.open(), "guest decoder opens");
    std::vector<uint8_t> payload, picture;
    uint32_t w = 0, h = 0;
    bool decoded = false;
    for (int frame = 0; frame < 10 && !decoded; ++frame) {
        require(wait_for([] { return host_wants_video(); }), "encoder ready for the next frame");
        host_submit_frame(rgba.data());
        Message type;
        size_t size = 0;
        require(recv_exact(s, header, kHeaderSize) && decode_header(header, type, size), "video header");
        require(type == Message::Video, "host sends video");
        payload.resize(size);
        require(recv_exact(s, payload.data(), size), "video payload");
        decoded = decoder.decode(payload.data(), size, picture, w, h);
    }
    require(decoded && w == kStreamWidth && h == kStreamHeight, "guest decodes the stream");
    // Solid colour: luma of (200, 40, 90) is about 95 in BT.601 limited range.
    require(std::abs(int(picture[size_t(h / 2) * w + w / 2]) - 95) < 6, "picture colour survives");
    close_socket(s);
    host_stop();
#endif
}

void test_connect_helpers() {
    require(is_private_ipv4("192.168.1.5"), "RFC1918 192.168 is private");
    require(is_private_ipv4("10.0.0.1"), "RFC1918 10/8 is private");
    require(is_private_ipv4("172.16.4.2"), "RFC1918 172.16/12 is private");
    require(is_private_ipv4("100.64.1.2"), "CGNAT 100.64/10 is private");
    require(is_private_ipv4("127.0.0.1"), "loopback is private");
    require(!is_private_ipv4("203.0.113.7"), "public IPv4 is not private");
    require(!is_private_ipv4("not-an-ip"), "invalid IP is not private");

    std::string out;
    require(clean_address("  203.0.113.7:52815  ", out) && out == "203.0.113.7:52815", "clean trims spaces");
    require(clean_address("http://example.com:1234/", out) && out == "example.com:1234", "clean strips scheme/slash");
    require(clean_address("friend.local", out) && out == "friend.local", "clean accepts bare host");
    require(!clean_address("", out), "empty address rejected");
    require(!clean_address("203.0.113.7:0", out), "port 0 rejected");
    require(!clean_address("203.0.113.7:70000", out), "port > 65535 rejected");
    require(!clean_address("bad host/path", out), "slash path rejected");

    require(is_valid_http_url("http://192.168.1.1"), "http router URL accepted");
    require(is_valid_http_url("https://example.com:8443/path"), "https URL accepted");
    require(!is_valid_http_url("http://"), "bare scheme rejected");
    require(!is_valid_http_url("file:///C:/Windows"), "file URL rejected");
    require(!is_valid_http_url("http://192.168.1.1/a b"), "URL with space rejected");
    require(!open_url("file:///C:/Windows") && !open_folder(""), "invalid url/folder rejected");

    const std::string ssdp = "HTTP/1.1 200 OK\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
                             "Location: http://192.168.1.1:5000/rootDesc.xml\r\n\r\n";
    require(upnp::ssdp_location(ssdp) == "http://192.168.1.1:5000/rootDesc.xml", "SSDP location parsed");
    require(upnp::ssdp_location("GARBAGE\r\nLocation: http://1.2.3.4/x\r\n\r\n").empty(), "non-HTTP SSDP ignored");

    require(upnp::xml_value("<u:GetExternalIPAddressResponse><NewExternalIPAddress>203.0.113.7</NewExternalIPAddress></"
                            "u:GetExternalIPAddressResponse>",
                            "NewExternalIPAddress") == "203.0.113.7",
            "XML value parsed");
    require(upnp::xml_value("<root><ns:errorCode>718</ns:errorCode></root>", "errorCode") == "718",
            "namespaced XML value parsed");

    const std::string desc = "<root><device><serviceList><service>"
                             "<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
                             "<controlURL>/ctl/IPConn</controlURL>"
                             "</service></serviceList></device></root>";
    std::string st, ctl;
    require(upnp::find_wan_service(desc, "http://192.168.1.1:5000/rootDesc.xml", st, ctl), "WAN service found");
    require(st == "urn:schemas-upnp-org:service:WANIPConnection:1" && ctl == "http://192.168.1.1:5000/ctl/IPConn",
            "WAN control URL resolved");
}

void test_upnp_mock_round_trip() {
    const socket_t srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    socklen_t len = sizeof(addr);
    require(bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 && listen(srv, 4) == 0 &&
                getsockname(srv, reinterpret_cast<sockaddr*>(&addr), &len) == 0,
            "mock UPnP server listens");
    const uint16_t port = ntohs(addr.sin_port);
    std::atomic<int> seen_add{0}, seen_del{0};
    std::thread server([&] {
        for (int req = 0; req < 5; ++req) {
            const socket_t c = accept(srv, nullptr, nullptr);
            if (c == kNoSocket) break;
            char buf[4096];
            const int n = recv(c, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                std::string r(buf, size_t(n)), body;
                if (r.find("GET /root.xml") != std::string::npos) {
                    body = "<root><device><serviceList><service>"
                           "<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
                           "<controlURL>/ctl</controlURL></service></serviceList></device></root>";
                } else if (r.find("#AddPortMapping\"") != std::string::npos) {
                    seen_add.fetch_add(1);
                    body = "<s:Envelope><s:Body><u:AddPortMappingResponse/></s:Body></s:Envelope>";
                } else if (r.find("#GetExternalIPAddress\"") != std::string::npos) {
                    body = "<s:Envelope><s:Body><u:GetExternalIPAddressResponse>"
                           "<NewExternalIPAddress>203.0.113.42</NewExternalIPAddress>"
                           "</u:GetExternalIPAddressResponse></s:Body></s:Envelope>";
                } else if (r.find("#DeletePortMapping\"") != std::string::npos) {
                    seen_del.fetch_add(1);
                    body = "<s:Envelope><s:Body><u:DeletePortMappingResponse/></s:Body></s:Envelope>";
                }
                const std::string resp = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                                         "\r\nConnection: close\r\n\r\n" + body;
                send(c, resp.data(), int(resp.size()), 0);
            }
            close_socket(c);
        }
        close_socket(srv);
    });
    const std::string loc = "http://127.0.0.1:" + std::to_string(port) + "/root.xml";
    std::string ext, loc_ip, err;
    require(upnp::add_mapping(loc, 52815, 3600, ext, loc_ip, err), "UPnP add_mapping succeeds");
    require(ext == "203.0.113.42" && loc_ip == "127.0.0.1" && err.empty(), "UPnP external & local IP returned");
    require(upnp::delete_mapping(loc, 52815), "UPnP delete_mapping succeeds");
    server.join();
    require(seen_add.load() == 1 && seen_del.load() == 1, "mock UPnP saw add and delete");
}

void test_reconnect_under_traffic() {
    require(host_start(0) && host_port() != 0, "host starts for reconnect test");
    std::atomic<bool> stop_feed{false};
    std::thread feeder([&] {
        std::vector<uint8_t> rgba(size_t(kStreamWidth) * kStreamHeight * 4, 0x40);
        float tone[2 * 160];
        for (int i = 0; i < 160; ++i) tone[i * 2] = tone[i * 2 + 1] = 0.25f * float(std::sin(i * 0.2));
        for (int tick = 0; !stop_feed.load(); ++tick) {
            host_submit_audio(tone, 160);
            if (host_wants_video()) {
                for (size_t i = 0; i < rgba.size(); i += 4) rgba[i] = uint8_t((tick * 7) & 0xFF);
                host_submit_frame(rgba.data());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    });

    const auto handshake = [](socket_t s) {
        uint8_t hello[kHelloSize];
        encode_hello(hello);
        send_message(s, Message::Hello, hello, kHelloSize);
        uint8_t header[kHeaderSize], reply[kHelloSize];
        require(recv_exact(s, header, kHeaderSize) && recv_exact(s, reply, kHelloSize), "handshake reply");
        Message t;
        size_t sz = 0;
        require(decode_header(header, t, sz) && t == Message::Hello && sz == kHelloSize && decode_hello(reply, sz),
                "first message after connect is Hello, not stale video/audio");
    };

    const socket_t s1 = connect_local(host_port());
    handshake(s1);
    require(wait_for([] { return host_guest_connected(); }), "guest 1 connected");
    ProPad pad1 = sample_pad(), got{};
    uint8_t payload[kPadSize];
    encode_pad(pad1, payload);
    send_message(s1, Message::Pad, payload, kPadSize);
    require(wait_for([&] { return host_remote_pad(got) && got == pad1; }), "host received guest 1 pad");
    close_socket(s1);
    require(wait_for([] { return !host_guest_connected(); }), "guest 1 disconnect clears connection");
    require(!host_remote_pad(got), "remote pad disabled after disconnect");

    const socket_t s2 = connect_local(host_port());
    handshake(s2);
    require(wait_for([] { return host_guest_connected(); }), "guest 2 connected under active traffic");
    require(host_remote_pad(got) && got == ProPad{}, "guest 2 starts with clean zeroed pad");
    ProPad pad2;
    pad2.hold = 0x1234u;
    pad2.lx = -0.5f;
    encode_pad(pad2, payload);
    send_message(s2, Message::Pad, payload, kPadSize);
    require(wait_for([&] { return host_remote_pad(got) && got == pad2; }), "host receives guest 2 pad");

    bool saw_video = false, saw_audio = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((!saw_video || !saw_audio) && std::chrono::steady_clock::now() < deadline) {
        uint8_t header[kHeaderSize];
        require(recv_exact(s2, header, kHeaderSize), "guest 2 receives packet header");
        Message t;
        size_t sz = 0;
        require(decode_header(header, t, sz), "guest 2 header valid");
        std::vector<uint8_t> pkt(sz);
        require(recv_exact(s2, pkt.data(), sz), "guest 2 receives packet body");
        if (t == Message::Audio) saw_audio = true;
        if (t == Message::Video && sz > 0) saw_video = true;
    }
    require(saw_audio && saw_video, "guest 2 receives fresh audio and video after reconnect");

    stop_feed.store(true);
    feeder.join();
    close_socket(s2);
    host_stop();
}

// RBREW_UPNP_LIVE_TEST=1: the real router. Opens a test port by UPnP (10 min lease, so it
// expires on its own), joins the host through the router's public address (needs NAT
// loopback in the router) and removes the forwarding again.
void test_upnp_live() {
    const char* live = std::getenv("RBREW_UPNP_LIVE_TEST");
    if (!live || live[0] != '1') return;
    constexpr uint16_t kTestPort = 52817;
    const std::string location = upnp::discover(4000);
    require(!location.empty(), "live: a UPnP gateway answers");
    std::printf("live: gateway %s\n", location.c_str());
    require(host_start(kTestPort), "live: host listens on the test port");
    std::string external, local, error;
    require(upnp::add_mapping(location, kTestPort, 600, external, local, error), "live: router accepts the forwarding");
    std::printf("live: router forwards %s:%u -> %s:%u (public: %s)\n", external.c_str(), unsigned(kTestPort),
                local.c_str(), unsigned(kTestPort), is_private_ipv4(external) ? "no, behind another NAT" : "yes");
    socket_t s = kNoSocket;
    if (!external.empty()) {
        addrinfo hints{}, *found = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(external.c_str(), std::to_string(kTestPort).c_str(), &hints, &found) == 0) {
            s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s != kNoSocket && connect(s, found->ai_addr, int(found->ai_addrlen)) != 0) {
                close_socket(s);
                s = kNoSocket;
            }
            freeaddrinfo(found);
        }
    }
    if (s != kNoSocket) {
        uint8_t hello[kHelloSize];
        encode_hello(hello);
        send_message(s, Message::Hello, hello, kHelloSize);
        uint8_t header[kHeaderSize], reply[kHelloSize];
        const bool joined = recv_exact(s, header, kHeaderSize) && recv_exact(s, reply, kHelloSize) &&
                            decode_hello(reply, kHelloSize) && wait_for([] { return host_guest_connected(); });
        std::printf("live: join through the public address: %s\n", joined ? "OK" : "FAILED");
        close_socket(s);
    } else {
        std::printf("live: join through the public address: no connection (router without NAT loopback?)\n");
    }
    require(upnp::delete_mapping(location, kTestPort), "live: forwarding removed again");
    host_stop();
}

template <class F> bool wait_seconds(int seconds, F condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

// A connection that never says hello must not block the only guest slot, and
// a guest whose link goes silent must not keep Player 2's last input pressed.
void test_silent_links() {
    require(host_start(0) && host_port() != 0, "host starts for silent link test");
    const socket_t mute = connect_local(host_port()); // connects, never sends anything

    const socket_t s = connect_local(host_port());
    uint8_t hello[kHelloSize];
    encode_hello(hello);
    send_message(s, Message::Hello, hello, kHelloSize);
    uint8_t header[kHeaderSize], reply[kHelloSize];
    require(recv_exact(s, header, kHeaderSize) && recv_exact(s, reply, kHelloSize) && decode_hello(reply, kHelloSize),
            "a real guest gets in after a silent connection times out");
    require(wait_for([] { return host_guest_connected(); }), "real guest connected");

    ProPad held;
    held.hold = 0x4u; // ZR
    held.ly = 1.0f;
    uint8_t payload[kPadSize];
    encode_pad(held, payload);
    send_message(s, Message::Pad, payload, kPadSize);
    ProPad got;
    require(wait_for([&] { return host_remote_pad(got) && got == held; }), "host sees the held pad");
    require(wait_seconds(2, [&] { return host_remote_pad(got) && got == ProPad{}; }),
            "a stalled link releases Player 2's buttons");
    require(wait_seconds(6, [] { return !host_guest_connected(); }), "a silent guest is dropped");

    close_socket(s);
    close_socket(mute);
    host_stop();
    require(host_start(0), "hosting starts again after stop");
    host_stop();
}

void test_stream_extent() {
    uint32_t w = 0, h = 0;
    stream_extent(1280, 720, w, h);
    require(w == 1280 && h == 720, "720p stream extent stays 1280x720");
    stream_extent(1920, 1080, w, h);
    require(w == 1920 && h == 1080, "1080p stream extent stays 1920x1080");
    stream_extent(2560, 1440, w, h);
    require(w == 1920 && h == 1080, "1440p stream extent clamps to 1920x1080");
    stream_extent(854, 480, w, h);
    require(w == 854 && h == 480, "480p stream extent stays even 854x480");
    stream_extent(0, 0, w, h);
    require(w == 1280 && h == 720, "zero stream extent falls back to 1280x720");
}

} // namespace

int main() {
    test_wire_format();
    test_stream_extent();
    test_keyboard_mapping();
    test_host_receives_pad();
    test_wrong_version_rejected();
    test_video_round_trip(1280, 720, 10000);
    test_video_round_trip(1920, 1080, 16000);
    test_host_streams_video();
    test_audio_wire_format();
    test_host_streams_audio();
    test_connect_helpers();
    test_upnp_mock_round_trip();
    test_reconnect_under_traffic();
    test_silent_links();
    test_upnp_live();
    RBREW_LOG_INFO("netplay", "all netplay tests passed\n");
    return 0;
}

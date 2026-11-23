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
#include <netinet/tcp.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kNoSocket = -1;
static void close_socket(socket_t s) {
    close(s);
}
#endif

#include "netplay.h"
#include "netplay_video.h"
#include "port_settings.h"
#include "rebrewu/logger.hpp"

#if defined(HAVE_SDL2)
#include "../os/padscore/pro_pad.h"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#endif
#if defined(HAVE_MINIAUDIO)
#include "miniaudio.h"
#endif
#include <deque>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace rbrew_net {

// --- Wire format: little-endian, one header per message ---

static void put32(uint8_t* out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out[i] = uint8_t(v >> (8 * i));
}
static uint32_t get32(const uint8_t* in) {
    return uint32_t(in[0]) | uint32_t(in[1]) << 8 | uint32_t(in[2]) << 16 | uint32_t(in[3]) << 24;
}
static void put_float(uint8_t* out, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    put32(out, bits);
}
static float get_float(const uint8_t* in) {
    const uint32_t bits = get32(in);
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

size_t encode_header(Message type, size_t payload, uint8_t* out) {
    out[0] = uint8_t(type);
    out[1] = uint8_t(payload);
    out[2] = uint8_t(payload >> 8);
    out[3] = uint8_t(payload >> 16);
    return kHeaderSize;
}
bool decode_header(const uint8_t* in, Message& type, size_t& payload) {
    type = Message(in[0]);
    payload = size_t(in[1]) | size_t(in[2]) << 8 | size_t(in[3]) << 16;
    return payload <= kMaxPayload;
}
void encode_hello(uint8_t* out) {
    std::memcpy(out, "SPNT", 4);
    out[4] = uint8_t(kProtocolVersion);
    out[5] = uint8_t(kProtocolVersion >> 8);
}
bool decode_hello(const uint8_t* in, size_t size) {
    return size == kHelloSize && std::memcmp(in, "SPNT", 4) == 0 &&
           (uint16_t(in[4]) | uint16_t(in[5]) << 8) == kProtocolVersion;
}
void encode_pad(const ProPad& pad, uint8_t* out) {
    put32(out, pad.hold);
    put_float(out + 4, pad.lx);
    put_float(out + 8, pad.ly);
    put_float(out + 12, pad.rx);
    put_float(out + 16, pad.ry);
}
bool decode_pad(const uint8_t* in, size_t size, ProPad& pad) {
    if (size != kPadSize) return false;
    pad.hold = get32(in);
    pad.lx = get_float(in + 4);
    pad.ly = get_float(in + 8);
    pad.rx = get_float(in + 12);
    pad.ry = get_float(in + 16);
    for (float v : {pad.lx, pad.ly, pad.rx, pad.ry})
        if (!(v >= -1.0f && v <= 1.0f)) return false;
    return true;
}

// --- Sockets ---

static bool sockets_ready() {
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

static void set_nodelay(socket_t s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}
// Receive/send timeouts in ms (0 = wait forever); a timed-out call fails like a closed connection.
static void set_timeouts(socket_t s, uint32_t recv_ms, uint32_t send_ms) {
#if defined(_WIN32)
    const DWORD r = recv_ms, w = send_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&r), sizeof(r));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&w), sizeof(w));
#else
    const timeval r{time_t(recv_ms / 1000), suseconds_t(recv_ms % 1000 * 1000)};
    const timeval w{time_t(send_ms / 1000), suseconds_t(send_ms % 1000 * 1000)};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &r, sizeof(r));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &w, sizeof(w));
#endif
}
static void set_blocking(socket_t s, bool blocking) {
#if defined(_WIN32)
    u_long mode = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK);
#endif
}
// Connection timeouts: the guest sends its pad at least every 100 ms, so a
// silent link is dead; the handshake and every send are bounded too.
constexpr uint32_t kHandshakeMs = 3000, kSilentLinkMs = 3000, kSendMs = 2000, kConnectMs = 1500, kHostSilentMs = 15000;

static bool send_all(socket_t s, const uint8_t* data, size_t size) {
#ifdef MSG_NOSIGNAL
    constexpr int kSendFlags = MSG_NOSIGNAL;
#else
    constexpr int kSendFlags = 0;
#endif
    while (size) {
        const int sent = send(s, reinterpret_cast<const char*>(data), int(size), kSendFlags);
        if (sent <= 0) return false;
        data += sent;
        size -= size_t(sent);
    }
    return true;
}
static bool recv_all(socket_t s, uint8_t* data, size_t size) {
    while (size) {
        const int got = recv(s, reinterpret_cast<char*>(data), int(size), 0);
        if (got <= 0) return false;
        data += got;
        size -= size_t(got);
    }
    return true;
}
static bool send_message(socket_t s, Message type, const uint8_t* payload, size_t size) {
    uint8_t header[kHeaderSize];
    encode_header(type, size, header);
    return send_all(s, header, kHeaderSize) && send_all(s, payload, size);
}
static bool recv_message(socket_t s, Message& type, std::vector<uint8_t>& payload) {
    uint8_t header[kHeaderSize];
    size_t size = 0;
    if (!recv_all(s, header, kHeaderSize) || !decode_header(header, type, size)) return false;
    payload.resize(size);
    return recv_all(s, payload.data(), size);
}
static bool exchange_hello(socket_t s) {
    uint8_t hello[kHelloSize];
    encode_hello(hello);
    Message type;
    std::vector<uint8_t> reply;
    return send_message(s, Message::Hello, hello, kHelloSize) && recv_message(s, type, reply) &&
           type == Message::Hello && decode_hello(reply.data(), reply.size());
}

// --- Host ---

namespace {
struct Host {
    std::mutex mutex; // pad, pad_time
    ProPad pad;
    std::chrono::steady_clock::time_point pad_time;
    std::atomic<socket_t> pending{kNoSocket}; // accepted, handshake running (host_stop can shut it down)
    std::mutex send_mutex;                    // guest socket while sending or closing
    std::mutex frame_mutex;
    std::condition_variable frame_cv;
    std::vector<uint8_t> frame;
    uint32_t frame_w = kStreamWidth, frame_h = kStreamHeight;
    bool frame_ready = false;
    std::vector<uint8_t> audio; // encoded PCM waiting to be sent (frame_mutex)
    std::atomic<bool> connected{false}, stopping{false}, video_failed{false};
    std::atomic<int> threads{0};
    std::atomic<uint64_t> session{0}; // counts guest connections
    std::atomic<socket_t> listener{kNoSocket};
    socket_t guest = kNoSocket; // send_mutex
    std::atomic<uint16_t> port{0};
};
Host& host() {
    static Host h;
    return h;
}

bool send_to_guest(Message type, uint64_t expected_session, const uint8_t* payload, size_t size) {
    Host& h = host();
    std::lock_guard lock(h.send_mutex);
    if (h.guest == kNoSocket || h.session.load() != expected_session) return false;
    if (send_message(h.guest, type, payload, size)) return true;
    shutdown(h.guest, 2);
    return false;
}

void serve_guest(socket_t s) {
    Host& h = host();
    set_nodelay(s);
    set_timeouts(s, kHandshakeMs, kSendMs);
    h.pending.store(s);
    const bool hello = !h.stopping.load() && exchange_hello(s);
    h.pending.store(kNoSocket);
    if (!hello) {
        RBREW_LOG_INFO("net", "guest rejected: wrong handshake, version or timeout\n");
        return;
    }
    set_timeouts(s, kSilentLinkMs, kSendMs);
    {
        std::lock_guard lock(h.frame_mutex);
        h.audio.clear();
        h.frame_ready = false;
    }
    {
        std::lock_guard lock(h.mutex);
        h.pad = ProPad{};
    }
    h.session.fetch_add(1);
    {
        std::lock_guard lock(h.send_mutex);
        h.guest = s;
    }
    h.connected.store(true);
    RBREW_LOG_INFO("net", "guest connected as Player 2\n");
    Message type;
    std::vector<uint8_t> payload;
    while (!h.stopping.load() && recv_message(s, type, payload)) {
        ProPad pad;
        if (type == Message::Pad && decode_pad(payload.data(), payload.size(), pad)) {
            std::lock_guard lock(h.mutex);
            h.pad = pad;
            h.pad_time = std::chrono::steady_clock::now();
        }
    }
    h.connected.store(false);
    {
        std::lock_guard lock(h.mutex);
        h.pad = ProPad{};
    }
    {
        std::lock_guard lock(h.send_mutex);
        h.guest = kNoSocket;
    }
    RBREW_LOG_INFO("net", "guest disconnected\n");
}

void host_loop() {
    Host& h = host();
    while (!h.stopping.load()) {
        const socket_t s = accept(h.listener.load(), nullptr, nullptr);
        if (s == kNoSocket) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        serve_guest(s);
        close_socket(s);
    }
    h.threads.fetch_sub(1);
}

uint32_t stream_kbps(uint32_t width) {
    const char* value = std::getenv("RBREW_NET_KBPS");
    if (value && *value) {
        const unsigned long kbps = std::strtoul(value, nullptr, 10);
        if (kbps >= 500 && kbps <= 100000) return uint32_t(kbps);
    }
    const uint32_t configured = uint32_t(rbrew_settings::get().net_kbps);
    return (width >= 1920u && configured <= 10000u) ? 16000u : configured;
}

// Sends waiting sound first, then encodes the newest captured frame; a new
// encoder per guest or resolution change starts on a keyframe.
void sender_loop() {
    Host& h = host();
    std::unique_ptr<H264Encoder> encoder;
    uint64_t encoder_session = 0;
    uint32_t enc_w = 0, enc_h = 0, enc_kbps = 0;
    std::vector<uint8_t> rgba, nv12(size_t(kStreamWidth) * kStreamHeight * 3 / 2), bytes, sound;
    while (!h.stopping.load()) {
        bool have_frame = false;
        uint32_t frame_w = kStreamWidth, frame_h = kStreamHeight;
        {
            std::unique_lock lock(h.frame_mutex);
            h.frame_cv.wait_for(lock, std::chrono::milliseconds(100),
                                [&] { return h.frame_ready || !h.audio.empty() || h.stopping.load(); });
            sound.swap(h.audio);
            h.audio.clear();
            if (h.frame_ready) {
                rgba.swap(h.frame);
                frame_w = h.frame_w;
                frame_h = h.frame_h;
                h.frame_ready = false;
                have_frame = true;
            }
        }
        if (!h.connected.load()) continue;
        const uint64_t session = h.session.load();
        if (!sound.empty()) send_to_guest(Message::Audio, session, sound.data(), sound.size());
        if (!have_frame || h.video_failed.load() || !h.connected.load()) continue;
        if (rgba.size() < size_t(frame_w) * frame_h * 4) continue;
        const uint32_t target_kbps = stream_kbps(frame_w);
        const bool fresh =
            !encoder || session != encoder_session || frame_w != enc_w || frame_h != enc_h || target_kbps != enc_kbps;
        if (fresh) {
            encoder = std::make_unique<H264Encoder>();
            if (!encoder->open(frame_w, frame_h, 60, target_kbps)) {
                RBREW_LOG_ERROR("net", "H.264 encoder unavailable; the guest gets no picture\n");
                h.video_failed.store(true);
                continue;
            }
            encoder_session = session;
            enc_w = frame_w;
            enc_h = frame_h;
            enc_kbps = target_kbps;
        }
        const size_t nv12_size = size_t(frame_w) * frame_h * 3 / 2;
        if (nv12.size() != nv12_size) nv12.resize(nv12_size);
        rgba_to_nv12(rgba.data(), frame_w, frame_h, frame_w * 4, nv12.data());
        bytes.clear();
        if (encoder->encode(nv12.data(), fresh, bytes) && !bytes.empty())
            send_to_guest(Message::Video, session, bytes.data(), bytes.size());
    }
    h.threads.fetch_sub(1);
}
} // namespace

void stream_extent(uint32_t src_w, uint32_t src_h, uint32_t& out_w, uint32_t& out_h) {
    if (src_w < 2 || src_h < 2) {
        out_w = kStreamWidth;
        out_h = kStreamHeight;
        return;
    }
    uint64_t w = src_w, h = src_h;
    if (w > kMaxStreamWidth) {
        h = (h * kMaxStreamWidth) / w;
        w = kMaxStreamWidth;
    }
    if (h > kMaxStreamHeight) {
        w = (w * kMaxStreamHeight) / h;
        h = kMaxStreamHeight;
    }
    w &= ~uint64_t(1);
    h &= ~uint64_t(1);
    out_w = std::max(2u, uint32_t(w));
    out_h = std::max(2u, uint32_t(h));
}

bool host_start(uint16_t port) {
    Host& h = host();
    if (h.listener.load() != kNoSocket || h.threads.load() > 0 || !sockets_ready()) return false;
    const socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket) return false;
    int one = 1;
#if defined(_WIN32)
    // SO_REUSEADDR would let a second instance take over a port already in use.
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof(one));
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    socklen_t length = sizeof(addr);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(s, 4) != 0 ||
        getsockname(s, reinterpret_cast<sockaddr*>(&addr), &length) != 0) {
        RBREW_LOG_ERROR("net", "cannot listen on port %u\n", unsigned(port));
        close_socket(s);
        return false;
    }
    h.port.store(ntohs(addr.sin_port));
    h.listener.store(s);
    h.stopping.store(false);
    h.video_failed.store(false);
    h.threads.store(2);
    // Never joined at exit; host_stop waits for both.
    std::thread(host_loop).detach();
    std::thread(sender_loop).detach();
    RBREW_LOG_INFO("net", "hosting on port %u\n", unsigned(h.port.load()));
    return true;
}

void host_start_from_env() {
    const char* value = std::getenv("RBREW_NET_HOST");
    if (!value || !*value || std::strcmp(value, "0") == 0) return;
    const unsigned long port = std::strcmp(value, "1") == 0 ? kDefaultPort : std::strtoul(value, nullptr, 10);
    if (port > 0 && port <= 65535) host_start(uint16_t(port));
}

uint16_t host_port() {
    return host().port.load();
}

void host_stop() {
    Host& h = host();
    h.stopping.store(true);
    h.frame_cv.notify_all();
    if (const socket_t s = h.listener.exchange(kNoSocket); s != kNoSocket) {
        shutdown(s, 2); // wakes a blocked accept() on Linux
        close_socket(s);
    }
    if (const socket_t s = h.pending.load(); s != kNoSocket) shutdown(s, 2);
    {
        std::lock_guard lock(h.send_mutex);
        if (h.guest != kNoSocket) shutdown(h.guest, 2);
    }
    for (int i = 0; i < 300 && h.threads.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    h.port.store(0);
    h.connected.store(false);
}

bool host_guest_connected() {
    return host().connected.load();
}

bool host_wants_video() {
    static const bool disabled = [] { // RBREW_NET_VIDEO=0: controller and sound only
        const char* v = std::getenv("RBREW_NET_VIDEO");
        return v && v[0] == '0';
    }();
    Host& h = host();
    if (disabled || !h.connected.load(std::memory_order_relaxed) || h.video_failed.load(std::memory_order_relaxed))
        return false;
    std::lock_guard lock(h.frame_mutex);
    return !h.frame_ready;
}

void encode_audio(const float* stereo, uint32_t frames, std::vector<uint8_t>& out) {
    const size_t start = out.size();
    out.resize(start + size_t(frames) * 4);
    for (uint32_t i = 0; i < frames * 2; ++i) {
        const float clamped = std::clamp(stereo[i], -1.0f, 1.0f);
        const int16_t sample = int16_t(std::lround(clamped * 32767.0f));
        out[start + i * 2] = uint8_t(sample);
        out[start + i * 2 + 1] = uint8_t(uint16_t(sample) >> 8);
    }
}

bool decode_audio(const uint8_t* in, size_t size, std::vector<int16_t>& stereo) {
    if (size % 4) return false;
    const size_t start = stereo.size();
    stereo.resize(start + size / 2);
    for (size_t i = 0; i < size / 2; ++i)
        stereo[start + i] = int16_t(uint16_t(in[i * 2]) | uint16_t(in[i * 2 + 1]) << 8);
    return true;
}

void host_submit_audio(const float* stereo, uint32_t frames) {
    Host& h = host();
    if (!h.connected.load(std::memory_order_relaxed)) return;
    {
        std::lock_guard lock(h.frame_mutex);
        if (h.audio.size() > kAudioRate * 4) h.audio.clear(); // over a second behind: drop it
        encode_audio(stereo, frames, h.audio);
    }
    h.frame_cv.notify_one();
}

void host_submit_frame(const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (!rgba || width < 2 || height < 2 || (width & 1u) != 0 || (height & 1u) != 0 || width > kMaxStreamWidth ||
        height > kMaxStreamHeight)
        return;
    Host& h = host();
    // Copied outside the lock so the audio thread never waits for it.
    thread_local std::vector<uint8_t> copy;
    copy.assign(rgba, rgba + size_t(width) * height * 4);
    {
        std::lock_guard lock(h.frame_mutex);
        if (h.frame_ready) return;
        h.frame.swap(copy);
        h.frame_w = width;
        h.frame_h = height;
        h.frame_ready = true;
    }
    h.frame_cv.notify_one();
}

bool host_remote_pad(ProPad& pad) {
    Host& h = host();
    if (!h.connected.load(std::memory_order_relaxed)) return false;
    std::lock_guard lock(h.mutex);
    // A stalled link must not leave Player 2 holding its last input.
    const bool fresh = std::chrono::steady_clock::now() - h.pad_time < std::chrono::milliseconds(500);
    pad = fresh ? h.pad : ProPad{};
    return true;
}

// --- Guest ---

std::string join_address(int argc, char** argv) {
#if defined(_WIN32)
    if (!argv && __argv) { // the distributable runtime starts the game without argv
        argc = __argc;
        argv = __argv;
    }
#endif
    for (int i = 1; argv && i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--join") == 0) return argv[i + 1];
    const char* value = std::getenv("RBREW_NET_JOIN");
    return value ? value : "";
}

static socket_t connect_to(const std::string& address) {
    std::string name = address;
    std::string port = std::to_string(kDefaultPort);
    if (const size_t colon = address.rfind(':'); colon != std::string::npos && address.find(':') == colon) {
        name = address.substr(0, colon);
        port = address.substr(colon + 1);
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(name.c_str(), port.c_str(), &hints, &found) != 0) return kNoSocket;
    socket_t s = kNoSocket;
    // Non-blocking connect with a time limit, so the guest window keeps responding.
    for (addrinfo* a = found; a && s == kNoSocket; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == kNoSocket) continue;
        set_blocking(s, false);
        connect(s, a->ai_addr, int(a->ai_addrlen));
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(s, &writable);
        FD_SET(s, &failed);
        timeval wait{long(kConnectMs / 1000), long(kConnectMs % 1000 * 1000)};
        int error = 1;
        socklen_t length = sizeof(error);
        if (select(int(s + 1), nullptr, &writable, &failed, &wait) > 0 && FD_ISSET(s, &writable) &&
            getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) == 0 && error == 0) {
            set_blocking(s, true);
        } else {
            close_socket(s);
            s = kNoSocket;
        }
    }
    freeaddrinfo(found);
    if (s != kNoSocket) {
        set_nodelay(s);
        set_timeouts(s, kHandshakeMs, kSendMs);
    }
    return s;
}

#if defined(HAVE_SDL2)
namespace {
// Plays the host's sound. Starts once ~60 ms are buffered and drops the oldest
// samples when more than ~200 ms pile up, so the delay stays short.
class GuestSound {
public:
    ~GuestSound() {
#if defined(HAVE_MINIAUDIO)
        if (open_) ma_device_uninit(&device_);
#endif
    }
    bool open() {
#if defined(HAVE_MINIAUDIO)
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_s16;
        config.playback.channels = 2;
        config.sampleRate = kAudioRate;
        config.dataCallback = callback;
        config.pUserData = this;
        if (ma_device_init(nullptr, &config, &device_) != MA_SUCCESS) return false;
        open_ = true;
        return ma_device_start(&device_) == MA_SUCCESS;
#else
        return false;
#endif
    }
    void push(const std::vector<int16_t>& stereo) {
        std::lock_guard lock(mutex_);
        queue_.insert(queue_.end(), stereo.begin(), stereo.end());
        if (queue_.size() > kMaxSamples) queue_.erase(queue_.begin(), queue_.end() - kStartSamples);
    }
    void clear() {
        std::lock_guard lock(mutex_);
        queue_.clear();
        playing_ = false;
    }

private:
    static constexpr size_t kStartSamples = kAudioRate * 2 * 60 / 1000, kMaxSamples = kAudioRate * 2 * 200 / 1000;
#if defined(HAVE_MINIAUDIO)
    static void callback(ma_device* device, void* output, const void*, ma_uint32 frames) {
        static_cast<GuestSound*>(device->pUserData)->fill(static_cast<int16_t*>(output), frames);
    }
    ma_device device_{};
#endif
    void fill(int16_t* out, uint32_t frames) {
        const float volume = rbrew_settings::g_volume.load(std::memory_order_relaxed);
        const size_t wanted = size_t(frames) * 2;
        std::lock_guard lock(mutex_);
        if (!playing_ && queue_.size() >= kStartSamples) playing_ = true;
        const size_t n = playing_ ? std::min(wanted, queue_.size()) : 0;
        for (size_t i = 0; i < n; ++i) out[i] = int16_t(float(queue_[i]) * volume);
        queue_.erase(queue_.begin(), queue_.begin() + n);
        std::fill(out + n, out + wanted, int16_t(0));
        if (n < wanted && playing_) {
            playing_ = false; // ran dry: buffer again before playing on
            gaps_.fetch_add(1, std::memory_order_relaxed);
        }
    }

public:
    std::atomic<uint32_t> gaps_{0};

private:
    std::mutex mutex_;
    std::deque<int16_t> queue_;
    bool playing_ = false, open_ = false;
};

// Decoded pictures from the receive thread, newest only.
struct GuestVideo {
    std::mutex mutex;
    std::vector<uint8_t> nv12;
    uint32_t width = 0, height = 0;
    bool fresh = false;
    std::atomic<bool> alive{true};
    std::atomic<uint32_t> pictures{0};
    std::atomic<uint32_t> sound_packets{0}; // with something audible
};

void receive_loop(socket_t s, GuestVideo* video, GuestSound* sound) {
    H264Decoder decoder;
    const bool can_decode = decoder.open();
    Message type;
    std::vector<uint8_t> payload, picture;
    std::vector<int16_t> samples;
    uint32_t width = 0, height = 0;
    while (recv_message(s, type, payload)) {
        if (type == Message::Audio) {
            samples.clear();
            if (!decode_audio(payload.data(), payload.size(), samples)) continue;
            sound->push(samples);
            if (std::any_of(samples.begin(), samples.end(), [](int16_t v) { return v != 0; }))
                video->sound_packets.fetch_add(1);
            continue;
        }
        if (type != Message::Video || !can_decode ||
            !decoder.decode(payload.data(), payload.size(), picture, width, height))
            continue;
        std::lock_guard lock(video->mutex);
        video->nv12.swap(picture);
        video->width = width;
        video->height = height;
        video->fresh = true;
        video->pictures.fetch_add(1);
    }
    video->alive.store(false);
}

// RBREW_NET_SNAPSHOT=<file.bmp>: the last picture is saved when the guest closes (tests).
void write_nv12_bmp(const char* path, uint32_t w, uint32_t h, const std::vector<uint8_t>& nv12) {
    if (!path || !*path || nv12.empty() || !w || !h) return;
    const uint32_t stride = (w * 3 + 3) & ~3u;
    std::vector<uint8_t> pixels(size_t(stride) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const int luma = nv12[size_t(y) * w + x] - 16;
            const uint8_t* uv = &nv12[size_t(w) * h + size_t(y / 2) * w + (x & ~1u)];
            const int u = uv[0] - 128, vv = uv[1] - 128;
            const auto clamp = [](int c) { return uint8_t(c < 0 ? 0 : c > 255 ? 255 : c); };
            uint8_t* out = &pixels[size_t(y) * stride + x * 3];
            out[0] = clamp((298 * luma + 516 * u + 128) >> 8);
            out[1] = clamp((298 * luma - 100 * u - 208 * vv + 128) >> 8);
            out[2] = clamp((298 * luma + 409 * vv + 128) >> 8);
        }
    uint8_t header[54] = {'B', 'M'};
    const auto put = [&](int at, uint32_t value) { put32(header + at, value); };
    put(2, uint32_t(54 + pixels.size()));
    put(10, 54);
    put(14, 40);
    put(18, w);
    put(22, uint32_t(-int32_t(h)));
    header[26] = 1;
    header[28] = 24;
    if (FILE* f = std::fopen(path, "wb")) {
        std::fwrite(header, 1, sizeof(header), f);
        std::fwrite(pixels.data(), 1, pixels.size(), f);
        std::fclose(f);
    }
}

void save_snapshot(const GuestVideo& v) {
    write_nv12_bmp(std::getenv("RBREW_NET_SNAPSHOT"), v.width, v.height, v.nv12);
}

std::string numbered_snapshot_path(const std::string& base, uint32_t seq) {
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "_%04u.bmp", seq);
    if (base.size() >= 4 && base.compare(base.size() - 4, 4, ".bmp") == 0)
        return base.substr(0, base.size() - 4) + suffix;
    return base + suffix;
}

struct PadCue {
    uint32_t start_ms = 0, end_ms = 0, hold = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;
};

std::vector<PadCue> load_pad_script() {
    std::vector<PadCue> cues;
    const char* path = std::getenv("RBREW_GUEST_PAD_SCRIPT");
    if (!path || !*path) return cues;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        PadCue cue;
        std::string hex;
        if (ss >> cue.start_ms >> cue.end_ms >> hex >> cue.lx >> cue.ly >> cue.rx >> cue.ry) {
            cue.hold = uint32_t(std::strtoul(hex.c_str(), nullptr, 16));
            cues.push_back(cue);
        }
    }
    return cues;
}

uint32_t read_env_ms(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) return 0;
    const unsigned long ms = std::strtoul(v, nullptr, 10);
    return ms > 0 ? uint32_t(ms) : 0u;
}

struct Connection {
    socket_t socket = kNoSocket;
    std::unique_ptr<GuestVideo> video;
    std::thread receiver;
    void close() {
        if (socket == kNoSocket) return;
        shutdown(socket, 2);
        if (receiver.joinable()) receiver.join();
        close_socket(socket);
        socket = kNoSocket;
    }
};
} // namespace

int run_guest(const std::string& address) {
#if defined(_WIN32)
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS); // only the game needs high priority
#endif
    SDL_SetMainReady();
    if (!sockets_ready() || SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) return 1;
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_SetYUVConversionMode(SDL_YUV_CONVERSION_BT601);
    SDL_Window* window = SDL_CreateWindow("Splatoon (P2) - joining", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          1280, 720, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (!renderer) return 1;
    SDL_RenderSetLogicalSize(renderer, kStreamWidth, kStreamHeight);
    SDL_Texture* texture = nullptr;
    uint32_t texture_w = 0, texture_h = 0, last_w = 0, last_h = 0;
    std::vector<uint8_t> last_picture;

    SDL_GameController* controller = nullptr;
    Connection link;
    const auto started_at = std::chrono::steady_clock::now();
    auto last_attempt = started_at - std::chrono::seconds(10);
    auto last_send = last_attempt, fps_since = last_attempt, connected_at = started_at, last_snap = started_at;
    uint32_t fps_pictures = 0, fps = 0, sound_seen = 0, total_pictures = 0, total_sound = 0, snap_seq = 0;
    bool hearing = false;
    const std::vector<PadCue> script = load_pad_script();
    const uint32_t snap_every_ms = read_env_ms("RBREW_NET_SNAPSHOT_EVERY");
    const uint32_t exit_ms = read_env_ms("RBREW_GUEST_EXIT_MS");
    const char* snap_base = std::getenv("RBREW_NET_SNAPSHOT");
    GuestSound sound;
    sound.open(); // no sound device: the picture still works
    ProPad sent;
    bool running = true, mouse_captured = false;
    std::string title;
    int bindings[rbrew_settings::kActionCount];
    for (int i = 0; i < rbrew_settings::kActionCount; ++i) bindings[i] = rbrew_settings::g_keys[i].load();
    auto last_frame = started_at;
    const auto capture_mouse = [&](bool on) {
        mouse_captured = on;
        SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
    };
    while (running) {
        SDL_Event event;
        int mouse_dx = 0, mouse_dy = 0;
        const bool aiming = mouse_captured; // the click that captures the mouse does not shoot
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            // Click into the window to aim with the mouse; Esc gives it back.
            if (event.type == SDL_MOUSEBUTTONDOWN && !mouse_captured) capture_mouse(true);
            if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) capture_mouse(false);
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) capture_mouse(false);
            if (event.type == SDL_MOUSEMOTION && mouse_captured) {
                mouse_dx += event.motion.xrel;
                mouse_dy += event.motion.yrel;
            }
            if (event.type == SDL_CONTROLLERDEVICEADDED && !controller)
                controller = SDL_GameControllerOpen(event.cdevice.which);
            if (event.type == SDL_CONTROLLERDEVICEREMOVED && controller &&
                event.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller))) {
                SDL_GameControllerClose(controller);
                controller = nullptr;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (exit_ms > 0 && now - started_at >= std::chrono::milliseconds(exit_ms)) {
            running = false;
            break;
        }
        const float frame_seconds = std::max(std::chrono::duration<float>(now - last_frame).count(), 0.001f);
        last_frame = now;
        if (link.socket != kNoSocket && !link.video->alive.load()) {
            total_pictures = link.video->pictures.load();
            total_sound = link.video->sound_packets.load();
            RBREW_LOG_INFO("net", "guest disconnected from %s (pictures=%u sound=%u gaps=%u)\n", address.c_str(),
                           total_pictures, total_sound, sound.gaps_.load());
            link.close();
        }
        if (link.socket == kNoSocket && now - last_attempt > std::chrono::seconds(2)) {
            last_attempt = now;
            const socket_t s = connect_to(address);
            if (s != kNoSocket && exchange_hello(s)) {
                set_timeouts(s, kHostSilentMs, kSendMs); // the host streams every frame; silence means it is gone
                link.socket = s;
                link.video = std::make_unique<GuestVideo>();
                sound.clear();
                link.receiver = std::thread(receive_loop, s, link.video.get(), &sound);
                connected_at = now;
                sent = ProPad{};
                fps_since = now;
                fps_pictures = sound_seen = 0;
                RBREW_LOG_INFO("net", "guest connected to %s\n", address.c_str());
            } else if (s != kNoSocket) {
                close_socket(s);
            }
        }
        if (link.socket != kNoSocket) {
            ProPad pad = controller ? pro_pad_from_controller(controller) : ProPad{};
            if (SDL_GetKeyboardFocus() == window) {
                int key_count = 0;
                const uint8_t* keys = SDL_GetKeyboardState(&key_count);
                const uint32_t buttons = aiming && mouse_captured ? SDL_GetMouseState(nullptr, nullptr) : 0;
                const float scale = rbrew_settings::g_mouse_scale.load();
                pad = merge_pads(pad, pro_pad_from_keyboard(keys, key_count, buttons, mouse_dx / frame_seconds,
                                                            mouse_dy / frame_seconds, bindings, scale));
            }
            if (!script.empty()) {
                const uint32_t elapsed_ms =
                    uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(now - connected_at).count());
                for (const PadCue& cue : script) {
                    if (elapsed_ms < cue.start_ms || elapsed_ms >= cue.end_ms) continue;
                    pad.hold |= cue.hold;
                    if (cue.lx != 0.0f || cue.ly != 0.0f) {
                        pad.lx = std::clamp(cue.lx, -1.0f, 1.0f);
                        pad.ly = std::clamp(cue.ly, -1.0f, 1.0f);
                    }
                    if (cue.rx != 0.0f || cue.ry != 0.0f) {
                        pad.rx = std::clamp(cue.rx, -1.0f, 1.0f);
                        pad.ry = std::clamp(cue.ry, -1.0f, 1.0f);
                    }
                }
            }
            if (!(pad == sent) || now - last_send > std::chrono::milliseconds(100)) {
                uint8_t payload[kPadSize];
                encode_pad(pad, payload);
                if (send_message(link.socket, Message::Pad, payload, kPadSize)) {
                    sent = pad;
                    last_send = now;
                } else {
                    total_pictures = link.video->pictures.load();
                    total_sound = link.video->sound_packets.load();
                    RBREW_LOG_INFO("net", "guest send failed to %s (pictures=%u sound=%u gaps=%u)\n", address.c_str(),
                                   total_pictures, total_sound, sound.gaps_.load());
                    link.close();
                }
            }
        }
        if (link.socket != kNoSocket) {
            {
                std::lock_guard lock(link.video->mutex);
                GuestVideo& v = *link.video;
                if (v.fresh) {
                    v.fresh = false;
                    last_picture = v.nv12;
                    last_w = v.width;
                    last_h = v.height;
                    if (!texture || texture_w != v.width || texture_h != v.height) {
                        if (texture) SDL_DestroyTexture(texture);
                        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_NV12, SDL_TEXTUREACCESS_STREAMING,
                                                    int(v.width), int(v.height));
                        SDL_RenderSetLogicalSize(renderer, int(v.width), int(v.height));
                        texture_w = v.width;
                        texture_h = v.height;
                    }
                    if (texture)
                        SDL_UpdateNVTexture(texture, nullptr, v.nv12.data(), int(v.width),
                                            v.nv12.data() + size_t(v.width) * v.height, int(v.width));
                }
            }
            total_pictures = link.video->pictures.load();
            total_sound = link.video->sound_packets.load();
            if (snap_every_ms > 0 && snap_base && *snap_base && !last_picture.empty() &&
                now - last_snap >= std::chrono::milliseconds(snap_every_ms)) {
                last_snap = now;
                write_nv12_bmp(numbered_snapshot_path(snap_base, ++snap_seq).c_str(), last_w, last_h, last_picture);
            }
            if (now - fps_since >= std::chrono::seconds(1)) {
                fps = total_pictures - fps_pictures;
                fps_pictures = total_pictures;
                hearing = total_sound != sound_seen;
                sound_seen = total_sound;
                fps_since = now;
            }
        }
        std::string state = link.socket == kNoSocket ? "connecting to " + address : "connected to " + address;
        if (link.socket != kNoSocket) {
            state += ", " + std::to_string(fps) + " fps";
            if (hearing) state += ", sound";
            if (const uint32_t gaps = sound.gaps_.load()) state += " (" + std::to_string(gaps) + " sound gaps)";
        }
        const char* input = controller       ? ""
                            : mouse_captured ? " - keyboard and mouse (Esc frees the mouse)"
                                             : " - keyboard (click to aim with the mouse)";
        const std::string next = "Splatoon (P2) - " + state + input;
        if (next != title) {
            title = next;
            SDL_SetWindowTitle(window, title.c_str());
        }
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (texture && link.socket != kNoSocket) SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
    }
    if (link.socket != kNoSocket && link.video) {
        total_pictures = link.video->pictures.load();
        total_sound = link.video->sound_packets.load();
    }
    RBREW_LOG_INFO("net", "guest exiting: pictures=%u sound=%u gaps=%u\n", total_pictures, total_sound,
                   sound.gaps_.load());
    link.close();
    GuestVideo snapshot;
    snapshot.nv12 = std::move(last_picture);
    snapshot.width = last_w;
    snapshot.height = last_h;
    save_snapshot(snapshot);
    if (texture) SDL_DestroyTexture(texture);
    if (controller) SDL_GameControllerClose(controller);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
#else
int run_guest(const std::string&) {
    return 1;
}
#endif

} // namespace rbrew_net

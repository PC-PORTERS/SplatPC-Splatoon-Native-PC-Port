#pragma once
// Online play by streaming: the host runs the game, the guest sends its
// controller as Player 2 (Battle Dojo Pro Controller) and will receive video.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rbrew_net {

// Pro Controller state in the KPAD button layout.
struct ProPad {
    uint32_t hold = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;
};
inline bool operator==(const ProPad& a, const ProPad& b) {
    return a.hold == b.hold && a.lx == b.lx && a.ly == b.ly && a.rx == b.rx && a.ry == b.ry;
}

constexpr uint16_t kDefaultPort = 52815;
constexpr uint16_t kProtocolVersion = 1;

enum class Message : uint8_t { Hello = 1, Pad = 2, Video = 3, Audio = 4 };
constexpr size_t kHeaderSize = 4; // type, 24-bit little-endian payload size
constexpr size_t kHelloSize = 6;  // "SPNT" + version
constexpr size_t kPadSize = 20;   // hold + four floats
constexpr size_t kMaxPayload = 1u << 23;

size_t encode_header(Message type, size_t payload, uint8_t* out);
bool decode_header(const uint8_t* in, Message& type, size_t& payload);
void encode_hello(uint8_t* out);
bool decode_hello(const uint8_t* in, size_t size);
void encode_pad(const ProPad& pad, uint8_t* out);
bool decode_pad(const uint8_t* in, size_t size, ProPad& pad);

// Host. RBREW_NET_HOST=1 listens on the default port, RBREW_NET_HOST=<port> on that port.
void host_start_from_env();
bool host_start(uint16_t port);
uint16_t host_port(); // the bound port (useful with port 0), 0 when not listening
void host_stop();
bool host_guest_connected();
bool host_remote_pad(ProPad& pad); // true while a guest is connected

// Stream picture size; the renderer scales Player 2's screen to it.
constexpr uint32_t kStreamWidth = 1280, kStreamHeight = 720;
constexpr uint32_t kMaxStreamWidth = 1920, kMaxStreamHeight = 1080;
void stream_extent(uint32_t src_w, uint32_t src_h, uint32_t& out_w, uint32_t& out_h);
bool host_wants_video(); // a guest is connected and the encoder is ready for a frame
// RGBA, width x height (even dimensions up to kMaxStreamWidth x kMaxStreamHeight), tightly packed.
// Frames arriving while the previous one is still encoding are dropped.
void host_submit_frame(const uint8_t* rgba, uint32_t width, uint32_t height);
inline void host_submit_frame(const uint8_t* rgba) {
    host_submit_frame(rgba, kStreamWidth, kStreamHeight);
}

// Game sound: the mixer's stereo output before the host's volume. Sent as
// 16-bit little-endian stereo PCM, about 1 Mbit/s.
constexpr uint32_t kAudioRate = 32000;
void host_submit_audio(const float* stereo, uint32_t frames); // audio thread, never blocks on the network
void encode_audio(const float* stereo, uint32_t frames, std::vector<uint8_t>& out); // appends
bool decode_audio(const uint8_t* in, size_t size, std::vector<int16_t>& stereo);    // appends

// Guest: "Splatoon.exe --join <host>[:port]". Returns the process exit code.
int run_guest(const std::string& address);
// "--join" from the command line, or RBREW_NET_JOIN; empty when not joining.
std::string join_address(int argc, char** argv);

} // namespace rbrew_net

// pad_remote.h — the GamePad screen and its touch on a phone browser (same local network).
//
// A small HTTP + WebSocket server on a thread of its own, started only while the menu option is on.
// The phone page shows the GamePad picture (JPEG) and sends one touch back; the game sends the next
// picture only once the phone has shown the previous one, so nothing ever queues.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pad_remote {
void start(int port); // window thread
void stop();
bool running();
int port();
// A phone is connected: the GPU copy of the GamePad picture must run.
bool wants_frames();
// Render thread: the GamePad picture (RGBA8); never blocks, copies only when the phone is ready.
void sink(const uint8_t* rgba, uint32_t width, uint32_t height);
// Addresses to open on the phone, most plausible first (private IPv4, Wi-Fi/Ethernet, not VPN/virtual).
std::vector<std::string> urls();
// One line for the menu: connected, waiting, or why the server could not start.
std::string status();
} // namespace pad_remote

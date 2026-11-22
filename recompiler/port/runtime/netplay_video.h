#pragma once
// H.264 for the online stream: Media Foundation on Windows (no extra
// dependency); other platforms report it as unavailable for now.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace rbrew_net {

// BT.601 limited range; width and height must be even.
void rgba_to_nv12(const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t stride, uint8_t* nv12);

class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();
    bool open(uint32_t width, uint32_t height, uint32_t fps, uint32_t kbps);
    // Appends the Annex B bytes this frame produced (can be empty).
    bool encode(const uint8_t* nv12, bool keyframe, std::vector<uint8_t>& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();
    bool open();
    // True when a picture came out: tightly packed NV12 of width x height.
    bool decode(const uint8_t* data, size_t size, std::vector<uint8_t>& nv12, uint32_t& width, uint32_t& height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rbrew_net

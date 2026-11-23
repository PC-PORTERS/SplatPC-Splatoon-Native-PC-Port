#include "netplay_video.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <initguid.h>
#include <codecapi.h>
#include <wrl/client.h>
#include <mutex>
using Microsoft::WRL::ComPtr;
#endif

namespace rbrew_net {

void rgba_to_nv12(const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t stride, uint8_t* nv12) {
    uint8_t* y_plane = nv12;
    uint8_t* uv_plane = nv12 + size_t(width) * height;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = rgba + size_t(y) * stride;
        for (uint32_t x = 0; x < width; ++x) {
            const int r = row[x * 4], g = row[x * 4 + 1], b = row[x * 4 + 2];
            y_plane[size_t(y) * width + x] = uint8_t(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    for (uint32_t y = 0; y < height; y += 2) {
        const uint8_t* top = rgba + size_t(y) * stride;
        const uint8_t* bottom = top + stride;
        uint8_t* out = uv_plane + size_t(y / 2) * width;
        for (uint32_t x = 0; x < width; x += 2) {
            int r = 0, g = 0, b = 0;
            for (const uint8_t* p : {top + x * 4, top + x * 4 + 4, bottom + x * 4, bottom + x * 4 + 4}) {
                r += p[0];
                g += p[1];
                b += p[2];
            }
            r /= 4;
            g /= 4;
            b /= 4;
            out[x] = uint8_t(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            out[x + 1] = uint8_t(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

#if defined(_WIN32)

namespace {

bool media_foundation_ready() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] { ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); });
    // COM per calling thread; S_FALSE / RPC_E_CHANGED_MODE leave it usable.
    thread_local const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)com;
    return ok;
}

ComPtr<IMFTransform> find_transform(const GUID& category, const GUID& input, const GUID& output) {
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, input}, out{MFMediaType_Video, output};
    IMFActivate** found = nullptr;
    UINT32 count = 0;
    ComPtr<IMFTransform> transform;
    if (FAILED(MFTEnumEx(category, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in,
                         &out, &found, &count)))
        return transform;
    for (UINT32 i = 0; i < count; ++i) {
        if (!transform) found[i]->ActivateObject(IID_PPV_ARGS(&transform));
        found[i]->Release();
    }
    CoTaskMemFree(found);
    return transform;
}

// Explicit IID: llvm-mingw's strmif.h omits __CRT_UUID_DECL(ICodecAPI), breaking IID_PPV_ARGS.
constexpr IID kIidCodecApi = {0x901db4c7u, 0x31ce, 0x41a2, {0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda}};

void set_codec_value(IMFTransform* transform, const GUID& key, UINT32 value, bool boolean = false) {
    void* raw = nullptr;
    if (FAILED(transform->QueryInterface(kIidCodecApi, &raw)) || !raw) return;
    ComPtr<ICodecAPI> codec;
    codec.Attach(static_cast<ICodecAPI*>(raw));
    VARIANT v;
    VariantInit(&v);
    if (boolean) {
        v.vt = VT_BOOL;
        v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    } else {
        v.vt = VT_UI4;
        v.ulVal = value;
    }
    codec->SetValue(&key, &v);
}

ComPtr<IMFSample> make_sample(const uint8_t* data, DWORD size) {
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample;
    BYTE* target = nullptr;
    if (FAILED(MFCreateMemoryBuffer(size, &buffer)) || FAILED(buffer->Lock(&target, nullptr, nullptr))) return {};
    if (data) std::memcpy(target, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength(data ? size : 0);
    if (FAILED(MFCreateSample(&sample)) || FAILED(sample->AddBuffer(buffer.Get()))) return {};
    return sample;
}

// One ProcessOutput call into a caller-allocated sample (software MFTs).
HRESULT pull_output(IMFTransform* transform, ComPtr<IMFSample>& sample) {
    MFT_OUTPUT_STREAM_INFO info{};
    if (FAILED(transform->GetOutputStreamInfo(0, &info))) return E_FAIL;
    const bool provides = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES));
    MFT_OUTPUT_DATA_BUFFER output{};
    if (!provides) {
        sample = make_sample(nullptr, std::max<DWORD>(info.cbSize, 1));
        output.pSample = sample.Get();
    }
    DWORD status = 0;
    const HRESULT hr = transform->ProcessOutput(0, 1, &output, &status);
    if (output.pEvents) output.pEvents->Release();
    if (provides && output.pSample) sample.Attach(output.pSample);
    return hr;
}

void append_sample(IMFSample* sample, std::vector<uint8_t>& out) {
    ComPtr<IMFMediaBuffer> buffer;
    BYTE* data = nullptr;
    DWORD length = 0;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || FAILED(buffer->Lock(&data, nullptr, &length))) return;
    out.insert(out.end(), data, data + length);
    buffer->Unlock();
}

} // namespace

struct H264Encoder::Impl {
    ComPtr<IMFTransform> transform;
    uint32_t width = 0, height = 0, fps = 60;
    LONGLONG time = 0;
};

H264Encoder::H264Encoder() : impl_(std::make_unique<Impl>()) {}
H264Encoder::~H264Encoder() = default;

bool H264Encoder::open(uint32_t width, uint32_t height, uint32_t fps, uint32_t kbps) {
    if (!media_foundation_ready()) return false;
    auto& t = impl_->transform;
    t = find_transform(MFT_CATEGORY_VIDEO_ENCODER, MFVideoFormat_NV12, MFVideoFormat_H264);
    if (!t) return false;
    set_codec_value(t.Get(), CODECAPI_AVLowLatencyMode, 1, true);
    set_codec_value(t.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
    set_codec_value(t.Get(), CODECAPI_AVEncCommonMeanBitRate, kbps * 1000);
    set_codec_value(t.Get(), CODECAPI_AVEncMPVGOPSize, fps * 2);

    ComPtr<IMFMediaType> output, input;
    if (FAILED(MFCreateMediaType(&output)) || FAILED(MFCreateMediaType(&input))) return false;
    output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    output->SetUINT32(MF_MT_AVG_BITRATE, kbps * 1000);
    output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
    MFSetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(output.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(output.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(t->SetOutputType(0, output.Get(), 0))) return false;
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(input.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(t->SetInputType(0, input.Get(), 0))) return false;
    t->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    t->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    impl_->width = width;
    impl_->height = height;
    impl_->fps = fps;
    return true;
}

bool H264Encoder::encode(const uint8_t* nv12, bool keyframe, std::vector<uint8_t>& out) {
    auto& t = impl_->transform;
    if (!t) return false;
    if (keyframe) set_codec_value(t.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
    const DWORD size = DWORD(size_t(impl_->width) * impl_->height * 3 / 2);
    ComPtr<IMFSample> sample = make_sample(nv12, size);
    if (!sample) return false;
    const LONGLONG duration = 10000000LL / impl_->fps;
    sample->SetSampleTime(impl_->time);
    sample->SetSampleDuration(duration);
    impl_->time += duration;
    if (FAILED(t->ProcessInput(0, sample.Get(), 0))) return false;
    for (;;) {
        ComPtr<IMFSample> encoded;
        const HRESULT hr = pull_output(t.Get(), encoded);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (FAILED(hr)) return false;
        append_sample(encoded.Get(), out);
    }
}

struct H264Decoder::Impl {
    ComPtr<IMFTransform> transform;
    uint32_t width = 0, height = 0, coded_height = 0, stride = 0;
};

H264Decoder::H264Decoder() : impl_(std::make_unique<Impl>()) {}
H264Decoder::~H264Decoder() = default;

static bool select_nv12_output(IMFTransform* t, uint32_t& width, uint32_t& height, uint32_t& coded_height,
                               uint32_t& stride) {
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        if (FAILED(t->GetOutputAvailableType(0, i, &type))) return false;
        GUID subtype{};
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12) continue;
        if (FAILED(t->SetOutputType(0, type.Get(), 0))) return false;
        ComPtr<IMFMediaType> current;
        IMFMediaType* chosen = SUCCEEDED(t->GetOutputCurrentType(0, &current)) && current ? current.Get() : type.Get();
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(chosen, MF_MT_FRAME_SIZE, &w, &h);
        width = w;
        height = coded_height = h;
        MFVideoArea area{};
        for (const GUID& attr : {MF_MT_MINIMUM_DISPLAY_APERTURE, MF_MT_GEOMETRIC_APERTURE}) {
            if (SUCCEEDED(chosen->GetBlob(attr, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr)) &&
                area.OffsetX.value == 0 && area.OffsetY.value == 0 && area.Area.cx > 0 && area.Area.cy > 0 &&
                uint32_t(area.Area.cx) <= w && uint32_t(area.Area.cy) <= h) {
                width = uint32_t(area.Area.cx) & ~1u;
                height = uint32_t(area.Area.cy) & ~1u;
                break;
            }
        }
        stride = MFGetAttributeUINT32(chosen, MF_MT_DEFAULT_STRIDE, w);
        return true;
    }
}

bool H264Decoder::open() {
    if (!media_foundation_ready()) return false;
    auto& t = impl_->transform;
    t = find_transform(MFT_CATEGORY_VIDEO_DECODER, MFVideoFormat_H264, MFVideoFormat_NV12);
    if (!t) return false;
    // Without this the decoder holds pictures back for frame reordering.
    ComPtr<IMFAttributes> attributes;
    // CODECAPI_AVLowLatencyMode has the same GUID as MF_LOW_LATENCY and is available under llvm-mingw.
    if (SUCCEEDED(t->GetAttributes(&attributes))) attributes->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
    set_codec_value(t.Get(), CODECAPI_AVLowLatencyMode, 1, true);
    ComPtr<IMFMediaType> input;
    if (FAILED(MFCreateMediaType(&input))) return false;
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(t->SetInputType(0, input.Get(), 0))) return false;
    select_nv12_output(t.Get(), impl_->width, impl_->height, impl_->coded_height, impl_->stride);
    t->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    t->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool H264Decoder::decode(const uint8_t* data, size_t size, std::vector<uint8_t>& nv12, uint32_t& width,
                         uint32_t& height) {
    auto& t = impl_->transform;
    if (!t || !size) return false;
    ComPtr<IMFSample> sample = make_sample(data, DWORD(size));
    if (!sample || FAILED(t->ProcessInput(0, sample.Get(), 0))) return false;
    bool picture = false;
    for (;;) {
        ComPtr<IMFSample> decoded;
        const HRESULT hr = pull_output(t.Get(), decoded);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!select_nv12_output(t.Get(), impl_->width, impl_->height, impl_->coded_height, impl_->stride))
                return false;
            continue;
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return picture;
        if (FAILED(hr)) return picture;
        std::vector<uint8_t> raw;
        append_sample(decoded.Get(), raw);
        const uint32_t w = impl_->width, h = impl_->height, coded_h = std::max(impl_->coded_height, h),
                       stride = std::max(impl_->stride, w);
        if (!w || !h || raw.size() < size_t(stride) * coded_h * 3 / 2) continue;
        // Planes may be padded: the UV plane starts after the aligned luma height.
        const size_t luma_rows = raw.size() / (size_t(stride) * 3 / 2);
        nv12.resize(size_t(w) * h * 3 / 2);
        for (uint32_t y = 0; y < h; ++y) std::memcpy(&nv12[size_t(y) * w], &raw[size_t(y) * stride], w);
        const uint8_t* uv = raw.data() + size_t(stride) * std::max<size_t>(luma_rows, coded_h);
        for (uint32_t y = 0; y < h / 2; ++y)
            std::memcpy(&nv12[size_t(w) * h + size_t(y) * w], uv + size_t(y) * stride, w);
        width = w;
        height = h;
        picture = true;
    }
}

#else

struct H264Encoder::Impl {};
H264Encoder::H264Encoder() : impl_(std::make_unique<Impl>()) {}
H264Encoder::~H264Encoder() = default;
bool H264Encoder::open(uint32_t, uint32_t, uint32_t, uint32_t) {
    return false;
}
bool H264Encoder::encode(const uint8_t*, bool, std::vector<uint8_t>&) {
    return false;
}
struct H264Decoder::Impl {};
H264Decoder::H264Decoder() : impl_(std::make_unique<Impl>()) {}
H264Decoder::~H264Decoder() = default;
bool H264Decoder::open() {
    return false;
}
bool H264Decoder::decode(const uint8_t*, size_t, std::vector<uint8_t>&, uint32_t&, uint32_t&) {
    return false;
}

#endif

} // namespace rbrew_net

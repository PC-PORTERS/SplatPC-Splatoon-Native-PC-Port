#include "zlib_bridge.h"
#include "../os_common.h"
#include <zlib.h>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace rbrew_zlib {
namespace {
// Guest z_stream offsets (zlib 1.2.5, ILP32).
constexpr uint32_t kNextIn = 0x00, kAvailIn = 0x04, kTotalIn = 0x08, kNextOut = 0x0C, kAvailOut = 0x10,
                   kTotalOut = 0x14, kMsg = 0x18, kState = 0x1C, kDataType = 0x2C, kAdler = 0x30;

struct Stream {
    z_stream z{}; bool deflating = false;
    ~Stream() { if (z.state) { if (deflating) ::deflateEnd(&z); else ::inflateEnd(&z); } }
};
struct StreamKey {
    const uint8_t* mem; uint32_t address;
    bool operator==(const StreamKey&) const = default;
};
struct StreamKeyHash { size_t operator()(const StreamKey& k) const { return std::hash<const void*>{}(k.mem) ^ (size_t(k.address) << 1); } };
std::mutex s_mutex;
std::unordered_map<StreamKey, std::unique_ptr<Stream>, StreamKeyHash> s_streams;

Stream* find(const uint8_t* mem, uint32_t strm) {
    const auto it = s_streams.find({mem,strm});
    return it == s_streams.end() ? nullptr : it->second.get();
}
void drop(const uint8_t* mem, uint32_t strm) {
    s_streams.erase({mem,strm});
}
// Guest buffers -> host stream. False if a buffer is outside guest memory.
bool load(CPUState* cpu, uint32_t strm, z_stream& z) {
    if (!rbrew_memory_accessible(MEM, strm, kGuestStreamSize)) return false;
    const uint32_t in = rbrew_read32(MEM, strm + kNextIn), avail_in = rbrew_read32(MEM, strm + kAvailIn);
    const uint32_t out = rbrew_read32(MEM, strm + kNextOut), avail_out = rbrew_read32(MEM, strm + kAvailOut);
    if ((avail_in && (!in || !rbrew_memory_accessible(MEM, in, avail_in))) || (avail_out && (!out || !rbrew_memory_accessible(MEM, out, avail_out))))
        return false;
    z.next_in = in ? MEM + in : Z_NULL; z.avail_in = avail_in;
    z.next_out = out ? MEM + out : Z_NULL; z.avail_out = avail_out;
    z.total_in = rbrew_read32(MEM, strm + kTotalIn); z.total_out = rbrew_read32(MEM, strm + kTotalOut);
    return true;
}
void store(CPUState* cpu, uint32_t strm, const z_stream& z, bool open) {
    rbrew_write32(MEM, strm + kNextIn, (z.next_in ? uint32_t(z.next_in - MEM) : 0u)); rbrew_write32(MEM, strm + kAvailIn, z.avail_in);
    rbrew_write32(MEM, strm + kTotalIn, uint32_t(z.total_in));
    rbrew_write32(MEM, strm + kNextOut, (z.next_out ? uint32_t(z.next_out - MEM) : 0u)); rbrew_write32(MEM, strm + kAvailOut, z.avail_out);
    rbrew_write32(MEM, strm + kTotalOut, uint32_t(z.total_out));
    rbrew_write32(MEM, strm + kMsg, 0);
    rbrew_write32(MEM, strm + kState, open ? strm : 0u);   // non-null while the stream is live
    rbrew_write32(MEM, strm + kDataType, uint32_t(z.data_type)); rbrew_write32(MEM, strm + kAdler, uint32_t(z.adler));
}
void ret(CPUState* cpu, int result) { RET = uint32_t(result); }

template <class Init>
void open(CPUState* cpu, uint32_t strm, uint32_t stream_size, bool deflating, Init init) {
    std::lock_guard operation(s_mutex);
    if (stream_size != kGuestStreamSize) return ret(cpu, Z_VERSION_ERROR);
    auto s = std::make_unique<Stream>();
    s->deflating = deflating;
    if (!load(cpu, strm, s->z)) return ret(cpu, Z_STREAM_ERROR);
    const int result = init(s->z);
    if (result != Z_OK) return ret(cpu, result);
    store(cpu, strm, s->z, true);
    {
        // Operation lock also serializes reinitialization with Step/End.
        auto& slot = s_streams[{MEM,strm}];
        // The old Stream releases its zlib allocations through RAII.
        slot = std::move(s);
    }
    ret(cpu, Z_OK);
}
template <class Step>
void step(CPUState* cpu, uint32_t strm, bool deflating, Step run) {
    std::lock_guard operation(s_mutex);
    Stream* s = find(MEM,strm);
    if (!s || s->deflating != deflating || !load(cpu, strm, s->z)) return ret(cpu, Z_STREAM_ERROR);
    const int result = run(s->z);
    store(cpu, strm, s->z, true);
    ret(cpu, result);
}
void close(CPUState* cpu, uint32_t strm, bool deflating) {
    std::lock_guard operation(s_mutex);
    Stream* s = find(MEM,strm);
    if (!s || s->deflating != deflating) return ret(cpu, Z_STREAM_ERROR);
    const int result = deflating ? ::deflateEnd(&s->z) : ::inflateEnd(&s->z);
    if (rbrew_memory_accessible(MEM, strm, kGuestStreamSize)) rbrew_write32(MEM, strm + kState, 0);
    drop(MEM,strm);
    ret(cpu, result);
}
}

void inflateInit2_(CPUState* cpu) {
    const int bits = int(ARG1);
    open(cpu, ARG0, ARG3, false, [bits](z_stream& z) { return ::inflateInit2(&z, bits); });
}
void inflateInit_(CPUState* cpu) {
    open(cpu, ARG0, ARG2, false, [](z_stream& z) { return ::inflateInit(&z); });
}
void inflate(CPUState* cpu) {
    const int flush = int(ARG1);
    step(cpu, ARG0, false, [flush](z_stream& z) { return ::inflate(&z, flush); });
}
void inflateEnd(CPUState* cpu) { close(cpu, ARG0, false); }
void deflateInit2_(CPUState* cpu) {
    const int level = int(ARG1), method = int(ARG2), bits = int(ARG3), mem_level = int(ARG4), strategy = int(ARG5);
    open(cpu, ARG0, ARG7, true, [=](z_stream& z) { return ::deflateInit2(&z, level, method, bits, mem_level, strategy); });
}
void deflate(CPUState* cpu) {
    const int flush = int(ARG1);
    step(cpu, ARG0, true, [flush](z_stream& z) { return ::deflate(&z, flush); });
}
void deflateEnd(CPUState* cpu) { close(cpu, ARG0, true); }
void deflateBound(CPUState* cpu) {
    std::lock_guard operation(s_mutex);
    Stream* s = find(MEM,ARG0);
    RET = uint32_t(s && s->deflating ? ::deflateBound(&s->z, ARG1) : ::compressBound(ARG1));
}
size_t open_streams() { std::lock_guard lock(s_mutex); return s_streams.size(); }
}

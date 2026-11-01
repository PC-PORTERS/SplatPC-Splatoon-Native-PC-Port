#pragma once
#include "rebrewu_runtime.h"
// zlib125 streaming API for the guest (deflate*/inflate*). The game's NEX
// library links a compression plugin and sead a zlib stream; the blank STUBs
// returned their first argument and produced no data. Each guest z_stream
// (zlib 1.2.5, 32-bit big-endian, 0x38 bytes) is mirrored into a host
// z_stream for the duration of a call. Guest zalloc/zfree are not used: host
// zlib owns the state and the guest `state` field holds a non-zero cookie.
namespace rbrew_zlib {
constexpr uint32_t kGuestStreamSize = 0x38;
void inflateInit2_(CPUState* cpu);   // (strm, windowBits, version, stream_size)
void inflateInit_(CPUState* cpu);    // (strm, version, stream_size)
void inflate(CPUState* cpu);         // (strm, flush)
void inflateEnd(CPUState* cpu);      // (strm)
void deflateInit2_(CPUState* cpu);   // (strm, level, method, windowBits, memLevel, strategy, version, stream_size)
void deflate(CPUState* cpu);         // (strm, flush)
void deflateEnd(CPUState* cpu);      // (strm)
void deflateBound(CPUState* cpu);    // (strm, sourceLen)
size_t open_streams();               // tests: streams not yet ended
}

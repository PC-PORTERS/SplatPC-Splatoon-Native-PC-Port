#include "py_random.h"
#include "hashing.h"

#include <string>

namespace rbrew_native {

// CPython: a str seed becomes the integer of (seed bytes + sha512(seed bytes)), big-endian, whose 32-bit
// words (least significant first) feed init_by_array.
PyRandom::PyRandom(std::string_view seed) {
    std::string bytes(seed);
    const auto digest = sha512(seed);
    bytes.append(reinterpret_cast<const char*>(digest.data()), digest.size());
    size_t first = 0;
    while (first < bytes.size() && bytes[first] == 0) ++first; // leading zero bytes are not part of the integer
    std::vector<uint32_t> key;
    for (size_t end = bytes.size(); end > first;) {
        const size_t begin = end >= first + 4 ? end - 4 : first;
        uint32_t word = 0;
        for (size_t i = begin; i < end; ++i) word = (word << 8) | uint8_t(bytes[i]);
        key.push_back(word);
        end = begin;
    }
    if (key.empty()) key.push_back(0);
    init_by_array(key);
}

void PyRandom::init_by_array(const std::vector<uint32_t>& key) {
    state_[0] = 19650218u;
    for (int i = 1; i < 624; ++i) state_[i] = 1812433253u * (state_[i - 1] ^ (state_[i - 1] >> 30)) + uint32_t(i);
    size_t i = 1, j = 0;
    for (size_t k = key.size() > 624 ? key.size() : 624; k; --k) {
        state_[i] = (state_[i] ^ ((state_[i - 1] ^ (state_[i - 1] >> 30)) * 1664525u)) + key[j] + uint32_t(j);
        ++i;
        ++j;
        if (i >= 624) {
            state_[0] = state_[623];
            i = 1;
        }
        if (j >= key.size()) j = 0;
    }
    for (size_t k = 623; k; --k) {
        state_[i] = (state_[i] ^ ((state_[i - 1] ^ (state_[i - 1] >> 30)) * 1566083941u)) - uint32_t(i);
        ++i;
        if (i >= 624) {
            state_[0] = state_[623];
            i = 1;
        }
    }
    state_[0] = 0x80000000u;
    index_ = 624;
}

uint32_t PyRandom::next32() {
    if (index_ >= 624) {
        for (int k = 0; k < 624; ++k) {
            const uint32_t y = (state_[k] & 0x80000000u) | (state_[(k + 1) % 624] & 0x7fffffffu);
            state_[k] = state_[(k + 397) % 624] ^ (y >> 1) ^ ((y & 1u) ? 0x9908b0dfu : 0u);
        }
        index_ = 0;
    }
    uint32_t y = state_[index_++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    y ^= y >> 18;
    return y;
}

uint32_t PyRandom::getrandbits(int bits) {
    return next32() >> (32 - bits);
}

uint32_t PyRandom::randbelow(uint32_t n) {
    if (n == 0) return 0;
    int bits = 0;
    for (uint32_t v = n; v; v >>= 1) ++bits;
    uint32_t r = getrandbits(bits);
    while (r >= n) r = getrandbits(bits);
    return r;
}

} // namespace rbrew_native

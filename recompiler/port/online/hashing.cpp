#include "hashing.h"

#include <cstring>
#include <stdexcept>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace rbrew_native {

#if defined(_WIN32)

Sha256::Sha256() {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        throw std::runtime_error("SHA-256 unavailable");
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("SHA-256 unavailable");
    }
    algorithm_ = algorithm;
    hash_ = hash;
}

Sha256::~Sha256() {
    if (hash_) BCryptDestroyHash(hash_);
    if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
}

void Sha256::update(const void* data, size_t size) {
    auto bytes = static_cast<const unsigned char*>(data);
    while (size) { // BCryptHashData takes a ULONG length
        const ULONG chunk = ULONG(size > 0x40000000 ? 0x40000000 : size);
        if (BCryptHashData(hash_, const_cast<PUCHAR>(bytes), chunk, 0) != 0) throw std::runtime_error("SHA-256 failed");
        bytes += chunk;
        size -= chunk;
    }
}

std::string Sha256::hex() {
    unsigned char digest[32];
    if (BCryptFinishHash(hash_, digest, sizeof digest, 0) != 0) throw std::runtime_error("SHA-256 failed");
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char b : digest) {
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    return out;
}

std::array<uint8_t, 64> sha512(std::string_view data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA512_ALGORITHM, nullptr, 0) != 0)
        throw std::runtime_error("SHA-512 unavailable");
    std::array<uint8_t, 64> out{};
    const NTSTATUS status = BCryptHash(algorithm, nullptr, 0, (PUCHAR)data.data(), ULONG(data.size()), out.data(), 64);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status != 0) throw std::runtime_error("SHA-512 failed");
    return out;
}

#else

namespace {

inline uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}
inline uint64_t rotr64(uint64_t x, uint64_t n) {
    return (x >> n) | (x << (64 - n));
}

struct Sha256State {
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    uint64_t total_bytes = 0;
    uint8_t buf[64]{};
    size_t buf_len = 0;

    static void transform(uint32_t h[8], const uint8_t block[64]) {
        static constexpr uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
                   (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            const uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    void update(const uint8_t* p, size_t n) {
        total_bytes += n;
        if (buf_len > 0) {
            const size_t take = std::min(64 - buf_len, n);
            std::memcpy(buf + buf_len, p, take);
            buf_len += take;
            p += take;
            n -= take;
            if (buf_len == 64) {
                transform(h, buf);
                buf_len = 0;
            }
        }
        while (n >= 64) {
            transform(h, p);
            p += 64;
            n -= 64;
        }
        if (n > 0) {
            std::memcpy(buf, p, n);
            buf_len = n;
        }
    }

    std::string finish_hex() {
        const uint64_t bits = total_bytes * 8;
        const uint8_t pad = 0x80;
        update(&pad, 1);
        const uint8_t zero = 0x00;
        while (buf_len != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
        // Append length without altering total_bytes logic (transform directly since buf_len == 56)
        std::memcpy(buf + 56, len, 8);
        transform(h, buf);
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (uint32_t word : h) {
            for (int shift = 28; shift >= 0; shift -= 4) out += kHex[(word >> shift) & 0xf];
        }
        return out;
    }
};

} // namespace

Sha256::Sha256() {
    hash_ = new Sha256State();
}

Sha256::~Sha256() {
    delete static_cast<Sha256State*>(hash_);
}

void Sha256::update(const void* data, size_t size) {
    if (!hash_ || !data || !size) return;
    static_cast<Sha256State*>(hash_)->update(static_cast<const uint8_t*>(data), size);
}

std::string Sha256::hex() {
    if (!hash_) throw std::runtime_error("SHA-256 failed");
    return static_cast<Sha256State*>(hash_)->finish_hex();
}

std::array<uint8_t, 64> sha512(std::string_view data) {
    static constexpr uint64_t k[80] = {
        0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
        0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
        0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
        0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
        0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
        0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
        0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
        0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
        0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
        0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
        0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
        0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
        0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
        0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
        0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
        0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
        0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
        0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
        0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
        0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull};
    uint64_t h[8] = {0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
                     0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull};
    auto transform = [&](const uint8_t block[128]) {
        uint64_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = 0;
            for (int b = 0; b < 8; ++b) w[i] = (w[i] << 8) | block[i * 8 + b];
        }
        for (int i = 16; i < 80; ++i) {
            const uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
            const uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 80; ++i) {
            const uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
            const uint64_t ch = (e & f) ^ ((~e) & g);
            const uint64_t t1 = hh + S1 + ch + k[i] + w[i];
            const uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
            const uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint64_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    };

    const size_t total = data.size();
    size_t pos = 0;
    while (total - pos >= 128) {
        transform(reinterpret_cast<const uint8_t*>(data.data()) + pos);
        pos += 128;
    }
    uint8_t tail[256]{};
    const size_t rem = total - pos;
    if (rem) std::memcpy(tail, data.data() + pos, rem);
    tail[rem] = 0x80;
    const size_t blocks = (rem + 1 + 16 <= 128) ? 1 : 2;
    const uint64_t bit_len_lo = uint64_t(total) * 8ull;
    const uint64_t bit_len_hi = uint64_t(total) >> 61;
    for (int i = 0; i < 8; ++i) tail[blocks * 128 - 16 + i] = uint8_t(bit_len_hi >> (56 - 8 * i));
    for (int i = 0; i < 8; ++i) tail[blocks * 128 - 8 + i] = uint8_t(bit_len_lo >> (56 - 8 * i));
    transform(tail);
    if (blocks == 2) transform(tail + 128);

    std::array<uint8_t, 64> out{};
    for (int i = 0; i < 8; ++i)
        for (int b = 0; b < 8; ++b) out[size_t(i * 8 + b)] = uint8_t(h[i] >> (56 - 8 * b));
    return out;
}

#endif

std::string sha256_hex(std::string_view data) {
    Sha256 hash;
    hash.update(data.data(), data.size());
    return hash.hex();
}

} // namespace rbrew_native

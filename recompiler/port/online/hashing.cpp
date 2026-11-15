#include "hashing.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <stdexcept>

namespace rbrew_native {

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

std::string sha256_hex(std::string_view data) {
    Sha256 hash;
    hash.update(data.data(), data.size());
    return hash.hex();
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

} // namespace rbrew_native

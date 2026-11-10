// hashing.h - SHA-256 and SHA-512 through Windows CNG (bcrypt), for the integrity proof and the
// Python-compatible random seed of the SpotPass rotation.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace rbrew_native {

// Incremental SHA-256 (the integrity proof hashes files block by block).
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(const void* data, size_t size);
    std::string hex(); // finishes the hash: lowercase hexadecimal, like hashlib's hexdigest()

private:
    void* algorithm_ = nullptr;
    void* hash_ = nullptr;
};

std::string sha256_hex(std::string_view data);
std::array<uint8_t, 64> sha512(std::string_view data);

} // namespace rbrew_native

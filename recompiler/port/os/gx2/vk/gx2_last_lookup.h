#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace gx2vk {
// Render-owner front cache only. The full cache retains every entry. Values
// must have stable addresses, and reset() must precede their destruction.
template <size_t Capacity, class T, size_t Slots = 64> class LastLookup {
    static_assert(Slots > 0 && (Slots & (Slots - 1)) == 0, "Slots must be a power of two");

public:
    T* find(std::string_view key) const noexcept {
        if (key.size() > Capacity) return nullptr;
        const auto& s = slots_[slot_for(key)];
        return s.epoch == epoch_ && s.value && s.size == key.size() && !std::memcmp(s.bytes.data(), key.data(), s.size)
                   ? s.value
                   : nullptr;
    }
    void remember(std::string_view key, T* value) noexcept {
        if (key.size() > Capacity) {
            reset();
            return;
        }
        auto& s = slots_[slot_for(key)];
        std::memcpy(s.bytes.data(), key.data(), key.size());
        s.size = key.size();
        s.value = value;
        s.epoch = epoch_;
    }
    void reset() noexcept { ++epoch_; }
    uint64_t epoch() const noexcept { return epoch_; }

private:
    struct Slot {
        std::array<char, Capacity> bytes{};
        size_t size = 0;
        T* value = nullptr;
        uint64_t epoch = 0;
    };
    static size_t slot_for(std::string_view key) noexcept {
        uint64_t h = key.size() * 0x9E3779B97F4A7C15ull;
        if (key.size() >= 24) {
            uint64_t a, b, c;
            std::memcpy(&a, key.data(), 8);
            std::memcpy(&b, key.data() + (key.size() >> 1) - 4, 8);
            std::memcpy(&c, key.data() + key.size() - 8, 8);
            h ^= a * 0xBF58476D1CE4E5B9ull;
            h ^= b * 0x94D049BB133111EBull;
            h ^= c;
        } else {
            for (unsigned char ch : key) h = (h ^ ch) * 0x100000001B3ull;
        }
        h ^= h >> 30;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 27;
        return size_t(h) & (Slots - 1);
    }

    std::array<Slot, Slots> slots_{};
    uint64_t epoch_ = 1;
};
} // namespace gx2vk

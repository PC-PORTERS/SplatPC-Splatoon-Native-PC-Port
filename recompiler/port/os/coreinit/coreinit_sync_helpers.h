#pragma once
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>

// Positive OS ticks -> a rounded-up, overflow-safe steady-clock deadline.
inline std::chrono::steady_clock::time_point rbrew_timeout_deadline(uint64_t ticks, uint32_t frequency,
                                                                    std::chrono::steady_clock::time_point origin) {
    using namespace std::chrono;
    if (!frequency || !ticks) return origin;
    const uint64_t seconds = ticks / frequency, rest = ticks % frequency;
    if (seconds > UINT64_MAX / 1000000ull) return steady_clock::time_point::max();
    const uint64_t micros = seconds * 1000000ull + (rest * 1000000ull + frequency - 1) / frequency;
    const auto room = duration_cast<microseconds>(steady_clock::time_point::max() - origin).count();
    if (room <= 0 || micros >= uint64_t(room)) return steady_clock::time_point::max();
    return origin + microseconds(micros);
}

// Only touched when the mutex diagnostic is enabled. Atomic fields avoid
// reading an owner's plain stack array concurrently with its next lock.
class WatchedOwnerStack {
    std::atomic<uint64_t> sequence_{0};
    std::array<std::atomic<uint32_t>, 6> frames_{};

public:
    void store(const uint32_t* frames) {
        sequence_.fetch_add(1, std::memory_order_seq_cst);
        for (size_t i = 0; i < 6; ++i) frames_[i].store(frames[i], std::memory_order_seq_cst);
        sequence_.fetch_add(1, std::memory_order_seq_cst);
    }
    std::array<uint32_t, 6> snapshot() const {
        std::array<uint32_t, 6> result{};
        for (;;) {
            const auto before = sequence_.load(std::memory_order_seq_cst);
            if (before & 1) continue;
            for (size_t i = 0; i < 6; ++i) result[i] = frames_[i].load(std::memory_order_seq_cst);
            if (sequence_.load(std::memory_order_seq_cst) == before) return result;
        }
    }
};

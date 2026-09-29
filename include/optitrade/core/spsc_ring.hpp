#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <new>
#include <type_traits>
#include <vector>

namespace optitrade {

inline constexpr std::size_t kCacheLine = 64;

// Wait-free single-producer / single-consumer ring buffer. Each side keeps a
// cached copy of the other side's index so the shared cache lines are only
// touched when the ring looks full/empty.
template <class T>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>, "SpscRing stores raw copies");

public:
    // Capacity is rounded up to a power of two (minimum 2).
    explicit SpscRing(std::size_t capacity)
        : cap_(std::bit_ceil(capacity < 2 ? 2 : capacity)), mask_(cap_ - 1), buf_(cap_) {}

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    std::size_t capacity() const noexcept { return cap_; }

    // Producer thread only.
    bool try_push(const T& v) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t - cached_head_ >= cap_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (t - cached_head_ >= cap_) return false;
        }
        buf_[t & mask_] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Consumer thread only.
    bool try_pop(T& out) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (h == cached_tail_) return false;
        }
        out = buf_[h & mask_];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Approximate (racy) occupancy; for monitoring only.
    std::size_t size_approx() const noexcept {
        return tail_.load(std::memory_order_relaxed) - head_.load(std::memory_order_relaxed);
    }

private:
    const std::size_t cap_;
    const std::size_t mask_;
    std::vector<T> buf_;
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // consumer owned
    std::size_t cached_tail_{0};                            // consumer local
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // producer owned
    std::size_t cached_head_{0};                            // producer local
    alignas(kCacheLine) char pad_[1]{};
};

}  // namespace optitrade

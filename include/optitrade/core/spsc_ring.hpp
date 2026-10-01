#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace optitrade {

inline constexpr std::size_t kCacheLine = 64;

// Wait-free single-producer / single-consumer ring buffer. Each side keeps a
// cached copy of the other side's index so the shared cache lines are only
// touched when the ring looks full/empty.
//
// head_ and tail_ are free-running counters (never masked, wrap modulo 2^N), so
// `tail - head` is the occupancy and a full ring is distinguishable from an
// empty one without sacrificing a slot.
template <class T>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>, "SpscRing stores raw copies");

public:
    // Capacity is rounded up to a power of two (minimum 2). Throws
    // std::length_error if the rounded size is not representable.
    explicit SpscRing(std::size_t capacity)
        : cap_(rounded_capacity(capacity)), mask_(cap_ - 1), buf_(std::make_unique<T[]>(cap_)) {}

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

    // Occupancy snapshot, safe from any thread; for monitoring only. head is read
    // first (acquire) and tail second: any tail the head can depend on is then
    // already visible, so tail >= head and the difference cannot wrap. The producer
    // may still run ahead between the two loads, hence the clamp to the capacity.
    std::size_t size_approx() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return std::min(t - h, cap_);
    }

private:
    static std::size_t rounded_capacity(std::size_t capacity) {
        constexpr std::size_t kLimit = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
        if (capacity > kLimit) throw std::length_error("SpscRing: capacity too large");
        return std::bit_ceil(capacity < 2 ? std::size_t{2} : capacity);
    }

    // Read-only after construction: shared by both threads, never written, so it
    // does not bounce between cores. (Not a std::vector: vector<bool> packs bits,
    // which would turn neighbouring slots into a data race.)
    const std::size_t cap_;
    const std::size_t mask_;
    std::unique_ptr<T[]> buf_;
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // consumer owned
    std::size_t cached_tail_{0};                            // consumer local
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // producer owned
    std::size_t cached_head_{0};                            // producer local
};

}  // namespace optitrade

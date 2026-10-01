#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace optitrade {

// Fixed-capacity object pool addressed by 32-bit handles (half the size of a
// pointer, stable across the pool's lifetime, trivially serialisable).
// allocate()/release() are O(1) and never touch the heap after construction.
//
// Free nodes form an intrusive LIFO list: a fresh pool hands out 0, 1, 2, ... and
// afterwards the most recently released handle is reused first, which keeps the
// hot part of the pool cache resident and makes the handle sequence reproducible.
//
// Allocated nodes carry a sentinel in their `next` field, so release() can tell a
// live handle from a free or bogus one. Releasing a handle twice in a row, or an
// out-of-range one, is a no-op instead of a corrupted free list (a cycle would hand
// the same node to two owners). Handles carry no generation: a stale handle released
// after its node was handed out again frees the new owner's node, so callers must
// drop a handle when they release it. operator[] stays unchecked on the hot path
// apart from a debug assert.
template <class T>
class SlabPool {
public:
    using Handle = std::uint32_t;
    static constexpr Handle kNull = 0xFFFFFFFFu;
    // The two largest 32-bit values are reserved (kNull and the allocated sentinel),
    // so every valid handle, being < capacity <= kMaxCapacity, stays clear of them.
    static constexpr std::size_t kMaxCapacity = 0xFFFFFFFEu;

    // Throws std::length_error above kMaxCapacity and std::bad_alloc when the memory
    // is not available; construction is the only place the pool can fail.
    explicit SlabPool(std::size_t capacity) : nodes_(checked(capacity)) {
        for (std::size_t i = 0; i < capacity; ++i)
            nodes_[i].next = (i + 1 < capacity) ? static_cast<Handle>(i + 1) : kNull;
        free_ = capacity ? 0 : kNull;
    }

    // Returns kNull when the pool is exhausted. The object is value-initialised.
    Handle allocate() noexcept {
        if (free_ == kNull) return kNull;
        const Handle h = free_;
        Node& n = nodes_[h];
        free_ = n.next;
        n.next = kAllocated;
        n.value = T{};
        ++size_;
        return h;
    }

    // Returns the node to the pool. Ignores handles that are not currently allocated
    // (kNull, out of range, already released). The stored value is left as is until
    // the node is allocated again.
    void release(Handle h) noexcept {
        if (!live(h)) return;
        nodes_[h].next = free_;
        free_ = h;
        --size_;
    }

    // True while `h` is allocated and not yet released.
    bool live(Handle h) const noexcept { return h < nodes_.size() && nodes_[h].next == kAllocated; }

    T& operator[](Handle h) noexcept {
        assert(h < nodes_.size());
        return nodes_[h].value;
    }
    const T& operator[](Handle h) const noexcept {
        assert(h < nodes_.size());
        return nodes_[h].value;
    }

    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return nodes_.size(); }

private:
    static constexpr Handle kAllocated = kNull - 1;

    struct Node {
        T value{};
        Handle next{kNull};
    };

    static std::size_t checked(std::size_t capacity) {
        if (capacity > kMaxCapacity) throw std::length_error("SlabPool: capacity too large");
        return capacity;
    }

    std::vector<Node> nodes_;
    Handle free_{kNull};
    std::size_t size_{0};
};

}  // namespace optitrade

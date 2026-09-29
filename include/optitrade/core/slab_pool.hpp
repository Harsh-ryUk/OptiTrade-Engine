#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace optitrade {

// Fixed-capacity object pool addressed by 32-bit handles (half the size of a
// pointer, stable across the pool's lifetime, trivially serialisable).
// allocate()/release() are O(1) and never touch the heap after construction.
template <class T>
class SlabPool {
public:
    using Handle = std::uint32_t;
    static constexpr Handle kNull = 0xFFFFFFFFu;

    explicit SlabPool(std::size_t capacity) : nodes_(capacity) {
        for (std::size_t i = 0; i < capacity; ++i)
            nodes_[i].next = (i + 1 < capacity) ? static_cast<Handle>(i + 1) : kNull;
        free_ = capacity ? 0 : kNull;
    }

    // Returns kNull when the pool is exhausted. The object is value-initialised.
    Handle allocate() noexcept {
        if (free_ == kNull) return kNull;
        const Handle h = free_;
        free_ = nodes_[h].next;
        nodes_[h].value = T{};
        ++size_;
        return h;
    }

    void release(Handle h) noexcept {
        nodes_[h].next = free_;
        free_ = h;
        --size_;
    }

    T& operator[](Handle h) noexcept { return nodes_[h].value; }
    const T& operator[](Handle h) const noexcept { return nodes_[h].value; }

    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return nodes_.size(); }

private:
    struct Node {
        T value{};
        Handle next{kNull};
    };
    std::vector<Node> nodes_;
    Handle free_{kNull};
    std::size_t size_{0};
};

}  // namespace optitrade

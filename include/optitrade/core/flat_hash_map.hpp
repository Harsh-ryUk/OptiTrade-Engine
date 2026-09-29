#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

#include "optitrade/core/types.hpp"

namespace optitrade {

inline constexpr std::uint64_t mix64(std::uint64_t x) noexcept {  // splitmix64 finaliser
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

template <class K, class = void>
struct Hash;

template <class K>
struct Hash<K, std::enable_if_t<std::is_integral_v<K>>> {
    std::uint64_t operator()(K k) const noexcept { return mix64(static_cast<std::uint64_t>(k)); }
};

template <>
struct Hash<Symbol, void> {
    std::uint64_t operator()(const Symbol& s) const noexcept {
        std::uint64_t v;
        std::memcpy(&v, s.raw().data(), sizeof v);
        return mix64(v);
    }
};

// Open-addressing hash map with linear probing and backward-shift deletion
// (no tombstones, so lookups never degrade after many erases).
//
// All memory is allocated in the constructor; insert/find/erase never allocate.
// Holds at most `max_elements` entries at <= 50 % load. K and V must be default
// constructible and movable.
template <class K, class V, class H = Hash<K>>
class FlatHashMap {
public:
    explicit FlatHashMap(std::size_t max_elements)
        : max_(max_elements),
          mask_(std::bit_ceil(std::max<std::size_t>(max_elements * 2, 8)) - 1),
          slots_(mask_ + 1) {}

    std::size_t size() const noexcept { return size_; }
    std::size_t max_size() const noexcept { return max_; }
    bool empty() const noexcept { return size_ == 0; }

    V* find(const K& key) noexcept {
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (!s.used) return nullptr;
            if (s.key == key) return &s.value;
        }
    }
    const V* find(const K& key) const noexcept {
        return const_cast<FlatHashMap*>(this)->find(key);
    }

    // Returns {value, true} when inserted, {existing value, false} when the key is
    // already present (value left untouched), and {nullptr, false} when full.
    std::pair<V*, bool> insert(const K& key, const V& value) noexcept(
        std::is_nothrow_copy_assignable_v<V>) {
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (!s.used) {
                if (size_ >= max_) return {nullptr, false};
                s.used = true;
                s.key = key;
                s.value = value;
                ++size_;
                return {&s.value, true};
            }
            if (s.key == key) return {&s.value, false};
        }
    }

    bool erase(const K& key) noexcept {
        std::size_t i = home(key);
        for (;; i = (i + 1) & mask_) {
            if (!slots_[i].used) return false;
            if (slots_[i].key == key) break;
        }
        // Backward-shift: pull later members of the probe run into the hole.
        for (std::size_t j = i;;) {
            j = (j + 1) & mask_;
            if (!slots_[j].used) break;
            const std::size_t k = home(slots_[j].key);
            const bool k_in_i_j = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
            if (k_in_i_j) continue;
            slots_[i] = std::move(slots_[j]);
            i = j;
        }
        slots_[i].used = false;
        slots_[i].key = K{};
        slots_[i].value = V{};
        --size_;
        return true;
    }

    void clear() noexcept {
        for (Slot& s : slots_) s = Slot{};
        size_ = 0;
    }

    template <class F>
    void for_each(F&& f) const {
        for (const Slot& s : slots_)
            if (s.used) f(s.key, s.value);
    }

private:
    struct Slot {
        K key{};
        V value{};
        bool used{false};
    };

    std::size_t home(const K& key) const noexcept { return H{}(key) & mask_; }

    std::size_t max_;
    std::size_t mask_;
    std::size_t size_{0};
    std::vector<Slot> slots_;
};

}  // namespace optitrade

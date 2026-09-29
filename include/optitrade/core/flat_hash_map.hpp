#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
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

// Integer keys are run through a full-avalanche mixer: order references and
// locates are sequential, which would otherwise pile into one probe run.
template <class K, class = void>
struct Hash;

template <class K>
struct Hash<K, std::enable_if_t<std::is_integral_v<K>>> {
    std::uint64_t operator()(K k) const noexcept { return mix64(static_cast<std::uint64_t>(k)); }
};

// Hashes the raw eight bytes, consistent with Symbol's byte-wise operator==.
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
// Holds at most `max_elements` entries at <= 50 % load, so a probe run always
// ends at an empty slot. K and V must be default constructible and movable.
//
// Pointer stability: insert() never relocates existing entries, but erase()
// does (backward shift moves later members of the probe run into the hole), so
// any V* obtained before an erase() must be treated as invalidated.
//
// Iteration order (for_each) is the table order and is unspecified: results
// must never depend on it.
template <class K, class V, class H = Hash<K>>
class FlatHashMap {
public:
    explicit FlatHashMap(std::size_t max_elements)
        : max_(max_elements), mask_(table_size(max_elements) - 1), slots_(mask_ + 1) {}

    std::size_t size() const noexcept { return size_; }
    std::size_t max_size() const noexcept { return max_; }
    bool empty() const noexcept { return size_ == 0; }

    V* find(const K& key) noexcept {
        Slot& s = slots_[probe(key)];
        return s.used ? &s.value : nullptr;
    }
    const V* find(const K& key) const noexcept {
        const Slot& s = slots_[probe(key)];
        return s.used ? &s.value : nullptr;
    }

    // Returns {value, true} when inserted, {existing value, false} when the key is
    // already present (value left untouched), and {nullptr, false} when full.
    // The slot is only marked used once key and value are stored.
    std::pair<V*, bool> insert(const K& key, const V& value) noexcept(
        std::is_nothrow_copy_assignable_v<K> && std::is_nothrow_copy_assignable_v<V>) {
        Slot& s = slots_[probe(key)];
        if (s.used) return {&s.value, false};
        if (size_ >= max_) return {nullptr, false};
        s.key = key;
        s.value = value;
        s.used = true;
        ++size_;
        return {&s.value, true};
    }

    bool erase(const K& key) noexcept {
        std::size_t hole = probe(key);
        if (!slots_[hole].used) return false;
        // Backward shift: walk the rest of the probe run and pull back every entry
        // that is allowed to occupy the hole, so lookups never need tombstones.
        for (std::size_t j = hole;;) {
            j = (j + 1) & mask_;
            if (!slots_[j].used) break;
            const std::size_t k = home(slots_[j].key);
            // An entry whose home slot lies cyclically inside (hole, j] would be
            // unreachable if moved before its home, so it has to stay.
            const bool stays = (hole <= j) ? (hole < k && k <= j) : (hole < k || k <= j);
            if (stays) continue;
            slots_[hole] = std::move(slots_[j]);
            hole = j;
        }
        slots_[hole] = Slot{};
        --size_;
        return true;
    }

    void clear() noexcept {
        for (Slot& s : slots_) s = Slot{};
        size_ = 0;
    }

    // Calls f(const K&, const V&) once for every live entry, in unspecified order.
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

    // Twice the entry limit, rounded up to a power of two (minimum 8 so tiny maps
    // still have room to probe). The guard keeps `max * 2` and bit_ceil in range.
    static std::size_t table_size(std::size_t max_elements) {
        constexpr std::size_t kLimit = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 2);
        if (max_elements > kLimit) throw std::length_error("FlatHashMap: capacity too large");
        return std::bit_ceil(std::max<std::size_t>(max_elements * 2, 8));
    }

    std::size_t home(const K& key) const noexcept { return H{}(key) & mask_; }

    // Slot holding `key`, or the empty slot that terminates its probe run. Always
    // terminates because at least half of the table is empty.
    std::size_t probe(const K& key) const noexcept {
        std::size_t i = home(key);
        while (slots_[i].used && !(slots_[i].key == key)) i = (i + 1) & mask_;
        return i;
    }

    std::size_t max_;
    std::size_t mask_;
    std::size_t size_{0};
    std::vector<Slot> slots_;
};

}  // namespace optitrade

// FlatHashMap: semantics, capacity limits, hand-built and exhaustive backward-shift
// layouts (including wrap-around at the end of the table), clear/for_each, Symbol
// keys and the hash functions. The randomized differential runs against
// std::unordered_map live in test_core_flat_hash_map_differential.cpp.

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>

#include "check.hpp"
#include "optitrade/core/flat_hash_map.hpp"
#include "optitrade/core/rng.hpp"

using namespace optitrade;

namespace {

// Home slot = key & mask: lets a test place entries in exact slots.
struct IdentityHash {
    std::uint64_t operator()(std::uint64_t k) const noexcept { return k; }
};

}  // namespace

// ---------------------------------------------------------------------------
// Basic behaviour
// ---------------------------------------------------------------------------

OT_TEST(new_map_is_empty) {
    FlatHashMap<std::uint64_t, int> m(10);
    OT_CHECK(m.empty());
    OT_CHECK_EQ(m.size(), std::size_t{0});
    OT_CHECK_EQ(m.max_size(), std::size_t{10});
    OT_CHECK(m.find(1) == nullptr);
    OT_CHECK(!m.erase(1));
    std::size_t visits = 0;
    m.for_each([&](const std::uint64_t&, const int&) { ++visits; });
    OT_CHECK_EQ(visits, std::size_t{0});
}

OT_TEST(insert_find_erase_round_trip) {
    FlatHashMap<std::uint64_t, int> m(8);
    const auto r1 = m.insert(100, 1);
    OT_CHECK(r1.first != nullptr && r1.second);
    OT_CHECK_EQ(*r1.first, 1);
    OT_CHECK(m.insert(200, 2).second);
    OT_CHECK(m.insert(300, 3).second);
    OT_CHECK_EQ(m.size(), std::size_t{3});
    OT_CHECK_EQ(*m.find(100), 1);
    OT_CHECK_EQ(*m.find(200), 2);
    OT_CHECK_EQ(*m.find(300), 3);
    OT_CHECK(m.find(400) == nullptr);

    OT_CHECK(m.erase(200));
    OT_CHECK(!m.erase(200));
    OT_CHECK(m.find(200) == nullptr);
    OT_CHECK_EQ(*m.find(100), 1);
    OT_CHECK_EQ(*m.find(300), 3);
    OT_CHECK_EQ(m.size(), std::size_t{2});
}

OT_TEST(duplicate_insert_returns_existing_and_keeps_value) {
    FlatHashMap<std::uint64_t, int> m(4);
    int* first = m.insert(7, 70).first;
    const auto dup = m.insert(7, 999);
    OT_CHECK(!dup.second);
    OT_CHECK(dup.first == first);
    OT_CHECK_EQ(*dup.first, 70);
    OT_CHECK_EQ(m.size(), std::size_t{1});
}

OT_TEST(values_are_writable_through_returned_pointers) {
    FlatHashMap<std::uint64_t, int> m(4);
    *m.insert(1, 10).first += 5;
    OT_CHECK_EQ(*m.find(1), 15);
    *m.find(1) = 42;
    const auto& cm = m;
    OT_CHECK_EQ(*cm.find(1), 42);
    OT_CHECK(cm.find(2) == nullptr);
}

// Unused slots hold a default-constructed key (0), so a lookup of key 0 must not
// mistake an empty slot for an entry.
OT_TEST(key_zero_is_an_ordinary_key) {
    FlatHashMap<std::uint64_t, int> m(4);
    OT_CHECK(m.find(0) == nullptr);
    OT_CHECK(!m.erase(0));
    OT_CHECK(m.insert(0, 11).second);
    OT_CHECK_EQ(*m.find(0), 11);
    OT_CHECK(!m.insert(0, 12).second);
    OT_CHECK(m.erase(0));
    OT_CHECK(m.find(0) == nullptr);
    OT_CHECK(m.empty());
}

OT_TEST(signed_and_narrow_key_types) {
    FlatHashMap<std::int32_t, int> a(16);
    OT_CHECK(a.insert(-1, 1).second);
    OT_CHECK(a.insert(0, 2).second);
    OT_CHECK(a.insert(INT32_MIN, 3).second);
    OT_CHECK_EQ(*a.find(-1), 1);
    OT_CHECK_EQ(*a.find(INT32_MIN), 3);
    OT_CHECK(a.find(1) == nullptr);

    FlatHashMap<std::uint16_t, int> b(300);
    for (int i = 0; i < 300; ++i) OT_CHECK(b.insert(static_cast<std::uint16_t>(i * 200), i).second);
    for (int i = 0; i < 300; ++i) OT_CHECK_EQ(*b.find(static_cast<std::uint16_t>(i * 200)), i);
}

// ---------------------------------------------------------------------------
// Capacity limits
// ---------------------------------------------------------------------------

// Sizes straddle the points where the table doubles (max 4 -> 8 slots, 5 -> 16).
OT_TEST(fill_to_max_size_then_reject) {
    for (std::size_t max : {1u, 2u, 3u, 4u, 5u, 7u, 8u, 9u, 15u, 16u, 17u, 100u, 1000u}) {
        FlatHashMap<std::uint64_t, std::uint64_t> m(max);
        Rng rng(max);
        std::vector<std::uint64_t> keys;
        while (keys.size() < max) {
            const std::uint64_t k = rng.next();
            if (m.find(k) != nullptr) continue;
            const auto r = m.insert(k, k ^ 1);
            OT_CHECK(r.first != nullptr && r.second);
            keys.push_back(k);
        }
        OT_CHECK_EQ(m.size(), max);
        OT_CHECK_EQ(m.max_size(), max);

        // A new key is refused and leaves the map untouched.
        std::uint64_t extra = rng.next();
        while (m.find(extra) != nullptr) extra = rng.next();
        const auto refused = m.insert(extra, 5);
        OT_CHECK(refused.first == nullptr);
        OT_CHECK(!refused.second);
        OT_CHECK_EQ(m.size(), max);
        OT_CHECK(m.find(extra) == nullptr);

        // An existing key is still answered when full, and lookups all work.
        const auto again = m.insert(keys[0], 5);
        OT_CHECK(again.first != nullptr && !again.second);
        OT_CHECK_EQ(*again.first, keys[0] ^ 1);
        for (std::uint64_t k : keys) OT_CHECK_EQ(*m.find(k), k ^ 1);

        // One erase frees exactly one place.
        OT_CHECK(m.erase(keys[max / 2]));
        OT_CHECK(m.insert(extra, 5).second);
        OT_CHECK(m.insert(keys[max / 2], 6).first == nullptr);
        OT_CHECK_EQ(m.size(), max);
    }
}

OT_TEST(zero_capacity_map_stores_nothing) {
    FlatHashMap<std::uint64_t, int> m(0);
    OT_CHECK_EQ(m.max_size(), std::size_t{0});
    const auto r = m.insert(1, 1);
    OT_CHECK(r.first == nullptr && !r.second);
    OT_CHECK(m.find(1) == nullptr);
    OT_CHECK(!m.erase(1));
    OT_CHECK(m.empty());
}

// Erasing everything and refilling repeatedly must not leak capacity (a tombstone
// scheme would fail here) nor slow lookups of absent keys into an endless scan.
OT_TEST(repeated_fill_and_drain_does_not_leak_capacity) {
    FlatHashMap<std::uint64_t, int> m(64);
    for (int round = 0; round < 200; ++round) {
        for (int i = 0; i < 64; ++i)
            OT_CHECK(m.insert(static_cast<std::uint64_t>(round) * 1000 + static_cast<std::uint64_t>(i), i).second);
        OT_CHECK(m.insert(999'999'999, 0).first == nullptr);
        for (int i = 0; i < 64; ++i) OT_CHECK(m.erase(static_cast<std::uint64_t>(round) * 1000 + static_cast<std::uint64_t>(i)));
        OT_CHECK(m.empty());
        OT_CHECK(m.find(12345) == nullptr);
    }
}

OT_TEST(absurd_capacity_is_rejected_not_undefined) {
    bool threw = false;
    try {
        FlatHashMap<std::uint64_t, int> m(SIZE_MAX / 2);
    } catch (const std::length_error&) {
        threw = true;
    }
    OT_CHECK(threw);
}

// ---------------------------------------------------------------------------
// Backward-shift deletion
// ---------------------------------------------------------------------------

// max 4 -> 8 slots, identity hash -> home = key & 7. Keys 6, 14, 22, 30 all live
// in the run 6,7,0,1 that wraps past the end of the table.
OT_TEST(backward_shift_across_table_end) {
    for (int victim = 0; victim < 4; ++victim) {
        FlatHashMap<std::uint64_t, int, IdentityHash> m(4);
        const std::array<std::uint64_t, 4> keys{6, 14, 22, 30};
        for (std::size_t i = 0; i < keys.size(); ++i) OT_CHECK(m.insert(keys[i], static_cast<int>(i)).second);
        OT_CHECK(m.erase(keys[static_cast<std::size_t>(victim)]));
        OT_CHECK(m.find(keys[static_cast<std::size_t>(victim)]) == nullptr);
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (static_cast<int>(i) == victim) continue;
            const int* v = m.find(keys[i]);
            OT_CHECK(v != nullptr);
            if (v != nullptr) OT_CHECK_EQ(*v, static_cast<int>(i));
        }
        OT_CHECK_EQ(m.size(), std::size_t{3});
    }
}

// Entries sitting at their own home slot must stay put when an earlier hole opens,
// even if the run they belong to wraps: keys 6 and 7 are homed at the last two
// slots, 14 and 15 are pushed past the end into slots 0 and 1.
OT_TEST(backward_shift_leaves_entries_at_home) {
    FlatHashMap<std::uint64_t, int, IdentityHash> m(4);
    OT_CHECK(m.insert(6, 60).second);
    OT_CHECK(m.insert(7, 70).second);
    OT_CHECK(m.insert(14, 140).second);  // home 6 -> slot 0
    OT_CHECK(m.insert(15, 150).second);  // home 7 -> slot 1
    OT_CHECK(m.erase(6));
    OT_CHECK(m.find(6) == nullptr);
    OT_CHECK_EQ(*m.find(7), 70);
    OT_CHECK_EQ(*m.find(14), 140);
    OT_CHECK_EQ(*m.find(15), 150);
    OT_CHECK(m.erase(7));
    OT_CHECK_EQ(*m.find(14), 140);
    OT_CHECK_EQ(*m.find(15), 150);
    OT_CHECK(m.erase(14));
    OT_CHECK_EQ(*m.find(15), 150);
    OT_CHECK(m.erase(15));
    OT_CHECK(m.empty());
}

// A run whose members have different homes: erasing in the middle must move only
// the entries that may legally move.
OT_TEST(backward_shift_mixed_homes) {
    FlatHashMap<std::uint64_t, int, IdentityHash> m(4);
    OT_CHECK(m.insert(7, 1).second);   // slot 7
    OT_CHECK(m.insert(8, 2).second);   // home 0 -> slot 0
    OT_CHECK(m.insert(15, 3).second);  // home 7 -> slot 1
    OT_CHECK(m.insert(16, 4).second);  // home 0 -> slot 2
    OT_CHECK(m.erase(7));
    OT_CHECK(m.find(7) == nullptr);
    OT_CHECK_EQ(*m.find(8), 2);
    OT_CHECK_EQ(*m.find(15), 3);
    OT_CHECK_EQ(*m.find(16), 4);
}

// Every 4-key subset of 0..15 on an 8-slot identity-hashed table (each home slot is
// shared by two candidate keys, so all collision/wrap patterns occur), in every
// insertion order, erased in every order, with a full lookup sweep after each erase.
OT_TEST(exhaustive_small_table_all_orders) {
    constexpr std::uint64_t kDomain = 16;
    std::size_t scenarios = 0;
    bool failed = false;
    for (std::uint32_t mask = 0; mask < (1u << kDomain) && !failed; ++mask) {
        if (std::popcount(mask) != 4) continue;
        std::array<std::uint64_t, 4> keys{};
        std::size_t n = 0;
        for (std::uint64_t k = 0; k < kDomain; ++k)
            if (mask & (1u << k)) keys[n++] = k;

        std::array<int, 4> ins{0, 1, 2, 3};
        do {
            std::array<int, 4> era{0, 1, 2, 3};
            do {
                FlatHashMap<std::uint64_t, std::uint64_t, IdentityHash> m(4);
                std::array<bool, kDomain> present{};
                for (int i : ins) {
                    const std::uint64_t k = keys[static_cast<std::size_t>(i)];
                    if (!m.insert(k, k + 100).second) failed = true;
                    present[k] = true;
                }
                for (int e : era) {
                    const std::uint64_t victim = keys[static_cast<std::size_t>(e)];
                    if (!m.erase(victim)) failed = true;
                    present[victim] = false;
                    for (std::uint64_t k = 0; k < kDomain; ++k) {
                        const std::uint64_t* v = m.find(k);
                        if (present[k] ? (v == nullptr || *v != k + 100) : (v != nullptr)) failed = true;
                    }
                }
                if (!m.empty()) failed = true;
                ++scenarios;
            } while (!failed && std::next_permutation(era.begin(), era.end()));
        } while (!failed && std::next_permutation(ins.begin(), ins.end()));
    }
    OT_CHECK(!failed);
    OT_CHECK_EQ(scenarios, std::size_t{1820 * 24 * 24});
}

// ---------------------------------------------------------------------------
// clear() and for_each()
// ---------------------------------------------------------------------------

OT_TEST(clear_resets_everything) {
    FlatHashMap<std::uint64_t, int> m(50);
    for (int i = 0; i < 50; ++i) OT_CHECK(m.insert(static_cast<std::uint64_t>(i), i).second);
    m.clear();
    OT_CHECK(m.empty());
    OT_CHECK_EQ(m.size(), std::size_t{0});
    OT_CHECK_EQ(m.max_size(), std::size_t{50});
    for (int i = 0; i < 50; ++i) OT_CHECK(m.find(static_cast<std::uint64_t>(i)) == nullptr);
    std::size_t visits = 0;
    m.for_each([&](const std::uint64_t&, const int&) { ++visits; });
    OT_CHECK_EQ(visits, std::size_t{0});
    // The full capacity is available again and values are fresh.
    for (int i = 0; i < 50; ++i) OT_CHECK(m.insert(static_cast<std::uint64_t>(i) + 1000, -i).second);
    OT_CHECK(m.insert(7777, 0).first == nullptr);
    OT_CHECK_EQ(*m.find(1003), -3);
    m.clear();
    m.clear();  // idempotent
    OT_CHECK(m.empty());
}

OT_TEST(for_each_visits_exactly_the_live_entries) {
    FlatHashMap<std::uint64_t, std::uint64_t> m(600);
    std::map<std::uint64_t, std::uint64_t> ref;
    Rng rng(99);
    for (int i = 0; i < 600; ++i) {
        const std::uint64_t k = rng.bounded(1000);
        const std::uint64_t v = rng.next();
        if (m.insert(k, v).second) ref.emplace(k, v);
    }
    for (int i = 0; i < 400; ++i) {
        const std::uint64_t k = rng.bounded(1000);
        OT_CHECK_EQ(m.erase(k), ref.erase(k) == 1);
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> seen;
    const auto& cm = m;
    cm.for_each([&](const std::uint64_t& k, const std::uint64_t& v) { seen.emplace_back(k, v); });
    std::sort(seen.begin(), seen.end());
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> expected(ref.begin(), ref.end());
    OT_CHECK_EQ(seen.size(), m.size());
    OT_CHECK(seen == expected);
}

// ---------------------------------------------------------------------------
// Symbol keys
// ---------------------------------------------------------------------------

OT_TEST(symbol_keys_basic) {
    FlatHashMap<Symbol, int> m(16);
    OT_CHECK(m.insert(Symbol("AAPL"), 1).second);
    OT_CHECK(m.insert(Symbol("MSFT"), 2).second);
    OT_CHECK(m.insert(Symbol("BRK.B"), 3).second);
    OT_CHECK(m.insert(Symbol("ABCDEFGH"), 4).second);
    // Padding is part of the value: the short and the padded spelling are one key.
    OT_CHECK(!m.insert(Symbol("AAPL    "), 9).second);
    OT_CHECK_EQ(*m.find(Symbol("AAPL")), 1);
    OT_CHECK_EQ(*m.find(Symbol("ABCDEFGH")), 4);
    OT_CHECK_EQ(*m.find(Symbol("ABCDEFGHIJ")), 4);  // constructor truncates to eight
    OT_CHECK(m.find(Symbol("AAP")) == nullptr);
    OT_CHECK(m.find(Symbol("aapl")) == nullptr);
    OT_CHECK(m.find(Symbol("AAPLX")) == nullptr);
    OT_CHECK(m.find(Symbol()) == nullptr);

    const char wire[8] = {'M', 'S', 'F', 'T', ' ', ' ', ' ', ' '};
    OT_CHECK_EQ(*m.find(Symbol::from_wire(wire)), 2);

    OT_CHECK(m.erase(Symbol("MSFT")));
    OT_CHECK(m.find(Symbol("MSFT")) == nullptr);
    OT_CHECK_EQ(*m.find(Symbol("BRK.B")), 3);
}

// The default (empty) symbol equals the padding of unused slots; it must still work
// as a real key.
OT_TEST(empty_symbol_is_an_ordinary_key) {
    FlatHashMap<Symbol, int> m(4);
    OT_CHECK(m.find(Symbol()) == nullptr);
    OT_CHECK(m.insert(Symbol(""), 5).second);
    OT_CHECK_EQ(*m.find(Symbol()), 5);
    OT_CHECK(!m.insert(Symbol("        "), 6).second);
    OT_CHECK(m.erase(Symbol()));
    OT_CHECK(m.find(Symbol()) == nullptr);
}

// ---------------------------------------------------------------------------
// Hash functions
// ---------------------------------------------------------------------------

// Known answers produced by an independent splitmix64 implementation.
OT_TEST(mix64_known_answers) {
    OT_CHECK_EQ(mix64(0), std::uint64_t{0xE220A8397B1DCDAFULL});
    OT_CHECK_EQ(mix64(1), std::uint64_t{0x910A2DEC89025CC1ULL});
    OT_CHECK_EQ(mix64(2), std::uint64_t{0x975835DE1C9756CEULL});
    OT_CHECK_EQ(mix64(0xFFFFFFFFFFFFFFFFULL), std::uint64_t{0xE4D971771B652C20ULL});
    OT_CHECK_EQ(mix64(0x123456789ABCDEF0ULL), std::uint64_t{0x161922C645CE50E8ULL});
    static_assert(mix64(0) == 0xE220A8397B1DCDAFULL, "mix64 is usable in constant expressions");
}

OT_TEST(integer_hash_widens_before_mixing) {
    OT_CHECK_EQ(Hash<std::uint64_t>{}(0), mix64(0));
    OT_CHECK_EQ(Hash<std::uint8_t>{}(255), mix64(255));
    OT_CHECK_EQ(Hash<std::uint32_t>{}(0xFFFFFFFFu), mix64(0xFFFFFFFFULL));
    OT_CHECK_EQ(Hash<std::int32_t>{}(-1), mix64(0xFFFFFFFFFFFFFFFFULL));  // sign extended
}

OT_TEST(symbol_hash_is_consistent_with_equality) {
    const Hash<Symbol> h;
    OT_CHECK_EQ(h(Symbol("AAPL")), h(Symbol("AAPL    ")));
    OT_CHECK(h(Symbol("AAPL")) != h(Symbol("AAPM")));
    OT_CHECK(h(Symbol("AAPL")) != h(Symbol("aapl")));
    OT_CHECK(h(Symbol("AB")) != h(Symbol("BA")));
    std::uint64_t raw;
    std::memcpy(&raw, "AAPL    ", sizeof raw);
    OT_CHECK_EQ(h(Symbol("AAPL")), mix64(raw));
}

// Keys that differ only in high bits or that step by a large power of two are the
// classic way to defeat a hash that just masks; the mixer has to spread them.
OT_TEST(hash_spreads_structured_keys) {
    for (int shift : {0, 8, 20, 32, 48}) {
        std::vector<bool> used(16384, false);
        std::size_t distinct = 0;
        for (std::uint64_t i = 0; i < 8192; ++i) {
            const std::size_t slot = Hash<std::uint64_t>{}(i << shift) & 16383;
            if (!used[slot]) {
                used[slot] = true;
                ++distinct;
            }
        }
        // A perfectly random hash lands on ~6450 distinct slots; identity-style
        // hashing on these shapes would give 1 or 8192 in a pattern, never this.
        OT_CHECK(distinct > 6000 && distinct < 7000);
    }
}

OT_TEST_MAIN()

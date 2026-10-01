// FlatHashMap against std::unordered_map: randomized differential runs with hostile
// key shapes and hash functions (heavy erase phases, keys that collide in the low
// bits, probe runs that wrap around the end of the table). Fixed seeds throughout.

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <string>
#include <unordered_map>
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

// Only sixteen distinct hash values whatever the table size: long probe runs.
struct LowNibbleHash {
    std::uint64_t operator()(std::uint64_t k) const noexcept { return k & 0xF; }
};

// Multi-field value so that a shift which moved a key but not its whole value
// (or the other way round) cannot go unnoticed.
struct Payload {
    std::uint64_t a{};
    std::uint32_t b{};
    friend bool operator==(const Payload&, const Payload&) = default;
};

Payload make_payload(std::uint64_t key, std::uint64_t salt) {
    return {key * 31 + salt, static_cast<std::uint32_t>(salt * 7 + key)};
}

enum class KeyShape { sequential, clustered, low_bits_zero, scattered, table_end };

// Maps a dense index to a key. Each shape stresses a different weakness: dense
// runs, tight clusters far apart, zero low bits, random-looking 64-bit values, and
// (for the identity hasher on an 8-slot table) keys whose homes are the last two
// slots so that probe runs wrap around to slot 0.
std::uint64_t shape_key(KeyShape shape, std::uint64_t i) {
    switch (shape) {
        case KeyShape::sequential: return i;
        case KeyShape::clustered: return ((i >> 5) << 40) | (i & 31);
        case KeyShape::low_bits_zero: return i << 24;
        case KeyShape::scattered: return mix64(i);
        case KeyShape::table_end: return (i / 2) * 8 + 6 + (i % 2);
    }
    return i;
}

struct Phase {
    unsigned insert_pct;
    unsigned erase_pct;  // the remainder is lookups
};

template <class Map>
bool contents_match(const Map& map, const std::unordered_map<std::uint64_t, Payload>& ref) {
    if (map.size() != ref.size()) return false;
    std::vector<std::uint64_t> visited;
    bool values_ok = true;
    map.for_each([&](const std::uint64_t& k, const Payload& v) {
        visited.push_back(k);
        const auto it = ref.find(k);
        if (it == ref.end() || !(it->second == v)) values_ok = false;
    });
    if (!values_ok || visited.size() != ref.size()) return false;
    std::sort(visited.begin(), visited.end());
    if (std::adjacent_find(visited.begin(), visited.end()) != visited.end()) return false;
    for (const auto& [k, v] : ref) {
        const Payload* p = map.find(k);
        if (p == nullptr || !(*p == v)) return false;
    }
    return true;
}

#define DIFF_REQUIRE(cond, what)                                                      \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ::ot_test::fail(__FILE__, __LINE__,                                       \
                            std::string(label) + ": " + (what) + " at op " +          \
                                std::to_string(op));                                  \
            return;                                                                   \
        }                                                                             \
    } while (0)

// Applies `ops` random operations to the map and a std::unordered_map in lockstep
// and compares every return value. Phases alternate between filling to the limit,
// churning, and draining, so full-table rejection and long erase sequences both
// happen many times. Stops at the first divergence so a bug yields one report.
template <class H>
void run_differential(const char* label, std::uint64_t seed, std::size_t max_elements,
                      std::uint64_t domain, KeyShape shape, std::size_t ops) {
    static constexpr Phase kPhases[] = {{70, 10}, {35, 35}, {10, 70}, {48, 44}};
    // Long enough for a fill phase to actually reach the limit, whatever its size.
    const std::size_t phase_len = std::max<std::size_t>(1500, max_elements * 4);
    constexpr std::size_t kVerifyEvery = 4096;

    Rng rng(seed);
    FlatHashMap<std::uint64_t, Payload, H> map(max_elements);
    std::unordered_map<std::uint64_t, Payload> ref;
    std::size_t rejected_full = 0;

    for (std::size_t op = 0; op < ops; ++op) {
        const Phase& ph = kPhases[(op / phase_len) % std::size(kPhases)];
        const std::uint64_t key = shape_key(shape, rng.bounded(domain));
        const std::uint64_t dice = rng.bounded(100);

        if (dice < ph.insert_pct) {
            const Payload val = make_payload(key, rng.next());
            const auto res = map.insert(key, val);
            const auto it = ref.find(key);
            if (it != ref.end()) {
                DIFF_REQUIRE(res.first != nullptr && !res.second, "duplicate insert must report existing");
                DIFF_REQUIRE(*res.first == it->second, "duplicate insert must not overwrite");
            } else if (ref.size() >= max_elements) {
                DIFF_REQUIRE(res.first == nullptr && !res.second, "insert into full map must fail");
                ++rejected_full;
            } else {
                DIFF_REQUIRE(res.first != nullptr && res.second, "insert of new key must succeed");
                DIFF_REQUIRE(*res.first == val, "inserted value");
                ref.emplace(key, val);
            }
        } else if (dice < ph.insert_pct + ph.erase_pct) {
            const bool erased = map.erase(key);
            DIFF_REQUIRE(erased == (ref.erase(key) == 1), "erase result");
        } else {
            const Payload* p = map.find(key);
            const auto it = ref.find(key);
            DIFF_REQUIRE((p != nullptr) == (it != ref.end()), "find presence");
            if (p != nullptr) DIFF_REQUIRE(*p == it->second, "find value");
        }
        DIFF_REQUIRE(map.size() == ref.size(), "size");
        if (op % kVerifyEvery == kVerifyEvery - 1) DIFF_REQUIRE(contents_match(map, ref), "full content check");
    }
    const std::size_t op = ops;
    DIFF_REQUIRE(contents_match(map, ref), "final content check");
    // The run only proves something about the full-table path if it reached it.
    DIFF_REQUIRE(max_elements == 0 || rejected_full > 0, "scenario never filled the map");
}

}  // namespace

// ---------------------------------------------------------------------------
// Symbol keys
// ---------------------------------------------------------------------------

OT_TEST(symbol_keys_differential) {
    constexpr std::size_t kMax = 500;
    FlatHashMap<Symbol, int> m(kMax);
    std::unordered_map<std::string, int> ref;
    Rng rng(4242);
    auto name = [](std::uint64_t i) {
        // "S" + up to five digits + optional class suffix: at most 8 characters.
        std::string s = "S" + std::to_string(i % 100000);
        if (i % 7 == 0) s += "A";
        if (i % 11 == 0) s += "B";
        s.resize(std::min<std::size_t>(s.size(), 8));
        return s;
    };
    for (int op = 0; op < 100'000; ++op) {
        const std::string n = name(rng.bounded(700));
        const Symbol sym(n);
        const std::uint64_t dice = rng.bounded(100);
        if (dice < 45) {
            const int v = static_cast<int>(rng.bounded(1'000'000));
            const auto r = m.insert(sym, v);
            const auto it = ref.find(n);
            if (it != ref.end()) {
                OT_CHECK(r.first != nullptr && !r.second);
                if (r.first == nullptr || r.second || *r.first != it->second) return;
            } else if (ref.size() >= kMax) {
                OT_CHECK(r.first == nullptr && !r.second);
                if (r.first != nullptr) return;
            } else {
                OT_CHECK(r.first != nullptr && r.second);
                if (r.first == nullptr || !r.second) return;
                ref.emplace(n, v);
            }
        } else if (dice < 75) {
            const bool erased = m.erase(sym);
            const bool expected = ref.erase(n) == 1;
            OT_CHECK_EQ(erased, expected);
            if (erased != expected) return;
        } else {
            const int* p = m.find(sym);
            const auto it = ref.find(n);
            OT_CHECK_EQ(p != nullptr, it != ref.end());
            if ((p != nullptr) != (it != ref.end())) return;
            if (p != nullptr) OT_CHECK_EQ(*p, it->second);
        }
        if (m.size() != ref.size()) {
            OT_CHECK_EQ(m.size(), ref.size());
            return;
        }
    }
}


// ---------------------------------------------------------------------------
// Differential runs against std::unordered_map
// ---------------------------------------------------------------------------

OT_TEST(differential_sequential_keys) {
    run_differential<Hash<std::uint64_t>>("sequential", 1, 3000, 4500, KeyShape::sequential, 200'000);
}
OT_TEST(differential_clustered_keys) {
    run_differential<Hash<std::uint64_t>>("clustered", 2, 3000, 4500, KeyShape::clustered, 200'000);
}
OT_TEST(differential_low_bits_zero_keys) {
    run_differential<Hash<std::uint64_t>>("low_bits_zero", 3, 1000, 1500, KeyShape::low_bits_zero, 200'000);
}
OT_TEST(differential_scattered_keys) {
    run_differential<Hash<std::uint64_t>>("scattered", 4, 512, 800, KeyShape::scattered, 200'000);
}
// The identity hasher sends every low_bits_zero key to slot 0: one giant probe run.
OT_TEST(differential_single_probe_run) {
    run_differential<IdentityHash>("identity_low_bits_zero", 5, 48, 70, KeyShape::low_bits_zero, 200'000);
}
OT_TEST(differential_sixteen_hash_values) {
    run_differential<LowNibbleHash>("low_nibble", 6, 60, 90, KeyShape::sequential, 200'000);
}
// 8-slot table, keys homed on slots 6 and 7: nearly every run wraps around.
OT_TEST(differential_wrap_around_tiny_table) {
    run_differential<IdentityHash>("tiny_wrap", 7, 3, 10, KeyShape::table_end, 200'000);
    run_differential<IdentityHash>("tiny_wrap_wide", 8, 4, 16, KeyShape::table_end, 200'000);
}
OT_TEST(differential_identity_hash_dense_table) {
    run_differential<IdentityHash>("identity_seq", 9, 8, 40, KeyShape::sequential, 200'000);
    run_differential<IdentityHash>("identity_seq_big", 10, 200, 320, KeyShape::sequential, 200'000);
}

// Random 4..8 element scenarios on 8/16-slot tables with hostile hashers, checking
// after every single operation that the whole key domain is still consistent.
OT_TEST(randomized_small_table_full_sweep) {
    Rng rng(2024);
    for (int scenario = 0; scenario < 3000; ++scenario) {
        FlatHashMap<std::uint64_t, std::uint64_t, IdentityHash> m(1 + rng.bounded(8));
        std::array<bool, 32> present{};
        for (int op = 0; op < 60; ++op) {
            const std::uint64_t k = rng.bounded(32);
            if (rng.chance(1, 2)) {
                const auto r = m.insert(k, k * 3);
                const std::size_t live = static_cast<std::size_t>(std::count(present.begin(), present.end(), true));
                if (present[k]) {
                    OT_CHECK(r.first != nullptr && !r.second);
                } else if (live >= m.max_size()) {
                    OT_CHECK(r.first == nullptr);
                } else {
                    OT_CHECK(r.first != nullptr && r.second);
                    present[k] = true;
                }
            } else {
                OT_CHECK_EQ(m.erase(k), present[k]);
                present[k] = false;
            }
            for (std::uint64_t q = 0; q < 32; ++q) {
                const std::uint64_t* v = m.find(q);
                if (present[q] ? (v == nullptr || *v != q * 3) : (v != nullptr)) {
                    OT_CHECK(false);
                    return;
                }
            }
        }
    }
}

OT_TEST_MAIN()

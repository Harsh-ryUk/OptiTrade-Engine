// SlabPool: exhaustion, LIFO reuse, value-initialisation, hostile releases, and a
// randomized comparison against an explicit model of the free list.

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "check.hpp"
#include "optitrade/core/rng.hpp"
#include "optitrade/core/slab_pool.hpp"

using namespace optitrade;

namespace {

// No default member initialisers: only value-initialisation (T{}) zeroes it.
struct Record {
    std::uint64_t a;
    std::int32_t b;
    char c[10];
};

struct WithDefaults {
    int x{7};
    int y{};
};

struct Tagged {
    std::uint64_t tag;
};

}  // namespace

OT_TEST(handle_constants) {
    using Pool = SlabPool<int>;
    OT_CHECK_EQ(Pool::kNull, std::uint32_t{0xFFFFFFFFu});
    static_assert(sizeof(Pool::Handle) == 4, "handles are 32 bit");
    OT_CHECK_EQ(Pool::kMaxCapacity, std::size_t{0xFFFFFFFEu});
}

OT_TEST(size_and_capacity_track_allocations) {
    SlabPool<int> pool(8);
    OT_CHECK_EQ(pool.capacity(), std::size_t{8});
    OT_CHECK_EQ(pool.size(), std::size_t{0});
    const auto a = pool.allocate();
    const auto b = pool.allocate();
    OT_CHECK_EQ(pool.size(), std::size_t{2});
    pool.release(a);
    OT_CHECK_EQ(pool.size(), std::size_t{1});
    pool.release(b);
    OT_CHECK_EQ(pool.size(), std::size_t{0});
    OT_CHECK_EQ(pool.capacity(), std::size_t{8});
}

OT_TEST(allocate_to_exhaustion) {
    constexpr std::size_t kCap = 100;
    SlabPool<int> pool(kCap);
    for (std::size_t i = 0; i < kCap; ++i) {
        const auto h = pool.allocate();
        OT_CHECK_EQ(h, static_cast<SlabPool<int>::Handle>(i));  // fresh pool: ascending order
        OT_CHECK_EQ(pool.size(), i + 1);
    }
    OT_CHECK_EQ(pool.size(), kCap);
    for (int attempt = 0; attempt < 3; ++attempt) {
        OT_CHECK_EQ(pool.allocate(), SlabPool<int>::kNull);
        OT_CHECK_EQ(pool.size(), kCap);  // failed allocation changes nothing
    }
    // Releasing one makes exactly one allocation possible again.
    pool.release(37);
    OT_CHECK_EQ(pool.allocate(), SlabPool<int>::Handle{37});
    OT_CHECK_EQ(pool.allocate(), SlabPool<int>::kNull);
}

OT_TEST(single_slot_and_empty_pools) {
    SlabPool<int> one(1);
    OT_CHECK_EQ(one.allocate(), SlabPool<int>::Handle{0});
    OT_CHECK_EQ(one.allocate(), SlabPool<int>::kNull);
    one.release(0);
    OT_CHECK_EQ(one.allocate(), SlabPool<int>::Handle{0});

    SlabPool<int> none(0);
    OT_CHECK_EQ(none.capacity(), std::size_t{0});
    OT_CHECK_EQ(none.allocate(), SlabPool<int>::kNull);
    OT_CHECK_EQ(none.size(), std::size_t{0});
    none.release(0);  // nothing to release, must not touch memory
    none.release(SlabPool<int>::kNull);
    OT_CHECK(!none.live(0));
}

OT_TEST(released_handles_are_reused_last_in_first_out) {
    SlabPool<int> pool(5);
    for (int i = 0; i < 5; ++i) pool.allocate();
    pool.release(2);
    pool.release(4);
    pool.release(1);
    OT_CHECK_EQ(pool.allocate(), SlabPool<int>::Handle{1});
    OT_CHECK_EQ(pool.allocate(), SlabPool<int>::Handle{4});
    OT_CHECK_EQ(pool.allocate(), SlabPool<int>::Handle{2});
    OT_CHECK_EQ(pool.allocate(), SlabPool<int>::kNull);

    // Interleaved: a handle released after an allocation is the next one handed out.
    pool.release(3);
    OT_CHECK_EQ(pool.allocate(), SlabPool<int>::Handle{3});
    pool.release(0);
    pool.release(3);
    OT_CHECK_EQ(pool.allocate(), SlabPool<int>::Handle{3});
    OT_CHECK_EQ(pool.allocate(), SlabPool<int>::Handle{0});
}

OT_TEST(release_all_then_reallocate_in_reverse_order) {
    constexpr std::size_t kCap = 16;
    SlabPool<int> pool(kCap);
    std::vector<SlabPool<int>::Handle> hs;
    for (std::size_t i = 0; i < kCap; ++i) hs.push_back(pool.allocate());
    for (auto h : hs) pool.release(h);
    OT_CHECK_EQ(pool.size(), std::size_t{0});
    for (std::size_t i = 0; i < kCap; ++i) OT_CHECK_EQ(pool.allocate(), hs[kCap - 1 - i]);
    OT_CHECK_EQ(pool.allocate(), SlabPool<int>::kNull);
}

OT_TEST(objects_are_value_initialised_on_every_allocation) {
    SlabPool<Record> pool(2);
    const auto h = pool.allocate();
    OT_CHECK_EQ(pool[h].a, std::uint64_t{0});
    OT_CHECK_EQ(pool[h].b, 0);
    pool[h].a = 0xDEADBEEFDEADBEEFULL;
    pool[h].b = -1;
    for (char& ch : pool[h].c) ch = 'x';
    pool.release(h);
    const auto again = pool.allocate();
    OT_CHECK_EQ(again, h);
    OT_CHECK_EQ(pool[again].a, std::uint64_t{0});
    OT_CHECK_EQ(pool[again].b, 0);
    for (char ch : pool[again].c) OT_CHECK_EQ(ch, '\0');

    SlabPool<WithDefaults> defaults(2);
    const auto d = defaults.allocate();
    OT_CHECK_EQ(defaults[d].x, 7);
    defaults[d].x = 99;
    defaults[d].y = 5;
    defaults.release(d);
    const auto d2 = defaults.allocate();
    OT_CHECK_EQ(d2, d);
    OT_CHECK_EQ(defaults[d2].x, 7);  // reset to the member initialiser, not zeroed
    OT_CHECK_EQ(defaults[d2].y, 0);
}

OT_TEST(handles_address_distinct_stable_storage) {
    constexpr std::size_t kCap = 64;
    SlabPool<Tagged> pool(kCap);
    std::vector<SlabPool<Tagged>::Handle> hs;
    for (std::size_t i = 0; i < kCap; ++i) {
        hs.push_back(pool.allocate());
        pool[hs.back()].tag = 1000 + i;
    }
    const Tagged* first = &pool[hs[0]];
    for (std::size_t i = 0; i < kCap; ++i) OT_CHECK_EQ(pool[hs[i]].tag, 1000 + i);
    // Releasing and reallocating other nodes never moves or disturbs live ones.
    pool.release(hs[10]);
    pool.release(hs[20]);
    pool.allocate();
    pool.allocate();
    OT_CHECK(&pool[hs[0]] == first);
    for (std::size_t i = 0; i < kCap; ++i) {
        if (i == 10 || i == 20) continue;
        OT_CHECK_EQ(pool[hs[i]].tag, 1000 + i);
    }
    const auto& cpool = pool;
    OT_CHECK_EQ(cpool[hs[5]].tag, std::uint64_t{1005});
}

OT_TEST(live_reports_allocation_state) {
    SlabPool<int> pool(4);
    OT_CHECK(!pool.live(0));  // never allocated
    const auto h = pool.allocate();
    OT_CHECK(pool.live(h));
    OT_CHECK(!pool.live(h + 1));
    pool.release(h);
    OT_CHECK(!pool.live(h));
    OT_CHECK(!pool.live(SlabPool<int>::kNull));
    OT_CHECK(!pool.live(4));
    OT_CHECK(!pool.live(0xFFFFFFFEu));
}

// A repeated release would create a cycle in an unguarded free list and hand one
// node to two owners; a wild handle would write outside the array.
OT_TEST(invalid_releases_are_ignored) {
    SlabPool<Tagged> pool(4);
    const auto a = pool.allocate();
    const auto b = pool.allocate();
    pool[a].tag = 11;
    pool[b].tag = 22;

    pool.release(a);
    pool.release(a);  // double release
    pool.release(a);
    OT_CHECK_EQ(pool.size(), std::size_t{1});
    pool.release(SlabPool<Tagged>::kNull);
    pool.release(4);
    pool.release(1000);
    pool.release(0xFFFFFFFEu);
    pool.release(2);  // never allocated
    pool.release(3);
    OT_CHECK_EQ(pool.size(), std::size_t{1});
    OT_CHECK(pool.live(b));
    OT_CHECK_EQ(pool[b].tag, std::uint64_t{22});

    // The free list is intact: three distinct handles, then exhaustion.
    const auto x = pool.allocate();
    const auto y = pool.allocate();
    const auto z = pool.allocate();
    OT_CHECK(x != y && y != z && x != z);
    OT_CHECK(x != b && y != b && z != b);
    OT_CHECK(x < 4 && y < 4 && z < 4);
    OT_CHECK_EQ(pool.allocate(), SlabPool<Tagged>::kNull);
    OT_CHECK_EQ(pool.size(), std::size_t{4});
}

OT_TEST(oversized_capacity_throws_before_allocating) {
    if constexpr (sizeof(std::size_t) > 4) {
        bool threw = false;
        try {
            SlabPool<char> pool(std::size_t{0xFFFFFFFFu});
        } catch (const std::length_error&) {
            threw = true;
        }
        OT_CHECK(threw);
        threw = false;
        try {
            SlabPool<char> pool(std::size_t{1} << 40);
        } catch (const std::length_error&) {
            threw = true;
        }
        OT_CHECK(threw);
    }
}

namespace {

// Compares the pool with an explicit stack model of its documented behaviour
// (fresh pool: 0,1,2,...; then most recently released first), including wild
// releases, on every operation.
void run_model(std::size_t capacity, std::uint64_t seed, std::size_t ops) {
    using Pool = SlabPool<Tagged>;
    Pool pool(capacity);
    Rng rng(seed);
    std::vector<Pool::Handle> free_stack;
    for (std::size_t i = capacity; i-- > 0;) free_stack.push_back(static_cast<Pool::Handle>(i));
    std::vector<std::uint64_t> tag(capacity, 0);
    std::vector<bool> live(capacity, false);
    std::size_t live_count = 0;
    std::uint64_t next_tag = 1;

    auto fail = [&](const char* what, std::size_t op) {
        ::ot_test::fail(__FILE__, __LINE__,
                        "capacity " + std::to_string(capacity) + ": " + what + " at op " + std::to_string(op));
    };

    for (std::size_t op = 0; op < ops; ++op) {
        const std::uint64_t dice = rng.bounded(100);
        if (dice < 50) {
            const Pool::Handle expected = free_stack.empty() ? Pool::kNull : free_stack.back();
            const Pool::Handle h = pool.allocate();
            if (h != expected) return fail("allocate returned an unexpected handle", op);
            if (h != Pool::kNull) {
                free_stack.pop_back();
                if (pool[h].tag != 0) return fail("allocation not value-initialised", op);
                live[h] = true;
                ++live_count;
                tag[h] = next_tag++;
                pool[h].tag = tag[h];
            }
        } else if (dice < 85) {
            const auto h = static_cast<Pool::Handle>(rng.bounded(capacity));
            pool.release(h);  // often not live: must then be a no-op
            if (live[h]) {
                live[h] = false;
                --live_count;
                free_stack.push_back(h);
            }
        } else if (dice < 90) {
            pool.release(static_cast<Pool::Handle>(capacity + rng.bounded(4)));
            pool.release(Pool::kNull);
        } else {
            const auto h = static_cast<Pool::Handle>(rng.bounded(capacity));
            if (pool.live(h) != live[h]) return fail("live() disagrees with model", op);
            if (live[h] && pool[h].tag != tag[h]) return fail("stored value changed", op);
        }
        if (pool.size() != live_count) return fail("size disagrees with model", op);
    }
    for (std::size_t h = 0; h < capacity; ++h)
        if (live[h] && pool[static_cast<Pool::Handle>(h)].tag != tag[h])
            return fail("final content check", ops);
}

}  // namespace

OT_TEST(randomized_model_comparison) {
    run_model(1, 11, 50'000);
    run_model(2, 12, 50'000);
    run_model(7, 13, 100'000);
    run_model(64, 14, 200'000);
    run_model(1000, 15, 200'000);
}

OT_TEST_MAIN()

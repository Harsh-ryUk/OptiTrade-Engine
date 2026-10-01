// The order book looks for a price in the last few levels first (where most market data lands)
// and only then binary-searches the rest. This test drives books with hundreds of levels, so that
// operations land at every distance from the best price, and compares the ladder with a
// std::map reference after every operation.

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <vector>

#include "check.hpp"
#include "optitrade/book/order_book.hpp"
#include "optitrade/core/rng.hpp"

using namespace optitrade;
using book::OrderBook;

namespace {

struct Ref {
    struct L { Qty qty; std::uint32_t orders; };
    std::map<Price, L> m;  // ascending by price

    void add(Price p, Qty q) {
        auto it = m.find(p);
        if (it == m.end()) m[p] = L{q, 1};
        else { it->second.qty += q; ++it->second.orders; }
    }
    bool take(Price p, Qty q, bool whole) {
        auto it = m.find(p);
        if (q == 0 || it == m.end() || q > it->second.qty) return false;
        it->second.qty -= q;
        if (it->second.qty == 0) m.erase(it);
        else if (whole && it->second.orders > 1) --it->second.orders;
        return true;
    }
};

void expect_same(const OrderBook& b, Side side, const Ref& r) {
    OT_CHECK_EQ(b.depth(side), r.m.size());
    // Index 0 is the best level: highest price for bids, lowest for asks.
    std::size_t i = 0;
    if (side == Side::buy) {
        for (auto it = r.m.rbegin(); it != r.m.rend(); ++it, ++i) {
            const auto& lv = b.level(side, i);
            OT_CHECK_EQ(lv.price, it->first);
            OT_CHECK_EQ(lv.qty, it->second.qty);
            OT_CHECK_EQ(lv.orders, it->second.orders);
        }
    } else {
        for (auto it = r.m.begin(); it != r.m.end(); ++it, ++i) {
            const auto& lv = b.level(side, i);
            OT_CHECK_EQ(lv.price, it->first);
            OT_CHECK_EQ(lv.qty, it->second.qty);
            OT_CHECK_EQ(lv.orders, it->second.orders);
        }
    }
}

// Picks a price near the touch most of the time and anywhere in the range otherwise.
Price pick_price(Rng& rng, const Ref& r, Side side, Price lo, Price hi) {
    if (!r.m.empty() && rng.chance(6, 10)) {
        const std::uint64_t d = rng.geometric(1, 3, 40);  // distance in levels from the best
        auto steps = std::min<std::uint64_t>(d, r.m.size() - 1);
        if (side == Side::buy) { auto it = r.m.rbegin(); std::advance(it, steps); return it->first + rng.range(-1, 1); }
        auto it = r.m.begin(); std::advance(it, steps); return it->first + rng.range(-1, 1);
    }
    return rng.range(lo, hi);
}

void run(std::uint64_t seed, std::size_t capacity, Price lo, Price hi, int ops, int check_every,
         std::size_t min_depth) {
    Rng rng(seed);
    OrderBook book(capacity);
    Ref ref[2];
    std::size_t max_depth = 0;
    for (int n = 0; n < ops; ++n) {
        const Side side = rng.chance(1, 2) ? Side::buy : Side::sell;
        Ref& r = ref[index(side)];
        const Price p = std::clamp<Price>(pick_price(rng, r, side, lo, hi), lo, hi);  // stay inside capacity: no eviction here
        const std::uint64_t what = rng.bounded(10);
        if (what < 5) {  // add
            const Qty q = static_cast<Qty>(rng.range(1, 500));
            OT_CHECK(book.add(side, p, q));
            r.add(p, q);
        } else if (what < 8) {  // remove a whole order (valid and invalid quantities)
            const Qty q = static_cast<Qty>(rng.range(1, 600));
            OT_CHECK_EQ(book.remove(side, p, q), r.take(p, q, true));
        } else {  // partial reduction
            const Qty q = static_cast<Qty>(rng.range(1, 300));
            OT_CHECK_EQ(book.reduce(side, p, q), r.take(p, q, false));
        }
        max_depth = std::max(max_depth, r.m.size());
        // Point queries every operation, the full ladder periodically.
        const auto it = r.m.find(p);
        OT_CHECK_EQ(book.qty_at(side, p), it == r.m.end() ? Qty{0} : it->second.qty);
        if (n % check_every == 0) { expect_same(book, Side::buy, ref[0]); expect_same(book, Side::sell, ref[1]); }
    }
    expect_same(book, Side::buy, ref[0]);
    expect_same(book, Side::sell, ref[1]);
    OT_CHECK(max_depth >= min_depth);  // the scenario really did build books of the intended depth
}

}  // namespace

OT_TEST(ladders_with_hundreds_of_levels_match_a_map_reference) {
    for (std::uint64_t seed = 1; seed <= 4; ++seed) run(seed, 600, 1, 400, 120'000, 7, 100);
}

OT_TEST(ladders_around_the_window_boundary_match_a_map_reference) {
    // Depths hovering around 8 (the scan window) and 9..20.
    for (std::uint64_t seed = 10; seed <= 13; ++seed) run(seed, 64, 1, 14, 80'000, 1, 9);
    for (std::uint64_t seed = 20; seed <= 23; ++seed) run(seed, 64, 1, 24, 80'000, 1, 16);
}

OT_TEST(book_ops_at_the_worst_end_of_a_deep_ladder) {
    OrderBook b(64);
    for (Price p = 1; p <= 40; ++p) OT_CHECK(b.add(Side::buy, p * 10, 5));  // best bid is 400
    OT_CHECK_EQ(b.depth(Side::buy), std::size_t{40});
    OT_CHECK(b.add(Side::buy, 5, 7));      // worse than everything
    OT_CHECK(b.add(Side::buy, 405, 7));    // better than everything
    OT_CHECK(b.add(Side::buy, 195, 7));    // in the middle
    OT_CHECK_EQ(b.qty_at(Side::buy, 5), Qty{7});
    OT_CHECK_EQ(b.qty_at(Side::buy, 405), Qty{7});
    OT_CHECK_EQ(b.level(Side::buy, 0).price, Price{405});
    OT_CHECK(b.remove(Side::buy, 5, 7));
    OT_CHECK_EQ(b.qty_at(Side::buy, 5), Qty{0});
    OT_CHECK_EQ(b.depth(Side::buy), std::size_t{42});
}

OT_TEST_MAIN()

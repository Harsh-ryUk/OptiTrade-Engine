// OrderBook: hand-computed ladders, the overflow policy, level-id staleness, and a
// seeded property test against a std::map model of the same documented rules.

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "check.hpp"
#include "optitrade/book/order_book.hpp"
#include "optitrade/core/rng.hpp"

using namespace optitrade;
using book::Level;
using book::OrderBook;

namespace {

constexpr Side B = Side::buy;
constexpr Side S = Side::sell;

std::vector<Price> prices(const OrderBook& b, Side s) {
    std::vector<Price> out;
    for (std::size_t i = 0; i < b.depth(s); ++i) out.push_back(b.level(s, i).price);
    return out;
}

using Prices = std::vector<Price>;

}  // namespace

OT_TEST(empty_book) {
    OrderBook b(8);
    OT_CHECK_EQ(b.depth(B), std::size_t{0});
    OT_CHECK_EQ(b.depth(S), std::size_t{0});
    OT_CHECK(!b.best(B).has_value());
    OT_CHECK(!b.best(S).has_value());
    OT_CHECK_EQ(b.qty_at(B, 100), Qty{0});
    OT_CHECK_EQ(b.total_qty(S, 5), Qty{0});
    OT_CHECK(!b.crossed());
    OT_CHECK_EQ(b.updates(), std::uint64_t{0});
    OT_CHECK_EQ(b.max_levels_per_side(), std::size_t{8});
    // Out-of-range access is defined: an all-zero level, never a stray read.
    OT_CHECK(b.level(B, 0) == Level{});
    OT_CHECK(b.level(S, 1000) == Level{});
}

OT_TEST(bids_are_ordered_best_first) {
    OrderBook b(8);
    OT_CHECK(b.add(B, 100, 10));
    OT_CHECK(b.add(B, 102, 5));
    OT_CHECK(b.add(B, 101, 7));
    OT_CHECK(b.add(B, 99, 1));
    OT_CHECK(prices(b, B) == (Prices{102, 101, 100, 99}));
    OT_CHECK(b.level(B, 0) == (Level{102, 5, 1}));
    OT_CHECK(b.level(B, 3) == (Level{99, 1, 1}));
    OT_CHECK(b.best(B).value() == (Level{102, 5, 1}));
    OT_CHECK_EQ(b.depth(S), std::size_t{0});
}

OT_TEST(asks_are_ordered_best_first) {
    OrderBook b(8);
    OT_CHECK(b.add(S, 105, 3));
    OT_CHECK(b.add(S, 103, 4));
    OT_CHECK(b.add(S, 104, 9));
    OT_CHECK(b.add(S, 106, 2));
    OT_CHECK(prices(b, S) == (Prices{103, 104, 105, 106}));
    OT_CHECK(b.best(S).value() == (Level{103, 4, 1}));
}

OT_TEST(same_price_aggregates_quantity_and_order_count) {
    OrderBook b(4);
    OT_CHECK(b.add(B, 100, 10));
    OT_CHECK(b.add(B, 100, 15));
    OT_CHECK(b.add(B, 100, 1));
    OT_CHECK_EQ(b.depth(B), std::size_t{1});
    OT_CHECK(b.level(B, 0) == (Level{100, 26, 3}));
    OT_CHECK_EQ(b.qty_at(B, 100), Qty{26});
    OT_CHECK_EQ(b.qty_at(B, 101), Qty{0});
    OT_CHECK_EQ(b.qty_at(S, 100), Qty{0});  // sides are independent
}

OT_TEST(remove_takes_a_whole_order_and_reduce_keeps_it) {
    OrderBook b(4);
    b.add(B, 100, 10);
    b.add(B, 100, 15);  // {100, 25, 2}
    OT_CHECK(b.reduce(B, 100, 5));
    OT_CHECK(b.level(B, 0) == (Level{100, 20, 2}));  // order count untouched by a partial
    OT_CHECK(b.remove(B, 100, 15));
    OT_CHECK(b.level(B, 0) == (Level{100, 5, 1}));
    OT_CHECK(b.remove(B, 100, 5));                    // last shares: the level disappears
    OT_CHECK_EQ(b.depth(B), std::size_t{0});
    OT_CHECK(!b.best(B).has_value());
}

OT_TEST(removals_that_cannot_be_applied_leave_the_book_untouched) {
    OrderBook b(4);
    b.add(S, 200, 10);
    const std::uint64_t before = b.updates();
    OT_CHECK(!b.remove(S, 201, 1));   // no such level
    OT_CHECK(!b.remove(B, 200, 1));   // wrong side
    OT_CHECK(!b.remove(S, 200, 11));  // more than the level holds
    OT_CHECK(!b.reduce(S, 200, 11));
    OT_CHECK(!b.remove(S, 200, 0));   // zero is not a removal
    OT_CHECK(!b.reduce(S, 200, 0));
    OT_CHECK(b.level(S, 0) == (Level{200, 10, 1}));
    OT_CHECK_EQ(b.updates(), before);
}

OT_TEST(reduce_that_empties_a_level_drops_it) {
    // Not reachable through MarketBooks, but the "no level with qty 0" invariant
    // must hold whatever the caller does.
    OrderBook b(4);
    b.add(B, 100, 10);
    b.add(B, 100, 5);
    OT_CHECK(b.reduce(B, 100, 15));
    OT_CHECK_EQ(b.depth(B), std::size_t{0});
}

OT_TEST(remove_of_a_partial_amount_never_leaves_zero_orders) {
    OrderBook b(4);
    b.add(B, 100, 10);
    OT_CHECK(b.remove(B, 100, 4));  // misuse: the order is not gone, but the level is
    OT_CHECK(b.level(B, 0) == (Level{100, 6, 1}));
}

OT_TEST(zero_quantity_add_is_refused) {
    OrderBook b(4);
    OT_CHECK(!b.add(B, 100, 0));
    OT_CHECK_EQ(b.depth(B), std::size_t{0});
    OT_CHECK_EQ(b.updates(), std::uint64_t{0});
    OT_CHECK_EQ(b.add_order(B, 100, 0), OrderBook::kNoLevel);
}

OT_TEST(total_qty_sums_the_best_levels_and_saturates) {
    OrderBook b(8);
    b.add(B, 99, 1);
    b.add(B, 100, 10);
    b.add(B, 101, 7);
    b.add(B, 102, 5);  // best first: 5, 7, 10, 1
    OT_CHECK_EQ(b.total_qty(B, 0), Qty{0});
    OT_CHECK_EQ(b.total_qty(B, 1), Qty{5});
    OT_CHECK_EQ(b.total_qty(B, 2), Qty{12});
    OT_CHECK_EQ(b.total_qty(B, 3), Qty{22});
    OT_CHECK_EQ(b.total_qty(B, 4), Qty{23});
    OT_CHECK_EQ(b.total_qty(B, 1000), Qty{23});
    OT_CHECK_EQ(b.total_qty(S, 4), Qty{0});

    OrderBook big(4);
    big.add(S, 1, 4'000'000'000u);
    big.add(S, 2, 4'000'000'000u);
    big.add(S, 3, 1);
    OT_CHECK_EQ(big.total_qty(S, 1), Qty{4'000'000'000u});
    OT_CHECK_EQ(big.total_qty(S, 3), std::numeric_limits<Qty>::max());  // 8e9 does not fit: saturate
}

OT_TEST(crossed_means_best_bid_at_or_above_best_ask) {
    OrderBook b(4);
    b.add(B, 100, 1);
    OT_CHECK(!b.crossed());  // one-sided
    b.add(S, 101, 1);
    OT_CHECK(!b.crossed());
    b.add(S, 100, 1);        // locked
    OT_CHECK(b.crossed());
    b.remove(S, 100, 1);
    OT_CHECK(!b.crossed());
    b.add(B, 102, 1);        // bid through the ask
    OT_CHECK(b.crossed());
    b.remove(B, 102, 1);
    OT_CHECK(!b.crossed());
}

OT_TEST(updates_counts_successful_mutations_only) {
    OrderBook b(2);
    b.add(B, 10, 1);          // 1: new level
    b.add(B, 10, 1);          // 2: joins
    b.add(B, 20, 1);          // 3
    OT_CHECK(!b.add(B, 5, 1));  // full and worse: not counted
    b.reduce(B, 10, 1);       // 4
    b.remove(B, 10, 1);       // 5
    b.remove(B, 99, 1);       // fails
    OT_CHECK_EQ(b.updates(), std::uint64_t{5});
}

OT_TEST(bid_overflow_keeps_the_best_prices) {
    OrderBook b(3);
    OT_CHECK(b.add(B, 20, 1));
    OT_CHECK(b.add(B, 30, 2));
    OT_CHECK(b.add(B, 40, 3));  // full: 40, 30, 20
    OT_CHECK(!b.add(B, 10, 9));                       // worse than the worst: refused
    OT_CHECK(prices(b, B) == (Prices{40, 30, 20}));
    OT_CHECK_EQ(b.overflows(), std::uint64_t{1});

    OT_CHECK(b.add(B, 50, 4));                        // better than the best: evicts 20
    OT_CHECK(prices(b, B) == (Prices{50, 40, 30}));
    OT_CHECK_EQ(b.qty_at(B, 20), Qty{0});
    OT_CHECK(b.add(B, 35, 5));                        // inside the ladder: evicts 30
    OT_CHECK(prices(b, B) == (Prices{50, 40, 35}));
    OT_CHECK(!b.add(B, 30, 1));                       // the evicted price is now worse than the worst
    OT_CHECK(b.add(B, 40, 6));                        // an existing level never overflows
    OT_CHECK(b.level(B, 1) == (Level{40, 9, 2}));
    OT_CHECK_EQ(b.overflows(), std::uint64_t{4});     // refused 10, evicted 20, evicted 30, refused 30
    OT_CHECK_EQ(b.depth(B), std::size_t{3});
}

OT_TEST(ask_overflow_keeps_the_best_prices) {
    OrderBook b(3);
    b.add(S, 40, 1);
    b.add(S, 30, 2);
    b.add(S, 20, 3);  // best first: 20, 30, 40
    OT_CHECK(!b.add(S, 50, 1));                      // worse than the worst (highest)
    OT_CHECK(prices(b, S) == (Prices{20, 30, 40}));
    OT_CHECK(b.add(S, 10, 4));                       // evicts 40
    OT_CHECK(prices(b, S) == (Prices{10, 20, 30}));
    OT_CHECK(b.add(S, 25, 5));                       // evicts 30
    OT_CHECK(prices(b, S) == (Prices{10, 20, 25}));
    OT_CHECK(!b.add(S, 30, 1));
    OT_CHECK_EQ(b.total_qty(S, 3), Qty{4 + 3 + 5});
}

OT_TEST(eviction_when_capacity_is_one_and_zero) {
    OrderBook one(1);
    OT_CHECK(one.add(B, 10, 1));
    OT_CHECK(!one.add(B, 9, 1));
    OT_CHECK(one.add(B, 11, 2));  // replaces the only level
    OT_CHECK(one.level(B, 0) == (Level{11, 2, 1}));
    OT_CHECK(one.add(S, 50, 1));  // the other side has its own storage
    OT_CHECK_EQ(one.depth(S), std::size_t{1});

    OrderBook none(0);
    OT_CHECK(!none.add(B, 10, 1));
    OT_CHECK_EQ(none.depth(B), std::size_t{0});
    OT_CHECK(!none.remove(B, 10, 1));
    OT_CHECK(!none.best(B).has_value());
    OT_CHECK(!none.crossed());
    OT_CHECK_EQ(none.overflows(), std::uint64_t{1});
}

OT_TEST(a_stale_level_id_cannot_touch_a_recreated_level) {
    OrderBook b(2);
    const auto id20 = b.add_order(B, 20, 5);
    const auto id30 = b.add_order(B, 30, 5);
    OT_CHECK(id20 != OrderBook::kNoLevel && id30 != OrderBook::kNoLevel && id20 != id30);
    OT_CHECK_EQ(b.add_order(B, 30, 1), id30);  // joining a level returns that level's id

    const auto id40 = b.add_order(B, 40, 7);   // evicts the level at 20
    OT_CHECK(id40 != OrderBook::kNoLevel);
    OT_CHECK(!b.remove_order(B, 20, 5, id20));  // gone
    OT_CHECK(prices(b, B) == (Prices{40, 30}));

    OT_CHECK(b.remove_order(B, 30, 5, id30));   // make room, keeping 30 alive with the other order
    OT_CHECK(b.remove_order(B, 30, 1, id30));
    OT_CHECK(prices(b, B) == (Prices{40}));
    const auto id20b = b.add_order(B, 20, 3);   // price 20 is back, as a new incarnation
    OT_CHECK(id20b != OrderBook::kNoLevel && id20b != id20);

    OT_CHECK(!b.remove_order(B, 20, 3, id20));  // the ghost from the first incarnation
    OT_CHECK(!b.reduce_order(B, 20, 1, id20));
    OT_CHECK(b.level(B, 1) == (Level{20, 3, 1}));  // untouched

    OT_CHECK(b.reduce_order(B, 20, 1, id20b));
    OT_CHECK(b.level(B, 1) == (Level{20, 2, 1}));
    OT_CHECK(b.remove_order(B, 20, 2, id20b));
    OT_CHECK(!b.remove_order(B, 40, 7, OrderBook::kNoLevel));  // kNoLevel is not a wildcard for the _order forms
    OT_CHECK(b.remove(B, 40, 7));                              // ... but it is for the plain remove()
    OT_CHECK_EQ(b.depth(B), std::size_t{0});
}

OT_TEST(a_level_refuses_an_order_that_would_overflow_its_quantity) {
    OrderBook b(4);
    OT_CHECK(b.add(B, 100, 0xFFFFFFF0u));
    OT_CHECK(!b.add(B, 100, 0x20));  // 2^32 + 16: does not fit in Qty
    OT_CHECK_EQ(b.qty_at(B, 100), Qty{0xFFFFFFF0u});
    OT_CHECK_EQ(b.level(B, 0).orders, 1u);
    OT_CHECK_EQ(b.overflows(), std::uint64_t{1});
    OT_CHECK(b.add(B, 100, 0x0F));   // exactly to the limit is fine
    OT_CHECK_EQ(b.qty_at(B, 100), std::numeric_limits<Qty>::max());
    OT_CHECK(!b.add(B, 100, 1));
    OT_CHECK(b.remove(B, 100, 0x0F));
    OT_CHECK_EQ(b.qty_at(B, 100), Qty{0xFFFFFFF0u});
}

OT_TEST(extreme_prices_order_correctly) {
    constexpr Price lo = std::numeric_limits<Price>::min();
    constexpr Price hi = std::numeric_limits<Price>::max();
    OrderBook b(8);
    for (const Price p : {Price{0}, hi, lo, Price{-1}}) {
        OT_CHECK(b.add(B, p, 1));
        OT_CHECK(b.add(S, p, 1));
    }
    OT_CHECK(prices(b, B) == (Prices{hi, 0, -1, lo}));
    OT_CHECK(prices(b, S) == (Prices{lo, -1, 0, hi}));
    OT_CHECK_EQ(b.qty_at(B, lo), Qty{1});
    OT_CHECK_EQ(b.qty_at(S, hi), Qty{1});
    OT_CHECK(b.crossed());
    OT_CHECK(b.remove(B, lo, 1));
    OT_CHECK(b.remove(S, hi, 1));
    OT_CHECK_EQ(b.depth(B), std::size_t{3});
}

// ---------------------------------------------------------------------------
// Property test. The model is std::map based and implements the documented rules
// (eviction, refusal, saturation, order counts) with no shared code.
// ---------------------------------------------------------------------------
namespace {

struct ModelLevel {
    std::uint64_t qty{};
    std::uint32_t orders{};
    std::uint64_t gen{};  // model's own incarnation counter
};

class ModelSide {
public:
    ModelSide(bool bids, std::size_t cap) : bids_(bids), cap_(cap) {}

    // Returns the incarnation the order landed on, 0 if not stored.
    std::uint64_t add(Price p, Qty q, std::uint64_t& next_gen) {
        if (q == 0) return 0;
        auto it = levels_.find(p);
        if (it != levels_.end()) {
            if (it->second.qty + q > std::numeric_limits<Qty>::max()) return 0;
            it->second.qty += q;
            ++it->second.orders;
            return it->second.gen;
        }
        if (levels_.size() >= cap_) {
            if (cap_ == 0) return 0;
            const Price worst = bids_ ? levels_.begin()->first : std::prev(levels_.end())->first;
            const bool new_is_worse = bids_ ? p < worst : p > worst;
            if (new_is_worse) return 0;
            levels_.erase(worst);
        }
        levels_[p] = ModelLevel{q, 1, next_gen};
        return next_gen++;
    }

    // gen == 0: any incarnation.
    bool take(Price p, Qty q, bool whole, std::uint64_t gen) {
        if (q == 0) return false;
        auto it = levels_.find(p);
        if (it == levels_.end()) return false;
        if (gen != 0 && it->second.gen != gen) return false;
        if (q > it->second.qty) return false;
        it->second.qty -= q;
        if (it->second.qty == 0) {
            levels_.erase(it);
        } else if (whole && it->second.orders > 1) {
            --it->second.orders;
        }
        return true;
    }

    // Best first.
    std::vector<std::pair<Price, ModelLevel>> ladder() const {
        std::vector<std::pair<Price, ModelLevel>> v(levels_.begin(), levels_.end());
        if (bids_) std::reverse(v.begin(), v.end());
        return v;
    }

private:
    bool bids_;
    std::size_t cap_;
    std::map<Price, ModelLevel> levels_;  // ascending price
};

struct Resting {
    Side side;
    Price price;
    Qty qty;
    OrderBook::LevelId id;   // implementation's incarnation
    std::uint64_t gen;       // model's incarnation
};

void compare(const OrderBook& b, const ModelSide& bids, const ModelSide& asks, int seed, int step) {
    const ModelSide* sides[2] = {&bids, &asks};
    const Side kinds[2] = {B, S};
    for (int k = 0; k < 2; ++k) {
        const auto want = sides[k]->ladder();
        if (b.depth(kinds[k]) != want.size()) {
            ot_test::fail(__FILE__, __LINE__, "depth mismatch seed " + std::to_string(seed) + " step " + std::to_string(step));
            return;
        }
        for (std::size_t i = 0; i < want.size(); ++i) {
            const Level got = b.level(kinds[k], i);
            if (got.price != want[i].first || got.qty != want[i].second.qty || got.orders != want[i].second.orders) {
                ot_test::fail(__FILE__, __LINE__, "level mismatch seed " + std::to_string(seed) + " step " + std::to_string(step));
                return;
            }
        }
    }
}

void property_run(int seed, std::size_t cap, Price range, int steps) {
    Rng rng(static_cast<std::uint64_t>(seed));
    OrderBook b(cap);
    ModelSide bids(true, cap), asks(false, cap);
    std::uint64_t next_gen[2] = {1, 1};
    std::vector<Resting> resting;

    for (int step = 0; step < steps; ++step) {
        const bool buy = rng.chance(1, 2);
        const Side side = buy ? B : S;
        ModelSide& ms = buy ? bids : asks;
        std::uint64_t& gens = next_gen[buy ? 0 : 1];
        const auto price = static_cast<Price>(rng.bounded(static_cast<std::uint64_t>(range))) - range / 2;

        switch (rng.bounded(6)) {
            case 0:
            case 1: {  // id-tracked add
                Qty q = static_cast<Qty>(rng.bounded(50));  // includes 0
                if (rng.chance(1, 300)) q = 0xF0000000u + static_cast<Qty>(rng.bounded(0x0FFFFFFF));
                const auto id = b.add_order(side, price, q);
                const auto gen = ms.add(price, q, gens);
                OT_CHECK_EQ(id != OrderBook::kNoLevel, gen != 0);
                if (gen != 0) resting.push_back({side, price, q, id, gen});
                break;
            }
            case 2: {  // contract add
                const Qty q = static_cast<Qty>(rng.bounded(30));
                OT_CHECK_EQ(b.add(side, price, q), ms.add(price, q, gens) != 0);
                break;
            }
            case 3: {  // wildcard remove / reduce
                const Qty q = static_cast<Qty>(rng.bounded(60));
                const bool whole = rng.chance(1, 2);
                OT_CHECK_EQ(whole ? b.remove(side, price, q) : b.reduce(side, price, q), ms.take(price, q, whole, 0));
                break;
            }
            default: {  // id-checked removal of a previously added order
                if (resting.empty()) break;
                const std::size_t i = static_cast<std::size_t>(rng.bounded(resting.size()));
                Resting r = resting[i];
                ModelSide& rms = r.side == B ? bids : asks;
                const bool whole = rng.chance(2, 3);
                Qty q = r.qty;
                if (!whole || rng.chance(1, 4)) q = static_cast<Qty>(1 + rng.bounded(r.qty));
                OT_CHECK_EQ(whole ? b.remove_order(r.side, r.price, q, r.id) : b.reduce_order(r.side, r.price, q, r.id),
                            rms.take(r.price, q, whole, r.gen));
                if (whole || rng.chance(1, 2)) {
                    resting[i] = resting.back();
                    resting.pop_back();
                }
                break;
            }
        }
        compare(b, bids, asks, seed, step);
        if (step % 97 == 0) {
            // Point queries agree with the ladder for every price in range.
            const ModelSide* sides[2] = {&bids, &asks};
            const Side kinds[2] = {B, S};
            for (int k = 0; k < 2; ++k) {
                const auto want = sides[k]->ladder();
                for (Price p = -range / 2; p < range - range / 2; ++p) {
                    Qty expect = 0;
                    for (const auto& w : want)
                        if (w.first == p) expect = static_cast<Qty>(w.second.qty);
                    OT_CHECK_EQ(b.qty_at(kinds[k], p), expect);
                }
                std::uint64_t top3 = 0;
                for (std::size_t i = 0; i < want.size() && i < 3; ++i) top3 += want[i].second.qty;
                OT_CHECK_EQ(b.total_qty(kinds[k], 3),
                            static_cast<Qty>(std::min<std::uint64_t>(top3, std::numeric_limits<Qty>::max())));
            }
            const auto bb = bids.ladder();
            const auto aa = asks.ladder();
            OT_CHECK_EQ(b.crossed(), !bb.empty() && !aa.empty() && bb.front().first >= aa.front().first);
        }
    }
}

}  // namespace

OT_TEST(property_matches_model_generous_capacity) {
    for (int seed = 1; seed <= 4; ++seed) property_run(seed, 64, 40, 40'000);
}

OT_TEST(property_matches_model_under_constant_overflow) {
    // Range far wider than capacity: refusals, evictions and stale ids every few steps.
    for (int seed = 11; seed <= 16; ++seed) property_run(seed, 5, 60, 40'000);
    for (int seed = 21; seed <= 22; ++seed) property_run(seed, 1, 12, 20'000);
    for (int seed = 31; seed <= 32; ++seed) property_run(seed, 0, 12, 5'000);
}

OT_TEST_MAIN()

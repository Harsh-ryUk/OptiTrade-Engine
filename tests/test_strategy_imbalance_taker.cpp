// Tests for the imbalance taker.
//
// Scenario tests use small books whose depth sums and imbalance are worked out by hand in
// the comments. The last test plays random books against a naive re-implementation of the
// decision rules written from the documented rules, using Responder as an instant exchange.
//
// Prices are in 1e-4 units: 1'000'000 = 100.0000, one cent = 100.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <type_traits>
#include <utility>
#include <vector>

#include "check.hpp"
#include "optitrade/core/rng.hpp"
#include "optitrade/strategy/imbalance_taker.hpp"
#include "test_strategy_util.hpp"

using namespace optitrade;
using namespace ot_strat;
using strategy::ImbalanceTaker;
using Levels = std::vector<std::pair<Price, Qty>>;

namespace {

constexpr Locate kLoc = 1;

ImbalanceTaker::Config config() {
    ImbalanceTaker::Config c;
    c.max_locates = 4;
    c.depth = 3;
    c.enter_bps = 3000;
    c.exit_bps = 1000;
    c.max_spread = 200;
    c.order_qty = 100;
    c.max_position = 250;
    c.cooldown_ns = 1000;
    return c;
}

// One update on `kLoc` at time `now`; returns how many actions were recorded.
std::size_t poke(Harness& h, ImbalanceTaker& s, Nanos now, Locate loc = kLoc) {
    const std::size_t before = h.orders.actions.size();
    h.now = now;
    s.on_book_update(loc, h.ctx());
    return h.orders.actions.size() - before;
}

// A 3-deep-plus-noise book with B = 1000 shares on the top three bids and A = 400 on the
// top three asks; the fourth level on each side (5000 / 9000 shares) lies beyond depth 3
// and must not count.
void strong_bid_book(Harness& h) {
    h.set_book(kLoc, {{1'000'000, 600}, {999'900, 300}, {999'800, 100}, {999'700, 5000}},
               {{1'000'100, 200}, {1'000'200, 100}, {1'000'300, 100}, {1'000'400, 9000}});
}

}  // namespace

static_assert(strategy::Strategy<ImbalanceTaker>);
static_assert(std::is_default_constructible_v<ImbalanceTaker>);

OT_TEST(imbalance_arithmetic_known_answers) {
    // (B - A) * 10000 / (B + A), truncated toward zero.
    OT_CHECK_EQ(ImbalanceTaker::imbalance_bps(1000, 400), 4285);   // 600 * 10000 / 1400 = 4285.7
    OT_CHECK_EQ(ImbalanceTaker::imbalance_bps(400, 1000), -4285);  // antisymmetric
    OT_CHECK_EQ(ImbalanceTaker::imbalance_bps(1300, 700), 3000);   // 600 * 10000 / 2000, exact
    OT_CHECK_EQ(ImbalanceTaker::imbalance_bps(1299, 700), 2996);   // 599 * 10000 / 1999 = 2996.5
    OT_CHECK_EQ(ImbalanceTaker::imbalance_bps(7, 3), 4000);        // 4 * 10000 / 10
    OT_CHECK_EQ(ImbalanceTaker::imbalance_bps(5, 0), 10000);
    OT_CHECK_EQ(ImbalanceTaker::imbalance_bps(0, 5), -10000);
    OT_CHECK_EQ(ImbalanceTaker::imbalance_bps(0, 0), 0);
    // Largest possible sums do not overflow: 2 * (2^32 - 1) shares.
    OT_CHECK_EQ(ImbalanceTaker::imbalance_bps(4'294'967'295LL, 0), 10000);
}

OT_TEST(buys_at_ask_when_bid_side_heavy_and_ignores_levels_beyond_depth) {
    Harness h;
    ImbalanceTaker s(config());
    strong_bid_book(h);
    // depth 3: B = 600+300+100 = 1000, A = 200+100+100 = 400 -> 4285 >= 3000, spread 100 <= 200.
    OT_CHECK_EQ(poke(h, s, 10'000), 1u);
    const Action& a = h.orders.actions.back();
    OT_CHECK(a.kind == Action::Kind::submit);
    OT_CHECK_EQ(a.locate, kLoc);
    OT_CHECK(a.side == Side::buy);
    OT_CHECK_EQ(a.price, 1'000'100);  // the best ask
    OT_CHECK_EQ(a.qty, 100u);
    OT_CHECK(a.tif == oms::Tif::ioc);
    OT_CHECK_EQ(a.now, 10'000u);
}

OT_TEST(sells_at_bid_when_ask_side_heavy) {
    Harness h;
    ImbalanceTaker s(config());
    h.set_book(kLoc, {{1'000'000, 100}, {999'900, 100}, {999'800, 100}},
               {{1'000'100, 600}, {1'000'200, 300}, {1'000'300, 100}});
    // B = 300, A = 1000 -> (300 - 1000) * 10000 / 1300 = -5384.6 -> -5384
    OT_CHECK_EQ(poke(h, s, 5), 1u);
    const Action& a = h.orders.actions.back();
    OT_CHECK(a.side == Side::sell);
    OT_CHECK_EQ(a.price, 1'000'000);
    OT_CHECK_EQ(a.qty, 100u);
}

OT_TEST(entry_threshold_is_inclusive_at_exactly_enter_bps) {
    auto entries = [](Qty bid_qty, Qty ask_qty) {
        Harness h;
        ImbalanceTaker s(config());
        h.set_book(kLoc, {{1'000'000, bid_qty}}, {{1'000'100, ask_qty}});
        return poke(h, s, 1);
    };
    OT_CHECK_EQ(entries(1300, 700), 1u);  // exactly 3000
    OT_CHECK_EQ(entries(1299, 700), 0u);  // 2996
    // and the mirror image on the sell side
    OT_CHECK_EQ(entries(700, 1300), 1u);  // exactly -3000
    OT_CHECK_EQ(entries(700, 1299), 0u);  // -2996
}

OT_TEST(spread_guard_boundary) {
    auto entries = [](Price ask) {
        Harness h;
        ImbalanceTaker s(config());
        h.set_book(kLoc, {{1'000'000, 900}}, {{ask, 100}});  // imbalance 8000
        return poke(h, s, 1);
    };
    OT_CHECK_EQ(entries(1'000'200), 1u);  // spread 200 == max_spread: allowed
    OT_CHECK_EQ(entries(1'000'201), 0u);  // 201: blocked
}

OT_TEST(no_orders_on_unusable_books) {
    Harness h;
    ImbalanceTaker s(config());
    // unknown book
    OT_CHECK_EQ(poke(h, s, 1), 0u);
    // bids only
    h.set_book(kLoc, {{1'000'000, 900}}, {});
    OT_CHECK_EQ(poke(h, s, 2), 0u);
    // asks only
    h.set_book(kLoc, {}, {{1'000'100, 900}});
    OT_CHECK_EQ(poke(h, s, 3), 0u);
    // crossed
    h.set_book(kLoc, {{1'000'200, 900}}, {{1'000'100, 100}});
    OT_CHECK_EQ(poke(h, s, 4), 0u);
    // locked
    h.set_book(kLoc, {{1'000'100, 900}}, {{1'000'100, 100}});
    OT_CHECK_EQ(poke(h, s, 5), 0u);
    // a holder is not flattened on a broken book either
    h.fill(kLoc, Side::buy, 100, 1'000'000);
    h.set_book(kLoc, {{1'000'200, 100}}, {{1'000'100, 900}});
    OT_CHECK_EQ(poke(h, s, 6), 0u);
}

OT_TEST(one_order_in_flight_then_cooldown_then_capped_by_max_position) {
    Harness h;
    ImbalanceTaker s(config());  // cooldown 1000, max_position 250, order_qty 100
    strong_bid_book(h);
    OT_CHECK_EQ(poke(h, s, 10'000), 1u);  // order 1, IOC still pending_new
    OT_CHECK_EQ(poke(h, s, 20'000), 0u);  // long past the cooldown, but nothing else in flight allowed

    auto finish = [&](oms::OrderId id, Qty qty, Price px) {
        h.fill(kLoc, Side::buy, qty, px);
        h.orders.mut(id).cum_qty = qty;
        h.orders.set_status(id, oms::OrderStatus::filled);
        s.on_order_update(*h.orders.find(id), h.ctx());
    };
    finish(1, 100, 1'000'100);  // position 100

    // Cooldown counts from the attempt at 10'000: 10'999 is one nanosecond short.
    OT_CHECK_EQ(poke(h, s, 10'999), 0u);
    OT_CHECK_EQ(poke(h, s, 11'000), 1u);  // room 250 - 100 = 150 -> full 100
    OT_CHECK_EQ(h.orders.actions.back().qty, 100u);
    finish(2, 100, 1'000'100);  // position 200

    OT_CHECK_EQ(poke(h, s, 12'000), 1u);  // room 250 - 200 = 50 -> clipped to 50
    OT_CHECK_EQ(h.orders.actions.back().qty, 50u);
    finish(3, 50, 1'000'100);  // position 250

    OT_CHECK_EQ(poke(h, s, 13'000), 0u);  // at the limit
    OT_CHECK_EQ(poke(h, s, 99'000), 0u);
    OT_CHECK_EQ(h.orders.actions.size(), 3u);
}

OT_TEST(gate_reopens_from_find_even_if_the_update_was_missed) {
    Harness h;
    ImbalanceTaker s(config());
    strong_bid_book(h);
    OT_CHECK_EQ(poke(h, s, 10'000), 1u);
    h.orders.set_status(1, oms::OrderStatus::canceled);  // no on_order_update delivered
    OT_CHECK_EQ(poke(h, s, 12'000), 1u);
}

OT_TEST(refused_submits_still_start_the_cooldown) {
    Harness h;
    ImbalanceTaker s(config());
    strong_bid_book(h);
    h.orders.submit_status = oms::SubmitStatus::rejected_by_risk;
    OT_CHECK_EQ(poke(h, s, 10'000), 1u);
    OT_CHECK_EQ(poke(h, s, 10'500), 0u);
    OT_CHECK_EQ(poke(h, s, 11'000), 1u);  // tries again once the cooldown has passed
    h.orders.submit_status = oms::SubmitStatus::ok;
    OT_CHECK_EQ(poke(h, s, 12'000), 1u);
}

OT_TEST(long_exit_hysteresis_between_exit_and_enter) {
    // Position 100 long. exit_bps = 1000, enter_bps = 3000.
    auto step = [](Qty bid_qty, Qty ask_qty, Price ask = 1'000'100) {
        Harness h;
        ImbalanceTaker s(config());
        h.fill(kLoc, Side::buy, 100, 1'000'000);
        h.set_book(kLoc, {{1'000'000, bid_qty}}, {{ask, ask_qty}});
        const std::size_t n = poke(h, s, 1);
        return std::pair{n, n ? h.orders.actions.back() : Action{}};
    };
    // 2000 bps: inside the band, hold.
    OT_CHECK_EQ(step(1200, 800).first, 0u);  // 400 * 10000 / 2000 = 2000
    // 1004 bps: still above exit_bps, hold.
    OT_CHECK_EQ(step(1101, 900).first, 0u);  // 201 * 10000 / 2001 = 1004.5
    // Exactly exit_bps: flatten (the exit is inclusive).
    auto [n, a] = step(1100, 900);            // 200 * 10000 / 2000 = 1000
    OT_CHECK_EQ(n, 1u);
    OT_CHECK(a.side == Side::sell);
    OT_CHECK_EQ(a.price, 1'000'000);  // hit the bid
    OT_CHECK_EQ(a.qty, 100u);
    OT_CHECK(a.tif == oms::Tif::ioc);
    // Just below exit_bps: 995.
    OT_CHECK_EQ(step(1099, 900).first, 1u);  // 199 * 10000 / 1999 = 995.5
    // Exits are not blocked by a wide spread (ask 5.0000 away) and never exceed the position.
    auto [nw, aw] = step(1100, 900, 1'050'000);
    OT_CHECK_EQ(nw, 1u);
    OT_CHECK(aw.side == Side::sell);
    OT_CHECK_EQ(aw.qty, 100u);
}

OT_TEST(short_exit_mirrors_long_exit) {
    auto step = [](Qty bid_qty, Qty ask_qty) {
        Harness h;
        ImbalanceTaker s(config());
        h.fill(kLoc, Side::sell, 100, 1'000'000);
        h.set_book(kLoc, {{1'000'000, bid_qty}}, {{1'000'100, ask_qty}});
        const std::size_t n = poke(h, s, 1);
        return std::pair{n, n ? h.orders.actions.back() : Action{}};
    };
    OT_CHECK_EQ(step(800, 1200).first, 0u);   // -2000: hold
    OT_CHECK_EQ(step(900, 1101).first, 0u);   // -1004: hold
    auto [n, a] = step(900, 1100);            // -1000: cover
    OT_CHECK_EQ(n, 1u);
    OT_CHECK(a.side == Side::buy);
    OT_CHECK_EQ(a.price, 1'000'100);  // lift the ask
    OT_CHECK_EQ(a.qty, 100u);
}

OT_TEST(exit_size_is_the_position_capped_at_order_qty) {
    Harness h;
    ImbalanceTaker s(config());
    h.fill(kLoc, Side::buy, 30, 1'000'000);
    h.set_book(kLoc, {{1'000'000, 100}}, {{1'000'100, 900}});  // -8000: exit
    OT_CHECK_EQ(poke(h, s, 1), 1u);
    OT_CHECK_EQ(h.orders.actions.back().qty, 30u);

    Harness h2;
    ImbalanceTaker s2(config());
    h2.fill(kLoc, Side::buy, 250, 1'000'000);
    h2.set_book(kLoc, {{1'000'000, 100}}, {{1'000'100, 900}});
    OT_CHECK_EQ(poke(h2, s2, 1), 1u);
    OT_CHECK_EQ(h2.orders.actions.back().qty, 100u);  // one clip at a time
}

OT_TEST(opposite_signal_flattens_before_flipping) {
    Harness h;
    ImbalanceTaker s(config());
    h.fill(kLoc, Side::sell, 100, 1'000'000);
    strong_bid_book(h);  // +4285 while short
    OT_CHECK_EQ(poke(h, s, 1), 1u);
    const Action& a = h.orders.actions.back();
    OT_CHECK(a.side == Side::buy);
    OT_CHECK_EQ(a.qty, 100u);  // covers the short; does not buy 200
}

OT_TEST(short_side_position_cap) {
    Harness h;
    ImbalanceTaker s(config());
    h.fill(kLoc, Side::sell, 240, 1'000'000);
    h.set_book(kLoc, {{1'000'000, 100}}, {{1'000'100, 900}});  // -8000: keep selling, but only the 10 of room
    OT_CHECK_EQ(poke(h, s, 1), 1u);
    OT_CHECK_EQ(h.orders.actions.back().qty, 10u);
    Harness h2;
    ImbalanceTaker s2(config());
    h2.set_book(kLoc, {{1'000'000, 100}}, {{1'000'100, 900}});
    OT_CHECK_EQ(poke(h2, s2, 1), 1u);  // flat: sells 100
    h2.fill(kLoc, Side::sell, 150, 1'000'000);
    h2.orders.set_status(1, oms::OrderStatus::canceled);
    OT_CHECK_EQ(poke(h2, s2, 5000), 1u);  // short 150, room 100
    OT_CHECK_EQ(h2.orders.actions.back().qty, 100u);
    h2.orders.set_status(2, oms::OrderStatus::canceled);
    h2.fill(kLoc, Side::sell, 90, 1'000'000);  // short 240
    OT_CHECK_EQ(poke(h2, s2, 9000), 1u);
    OT_CHECK_EQ(h2.orders.actions.back().qty, 10u);  // room 250 - 240
}

OT_TEST(locates_at_or_above_max_locates_are_ignored) {
    Harness h;
    ImbalanceTaker s(config());  // max_locates = 4
    for (Locate loc : {Locate{3}, Locate{4}, Locate{5}, Locate{65535}}) {
        h.set_book(loc, {{1'000'000, 900}}, {{1'000'100, 100}});
    }
    OT_CHECK_EQ(poke(h, s, 1, 4), 0u);
    OT_CHECK_EQ(poke(h, s, 1, 5), 0u);
    OT_CHECK_EQ(poke(h, s, 1, 65535), 0u);
    OT_CHECK_EQ(poke(h, s, 1, 3), 1u);
    // Order updates for ignored locates are harmless too.
    oms::OrderInfo o;
    o.id = 7;
    o.req.locate = 65535;
    o.status = oms::OrderStatus::filled;
    s.on_order_update(o, h.ctx());
}

OT_TEST(instruments_are_independent) {
    Harness h;
    ImbalanceTaker s(config());
    h.set_book(1, {{1'000'000, 900}}, {{1'000'100, 100}});
    h.set_book(2, {{2'000'000, 900}}, {{2'000'100, 100}});
    OT_CHECK_EQ(poke(h, s, 100, 1), 1u);
    OT_CHECK_EQ(poke(h, s, 100, 2), 1u);  // instrument 1 in flight does not gate instrument 2
    OT_CHECK_EQ(h.orders.actions[1].locate, 2);
    OT_CHECK_EQ(h.orders.actions[1].price, 2'000'100);
}

OT_TEST(config_is_sanitised) {
    ImbalanceTaker::Config c = config();
    c.exit_bps = 5000;  // above enter_bps: would make the strategy flap
    c.enter_bps = 3000;
    c.order_qty = 0;
    c.depth = 0;
    ImbalanceTaker s(c);
    OT_CHECK(s.config().exit_bps < s.config().enter_bps);
    OT_CHECK(s.config().order_qty >= 1);
    OT_CHECK(s.config().depth >= 1);
}

// Naive model of the documented rules, working on raw level lists instead of the book.
namespace {

struct Expect {
    bool act{};
    Side side{};
    Price price{};
    Qty qty{};
};

Expect model(Levels bids, Levels asks, std::int64_t pos, const ImbalanceTaker::Config& c) {
    std::sort(bids.begin(), bids.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::sort(asks.begin(), asks.end(), [](auto& a, auto& b) { return a.first < b.first; });
    if (bids.empty() || asks.empty() || bids[0].first >= asks[0].first) return {};
    std::int64_t b = 0, a = 0;
    for (std::size_t i = 0; i < c.depth && i < bids.size(); ++i) b += bids[i].second;
    for (std::size_t i = 0; i < c.depth && i < asks.size(); ++i) a += asks[i].second;
    const std::int64_t imb = (b - a) * 10000 / (b + a);
    const Price bid = bids[0].first, ask = asks[0].first;
    auto clip = [&](std::int64_t q) { return static_cast<Qty>(std::min<std::int64_t>(q, c.order_qty)); };
    if (pos > 0 && imb <= c.exit_bps) return {true, Side::sell, bid, clip(pos)};
    if (pos < 0 && imb >= -c.exit_bps) return {true, Side::buy, ask, clip(-pos)};
    if (ask - bid > c.max_spread) return {};
    if (imb >= c.enter_bps && pos < c.max_position) return {true, Side::buy, ask, clip(c.max_position - pos)};
    if (imb <= -c.enter_bps && -pos < c.max_position) return {true, Side::sell, bid, clip(c.max_position + pos)};
    return {};
}

}  // namespace

OT_TEST(randomized_differential_against_naive_model) {
    ImbalanceTaker::Config c = config();
    c.cooldown_ns = 0;
    c.depth = 3;
    Harness h;
    ImbalanceTaker s(c);
    Responder<ImbalanceTaker> ex(h, s);
    Rng rng(0x1B'A1A4CEULL);
    std::size_t acted = 0, quiet = 0;
    for (int i = 0; i < 20'000; ++i) {
        Levels bids, asks;
        Price bid_top = 1'000'000 + rng.range(-3, 3) * 100;
        Price ask_top = bid_top + rng.range(-1, 5) * 100;  // -1 crosses, 0 locks
        for (std::uint64_t k = 0, n = rng.bounded(6); k < n; ++k) {
            bids.push_back({bid_top - static_cast<Price>(k) * 100, static_cast<Qty>(rng.range(1, 2000))});
        }
        for (std::uint64_t k = 0, n = rng.bounded(6); k < n; ++k) {
            asks.push_back({ask_top + static_cast<Price>(k) * 100, static_cast<Qty>(rng.range(1, 2000))});
        }
        h.set_book(kLoc, bids, asks);
        h.now += 1 + rng.bounded(1000);

        const std::int64_t pos = h.risk.position(kLoc).qty;
        const Expect e = model(bids, asks, pos, c);
        const std::size_t before = h.orders.actions.size();
        s.on_book_update(kLoc, h.ctx());
        const std::size_t n = h.orders.actions.size() - before;
        OT_CHECK_EQ(n, e.act ? 1u : 0u);
        if (n == 1 && e.act) {
            const Action& a = h.orders.actions.back();
            OT_CHECK(a.side == e.side);
            OT_CHECK_EQ(a.price, e.price);
            OT_CHECK_EQ(a.qty, e.qty);
            OT_CHECK(a.qty > 0 && a.price > 0);
            ++acted;
        } else {
            ++quiet;
        }
        ex.settle();
        OT_CHECK(std::abs(h.risk.position(kLoc).qty) <= c.max_position);
    }
    // The run must exercise both branches for the comparison to mean anything.
    OT_CHECK(acted > 500);
    OT_CHECK(quiet > 500);
}

OT_TEST_MAIN()

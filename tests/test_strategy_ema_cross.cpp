// Tests for the EMA crossover strategy.
//
// The scripted price paths use a fixed 100-unit spread, bid = P and ask = P + 100, so the
// sample fed to the averages is bid + ask = 2P + 100. With fast_shift 1, slow_shift 2 and
// kFrac = 8 fractional bits the averages of a path can be followed by hand:
//
//   P = 1'000'000 (flat) : sample = 2'000'100 * 256 = 512'025'600 for both averages
//   P = 1'000'100        : sample 512'076'800, diff to both averages 51'200
//                          fast += 51'200 >> 1 = 25'600 -> 512'051'200
//                          slow += 51'200 >> 2 = 12'800 -> 512'038'400   (fast - slow = 12'800)
//   P = 1'000'200        : sample 512'128'000
//                          fast += (512'128'000 - 512'051'200) >> 1 = 38'400 -> 512'089'600
//                          slow += (512'128'000 - 512'038'400) >> 2 = 22'400 -> 512'060'800
//
// Later rows of the tables below (marked "model") were produced by a separate Python
// implementation of the same recurrence, not by this code.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <type_traits>
#include <vector>

#include "check.hpp"
#include "optitrade/core/rng.hpp"
#include "optitrade/strategy/ema_cross.hpp"
#include "test_strategy_util.hpp"

using namespace optitrade;
using namespace ot_strat;
using strategy::EmaCross;
using K = Action::Kind;

namespace {

constexpr Locate kLoc = 1;

EmaCross::Config config() {
    EmaCross::Config c;
    c.max_locates = 4;
    c.fast_shift = 1;
    c.slow_shift = 2;
    c.warmup = 4;
    c.confirm_ticks = 2;
    c.order_qty = 10;
    c.max_position = 10;
    c.cooldown_ns = 0;
    return c;
}

// Drives a price path one book update at a time and lets tests interleave fills.
struct Run {
    Harness h;
    EmaCross s;
    Nanos t{0};
    explicit Run(const EmaCross::Config& c = config()) : s(c) {}

    // Moves the book to bid = p, ask = p + 100 and updates the strategy; returns new actions.
    std::size_t step(Price p, Locate loc = kLoc) {
        h.set_book(loc, {{p, 100}}, {{p + 100, 100}});
        h.now = (t += 1000);
        const std::size_t before = h.orders.actions.size();
        s.on_book_update(loc, h.ctx());
        return h.orders.actions.size() - before;
    }
    // The last order is filled completely at its limit.
    void fill_last() {
        const oms::OrderId id = h.orders.last_id();
        const oms::OrderInfo& o = *h.orders.find(id);
        h.fill(o.req.locate, o.req.side, o.req.qty, o.req.price);
        h.orders.mut(id).cum_qty = o.req.qty;
        h.orders.set_status(id, oms::OrderStatus::filled);
        s.on_order_update(*h.orders.find(id), h.ctx());
    }
    const Action& last() const { return h.orders.actions.back(); }
};

}  // namespace

static_assert(strategy::Strategy<EmaCross>);
static_assert(std::is_default_constructible_v<EmaCross>);

OT_TEST(averages_follow_the_recurrence_and_seed_from_the_first_sample) {
    Run r;
    OT_CHECK_EQ(r.s.samples(kLoc), 0u);
    r.step(1'000'000);
    OT_CHECK_EQ(r.s.fast(kLoc), 512'025'600);
    OT_CHECK_EQ(r.s.slow(kLoc), 512'025'600);
    r.step(1'000'000);
    r.step(1'000'000);
    r.step(1'000'000);
    OT_CHECK_EQ(r.s.samples(kLoc), 4u);
    r.step(1'000'100);
    OT_CHECK_EQ(r.s.fast(kLoc), 512'051'200);
    OT_CHECK_EQ(r.s.slow(kLoc), 512'038'400);
    r.step(1'000'200);
    OT_CHECK_EQ(r.s.fast(kLoc), 512'089'600);
    OT_CHECK_EQ(r.s.slow(kLoc), 512'060'800);
    r.step(1'000'300);  // model
    OT_CHECK_EQ(r.s.fast(kLoc), 512'134'400);
    OT_CHECK_EQ(r.s.slow(kLoc), 512'090'400);
    r.step(1'000'300);  // model
    OT_CHECK_EQ(r.s.fast(kLoc), 512'156'800);
    OT_CHECK_EQ(r.s.slow(kLoc), 512'112'600);
    r.step(1'000'000);  // model: a falling sample rounds toward minus infinity
    OT_CHECK_EQ(r.s.fast(kLoc), 512'091'200);
    OT_CHECK_EQ(r.s.slow(kLoc), 512'090'850);
    r.step(900'000);    // model
    OT_CHECK_EQ(r.s.fast(kLoc), 486'458'400);
    OT_CHECK_EQ(r.s.slow(kLoc), 499'274'537);
}

OT_TEST(averages_match_floor_division_on_random_paths) {
    EmaCross::Config c = config();
    c.fast_shift = 3;
    c.slow_shift = 7;
    Run r(c);
    Rng rng(99);
    std::int64_t f = 0, sl = 0;
    auto floor_div = [](std::int64_t x, std::int64_t d) {
        std::int64_t q = x / d;
        if (x % d != 0 && x < 0) --q;
        return q;
    };
    for (int i = 0; i < 5000; ++i) {
        const Price p = 1'000'000 + rng.range(-50'000, 50'000);
        r.step(p);
        const std::int64_t x = (2 * p + 100) * 256;
        if (i == 0) {
            f = sl = x;
        } else {
            f += floor_div(x - f, 8);
            sl += floor_div(x - sl, 128);
        }
        OT_CHECK_EQ(r.s.fast(kLoc), f);
        OT_CHECK_EQ(r.s.slow(kLoc), sl);
    }
}

OT_TEST(enters_long_after_confirmed_cross_then_exits_and_reverses) {
    Run r;
    for (int i = 0; i < 4; ++i) OT_CHECK_EQ(r.step(1'000'000), 0u);  // warm-up, no signal
    // Step 5: fast 512'051'200 > slow 512'038'400: a cross, run 1 of 2 needed.
    OT_CHECK_EQ(r.step(1'000'100), 0u);
    // Step 6: run 2 -> buy 10 at the ask (1'000'200 + 100).
    OT_CHECK_EQ(r.step(1'000'200), 1u);
    OT_CHECK(r.last().kind == K::submit && r.last().side == Side::buy);
    OT_CHECK_EQ(r.last().price, 1'000'300);
    OT_CHECK_EQ(r.last().qty, 10u);
    OT_CHECK(r.last().tif == oms::Tif::ioc);
    // Still in flight: the trend continues but nothing more is sent.
    OT_CHECK_EQ(r.step(1'000'300), 0u);
    r.fill_last();
    OT_CHECK_EQ(r.step(1'000'300), 0u);  // at target
    OT_CHECK_EQ(r.step(1'000'000), 0u);  // sign still up (fast - slow = 350)
    // P = 900'000: fast - slow = -12'816'137, an opposite cross with run 1 (< confirm):
    // the long is closed immediately at the bid.
    OT_CHECK_EQ(r.step(900'000), 1u);
    OT_CHECK(r.last().side == Side::sell);
    OT_CHECK_EQ(r.last().price, 900'000);
    OT_CHECK_EQ(r.last().qty, 10u);
    r.fill_last();
    // Next update: run 2 confirms the downtrend, go short 10.
    OT_CHECK_EQ(r.step(900'000), 1u);
    OT_CHECK(r.last().side == Side::sell);
    OT_CHECK_EQ(r.last().qty, 10u);
    r.fill_last();
    OT_CHECK_EQ(r.h.risk.position(kLoc).qty, -10);
    OT_CHECK_EQ(r.step(900'000), 0u);
    OT_CHECK_EQ(r.step(900'000), 0u);
}

OT_TEST(cross_reverting_before_confirmation_does_not_enter) {
    EmaCross::Config c = config();
    c.confirm_ticks = 3;
    Run r(c);
    for (int i = 0; i < 4; ++i) r.step(1'000'000);
    OT_CHECK_EQ(r.step(1'000'100), 0u);  // up cross, run 1
    OT_CHECK_EQ(r.step(1'000'000), 0u);  // fast - slow = 3200: still up, run 2
    OT_CHECK_EQ(r.step(1'000'000), 0u);  // fast - slow = -800: flipped down, run 1, up-trade never confirmed
    OT_CHECK_EQ(r.step(1'000'000), 0u);  // run 2
    OT_CHECK_EQ(r.h.orders.actions.size(), 0u);
    OT_CHECK_EQ(r.step(1'000'000), 1u);  // run 3 (fast - slow = -2450): short entry
    OT_CHECK(r.last().side == Side::sell);
    OT_CHECK_EQ(r.last().price, 1'000'000);
}

OT_TEST(a_trend_already_under_way_at_the_end_of_warmup_is_not_a_cross) {
    Run r;
    // Rising every step from the second sample: the sign is established during warm-up, so
    // run passes confirm_ticks without ever having been armed.
    for (int i = 0; i < 8; ++i) OT_CHECK_EQ(r.step(1'000'000 + 100 * i), 0u);
    OT_CHECK_EQ(r.s.samples(kLoc), 8u);
    // The turn is a real cross: armed, run 1, then run 2 (model: fast - slow = -15'176, -49'832).
    OT_CHECK_EQ(r.step(1'000'000), 0u);
    OT_CHECK_EQ(r.step(1'000'000), 1u);
    OT_CHECK(r.last().side == Side::sell);
    OT_CHECK_EQ(r.last().price, 1'000'000);
}

OT_TEST(exit_on_opposite_cross_needs_no_confirmation_even_if_entry_was_never_armed) {
    Run r;
    // Rising warm-up (never armed), so no entry. A pre-existing long is still closed at
    // the first opposite cross.
    r.h.fill(kLoc, Side::buy, 10, 1'000'000);
    for (int i = 0; i < 8; ++i) OT_CHECK_EQ(r.step(1'000'000 + 100 * i), 0u);  // long, trend up: hold
    OT_CHECK_EQ(r.step(1'000'000), 1u);                                      // flip down: flatten
    OT_CHECK(r.last().side == Side::sell);
    OT_CHECK_EQ(r.last().qty, 10u);
}

OT_TEST(no_orders_during_warmup_even_with_a_position_against_the_sign) {
    EmaCross::Config c = config();
    c.warmup = 10;
    Run r(c);
    r.h.fill(kLoc, Side::buy, 10, 1'000'000);
    // Falling from the start: sign is down from sample 2 but warm-up lasts 10 samples.
    for (int i = 0; i < 9; ++i) OT_CHECK_EQ(r.step(1'000'000 - 100 * i), 0u);
    OT_CHECK_EQ(r.step(1'000'000 - 900), 1u);  // sample 10: warm; long against a down sign -> sell
    OT_CHECK(r.last().side == Side::sell);
}

OT_TEST(cooldown_retries_and_clips_orders_to_order_qty) {
    EmaCross::Config c = config();
    c.cooldown_ns = 5000;
    Run r(c);  // clock advances 1000 per step
    for (int i = 0; i < 4; ++i) r.step(1'000'000);          // t = 1000..4000
    OT_CHECK_EQ(r.step(1'000'100), 0u);                     // t = 5000, cross
    OT_CHECK_EQ(r.step(1'000'200), 1u);                     // t = 6000: buy, cooldown starts
    OT_CHECK_EQ(r.last().now, 6000u);
    r.fill_last();
    OT_CHECK_EQ(r.step(1'000'300), 0u);                     // t = 7000
    OT_CHECK_EQ(r.step(1'000'300), 0u);                     // t = 8000
    OT_CHECK_EQ(r.step(1'000'000), 0u);                     // t = 9000
    OT_CHECK_EQ(r.step(900'000), 0u);                       // t = 10'000: exit due but 4000 < 5000
    OT_CHECK_EQ(r.step(900'000), 1u);                       // t = 11'000: exactly 5000 after 6000
    // Target is now -10 while long 10: the gap is 20, one order carries only order_qty = 10.
    OT_CHECK(r.last().side == Side::sell);
    OT_CHECK_EQ(r.last().qty, 10u);
    r.fill_last();                                          // flat
    OT_CHECK_EQ(r.step(900'000), 0u);                       // t = 12'000
    OT_CHECK_EQ(r.step(900'000), 0u);                       // t = 13'000
    OT_CHECK_EQ(r.step(900'000), 0u);                       // t = 14'000
    OT_CHECK_EQ(r.step(900'000), 0u);                       // t = 15'000
    OT_CHECK_EQ(r.step(900'000), 1u);                       // t = 16'000: short entry
    OT_CHECK_EQ(r.last().qty, 10u);
    OT_CHECK_EQ(r.h.orders.actions.size(), 3u);
}

OT_TEST(unfilled_ioc_is_retried_until_the_position_reaches_the_target) {
    Run r;
    for (int i = 0; i < 4; ++i) r.step(1'000'000);
    r.step(1'000'100);
    OT_CHECK_EQ(r.step(1'000'200), 1u);
    r.h.orders.set_status(r.h.orders.last_id(), oms::OrderStatus::canceled);  // IOC found nothing
    r.s.on_order_update(*r.h.orders.find(r.h.orders.last_id()), r.h.ctx());
    OT_CHECK_EQ(r.step(1'000'300), 1u);  // still armed and confirmed: tries again
    OT_CHECK(r.last().side == Side::buy);
}

OT_TEST(target_is_capped_by_max_position) {
    EmaCross::Config c = config();
    c.order_qty = 50;
    c.max_position = 20;
    Run r(c);
    for (int i = 0; i < 4; ++i) r.step(1'000'000);
    r.step(1'000'100);
    OT_CHECK_EQ(r.step(1'000'200), 1u);
    OT_CHECK_EQ(r.last().qty, 20u);
}

OT_TEST(one_sided_or_crossed_books_are_not_sampled) {
    Run r;
    r.step(1'000'000);
    const std::int64_t f = r.s.fast(kLoc);
    r.h.set_book(kLoc, {{1'000'000, 100}}, {});
    r.s.on_book_update(kLoc, r.h.ctx());
    r.h.set_book(kLoc, {{2'000'000, 100}}, {{1'000'100, 100}});
    r.s.on_book_update(kLoc, r.h.ctx());
    OT_CHECK_EQ(r.s.samples(kLoc), 1u);
    OT_CHECK_EQ(r.s.fast(kLoc), f);
}

OT_TEST(locates_at_or_above_max_locates_are_ignored) {
    Run r;  // max_locates = 4
    for (int i = 0; i < 4; ++i) {
        r.step(1'000'000, 4);
        r.step(1'000'000, 65535);
    }
    r.step(1'000'100, 4);
    r.step(1'000'200, 4);
    r.step(1'000'100, 65535);
    r.step(1'000'200, 65535);
    OT_CHECK_EQ(r.h.orders.actions.size(), 0u);
    OT_CHECK_EQ(r.s.samples(4), 0u);
    OT_CHECK_EQ(r.s.fast(65535), 0);
    // the same path on locate 3 does trade
    for (int i = 0; i < 4; ++i) r.step(1'000'000, 3);
    r.step(1'000'100, 3);
    OT_CHECK_EQ(r.step(1'000'200, 3), 1u);
    oms::OrderInfo o;
    o.id = 9;
    o.req.locate = 65535;
    o.status = oms::OrderStatus::filled;
    r.s.on_order_update(o, r.h.ctx());
}

OT_TEST(config_is_sanitised) {
    EmaCross::Config c;
    c.fast_shift = 0;
    c.slow_shift = 0;
    c.confirm_ticks = 0;
    c.order_qty = 0;
    EmaCross s(c);
    OT_CHECK(s.config().fast_shift >= 1);
    OT_CHECK(s.config().slow_shift > s.config().fast_shift);
    OT_CHECK(s.config().confirm_ticks >= 1);
    OT_CHECK(s.config().order_qty >= 1);
    c.fast_shift = 40;
    c.slow_shift = 50;
    EmaCross s2(c);  // shifts stay below the word size
    OT_CHECK(s2.config().slow_shift <= 31);
    OT_CHECK(s2.config().fast_shift < s2.config().slow_shift);
}

OT_TEST(random_walk_orders_are_always_valid_and_position_bounded) {
    EmaCross::Config c = config();
    c.warmup = 8;
    c.fast_shift = 2;
    c.slow_shift = 4;
    c.order_qty = 30;
    c.max_position = 50;
    Run r(c);
    Responder<EmaCross> ex(r.h, r.s);
    Rng rng(4242);
    Price p = 1'000'000;
    std::int64_t drift = 100;
    for (int i = 0; i < 30'000; ++i) {
        if (i % 500 == 0) drift = -drift;
        p += drift + rng.range(-300, 300);
        if (p < 100'000) p = 100'000;
        r.step(p);
        ex.settle();
        OT_CHECK(std::abs(r.h.risk.position(kLoc).qty) <= 50);
    }
    OT_CHECK(r.h.orders.actions.size() > 50);
    for (const Action& a : r.h.orders.actions) {
        OT_CHECK(a.qty > 0 && a.qty <= 30);
        OT_CHECK(a.price > 0);
        OT_CHECK(a.tif == oms::Tif::ioc);
    }
}

OT_TEST_MAIN()

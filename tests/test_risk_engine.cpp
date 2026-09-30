// Tests for the pre-trade risk engine and its position/PnL accounting.
//
// Known-answer scenarios carry their arithmetic in comments so a reviewer can redo it by hand.
// The randomized tests compare the engine with reference models written here, independently of
// the engine's code path (see the notes on each).

#include <cstdint>
#include <cstdio>
#include <limits>
#include <numeric>
#include <string_view>
#include <vector>

#include "check.hpp"
#include "optitrade/core/rng.hpp"
#include "optitrade/core/types.hpp"
#include "optitrade/risk/risk_engine.hpp"

using namespace optitrade;
using risk::Limits;
using risk::Position;
using risk::Reject;
using risk::RiskEngine;

__extension__ typedef __int128 I128;

#if defined(__clang__)
// The reference model multiplies signed 128-bit values, which UBSan lowers to
// __muloti4; libgcc does not provide it, so the link fails on Linux. The model is
// checked for overflow by construction (values stay far below 2^127), so the
// instrumentation is switched off for this file's own functions.
#pragma clang attribute push(__attribute__((no_sanitize("signed-integer-overflow"))), apply_to = function)
#endif

namespace {

constexpr Nanos kSec = 1'000'000'000;
constexpr Locate kA = 1;
constexpr Locate kB = 2;
constexpr Locate kC = 3;
constexpr Side kBuy = Side::buy;
constexpr Side kSell = Side::sell;
constexpr std::int64_t kI64Max = std::numeric_limits<std::int64_t>::max();
constexpr Qty kQtyMax = std::numeric_limits<Qty>::max();

// Only the two hard caps are open; every optional limit is disabled, so a test switches on
// exactly the limit it is about.
Limits wide() {
    Limits l;
    l.max_order_qty = kMaxOrderQty;
    l.max_position = 1'000'000'000;
    return l;
}

Reject chk(RiskEngine& e, Side side, Price px, Qty qty, Price ref = 0, Nanos now = 0) {
    return e.check(kA, side, px, qty, ref, now);
}

// Compares reasons by name so a failure prints "position" instead of an enum ordinal.
#define OT_REJECT(got, want)                                                       \
    OT_CHECK_EQ(std::string_view(risk::to_string(got)), std::string_view(risk::to_string(want)))

}  // namespace

// ---------------------------------------------------------------------------------------------
// Pre-trade checks, one reason at a time
// ---------------------------------------------------------------------------------------------

OT_TEST(defaults_match_the_contract) {
    const Limits l;
    OT_CHECK_EQ(l.max_order_qty, Qty{1000});
    OT_CHECK_EQ(l.max_order_notional, std::int64_t{0});
    OT_CHECK_EQ(l.max_position, std::int64_t{5000});
    OT_CHECK_EQ(l.max_gross_position, std::int64_t{0});
    OT_CHECK_EQ(l.price_band_bps, std::uint32_t{0});
    OT_CHECK(!l.require_reference);
    OT_CHECK_EQ(l.max_orders_per_second, std::uint32_t{0});
    OT_CHECK_EQ(l.max_open_orders, std::uint32_t{0});
    OT_CHECK_EQ(l.max_loss, std::int64_t{0});

    RiskEngine e(l, 4);
    OT_CHECK(!e.kill_switch());
    OT_CHECK_EQ(e.open_orders(), 0u);
    OT_CHECK_EQ(e.total_pnl(), std::int64_t{0});
    OT_CHECK_EQ(e.max_drawdown(), std::int64_t{0});
    OT_REJECT(chk(e, kBuy, 1'000'000, 1000), Reject::none);
    OT_REJECT(chk(e, kBuy, 1'000'000, 1001), Reject::order_qty);
}

OT_TEST(kill_switch_beats_every_other_check) {
    RiskEngine e(wide(), 4);
    OT_REJECT(chk(e, kBuy, 100, 1), Reject::none);
    e.set_kill_switch(true);
    OT_CHECK(e.kill_switch());
    OT_REJECT(chk(e, kBuy, 100, 1), Reject::kill_switch);
    OT_REJECT(chk(e, kBuy, 0, 0), Reject::kill_switch);  // even malformed input reports the halt
    e.set_kill_switch(false);
    OT_CHECK(!e.kill_switch());
    OT_REJECT(chk(e, kBuy, 0, 0), Reject::invalid_order);
    OT_REJECT(chk(e, kBuy, 100, 1), Reject::none);
}

OT_TEST(invalid_orders) {
    Limits l = wide();
    l.max_order_qty = 10;
    RiskEngine e(l, 4);
    OT_REJECT(chk(e, kBuy, 100, 0), Reject::invalid_order);
    OT_REJECT(chk(e, kBuy, 0, 1), Reject::invalid_order);
    OT_REJECT(chk(e, kBuy, -1, 1), Reject::invalid_order);
    OT_REJECT(chk(e, kBuy, std::numeric_limits<Price>::min(), 1), Reject::invalid_order);
    // Malformed beats an oversized quantity: qty 11 alone would be order_qty.
    OT_REJECT(chk(e, kBuy, 0, 11), Reject::invalid_order);
    OT_REJECT(e.check(4, kBuy, 100, 1, 0, 0), Reject::invalid_order);  // == max_locates
    OT_REJECT(e.check(65535, kBuy, 100, 1, 0, 0), Reject::invalid_order);
    OT_REJECT(e.check(3, kBuy, 100, 1, 0, 0), Reject::none);  // last valid locate
    OT_REJECT(chk(e, static_cast<Side>(2), 100, 1), Reject::invalid_order);
    OT_REJECT(chk(e, static_cast<Side>(255), 100, 1), Reject::invalid_order);

    RiskEngine none(wide(), 0);
    OT_REJECT(none.check(0, kBuy, 100, 1, 0, 0), Reject::invalid_order);

    // The table is capped at the 16-bit Locate range but a larger request is not an error.
    RiskEngine big(wide(), 1'000'000);
    OT_REJECT(big.check(65535, kBuy, 100, 1, 0, 0), Reject::none);
}

OT_TEST(order_qty_boundary) {
    Limits l = wide();
    l.max_order_qty = 100;
    RiskEngine e(l, 4);
    OT_REJECT(chk(e, kBuy, 100, 100), Reject::none);
    OT_REJECT(chk(e, kBuy, 100, 101), Reject::order_qty);
    OT_REJECT(chk(e, kSell, 100, 101), Reject::order_qty);

    // A zero cap is not "unlimited": nothing may be sent.
    l.max_order_qty = 0;
    RiskEngine closed(l, 4);
    OT_REJECT(chk(closed, kBuy, 100, 1), Reject::order_qty);
}

OT_TEST(order_notional_boundary_and_overflow) {
    Limits l = wide();
    l.max_order_notional = 1'000'000'000;  // 100'000.0000 currency
    RiskEngine e(l, 4);
    OT_REJECT(chk(e, kBuy, 10'000'000, 100), Reject::none);  // exactly 1e9
    OT_REJECT(chk(e, kBuy, 10'000'000, 101), Reject::order_notional);
    OT_REJECT(chk(e, kBuy, 10'000'001, 100), Reject::order_notional);  // 1'000'000'100
    OT_REJECT(chk(e, kSell, 10'000'001, 100), Reject::order_notional);

    l.max_order_notional = 0;  // disabled
    RiskEngine off(l, 4);
    OT_REJECT(chk(off, kBuy, 10'000'001, 100), Reject::none);

    // The product does not fit in 64 bits; the check must still be exact.
    l.max_order_notional = kI64Max;
    RiskEngine hostile(l, 4);
    OT_REJECT(chk(hostile, kBuy, kI64Max / 2, 3), Reject::order_notional);  // 1.38e19 > 9.22e18
    OT_REJECT(chk(hostile, kBuy, kI64Max, 2), Reject::order_notional);
    OT_REJECT(chk(hostile, kBuy, kI64Max, 1), Reject::none);  // == limit exactly
}

OT_TEST(position_limit_includes_position_and_same_side_open_orders) {
    Limits l = wide();
    l.max_position = 1000;
    RiskEngine e(l, 4);
    OT_REJECT(chk(e, kBuy, 100, 1000), Reject::none);
    OT_REJECT(chk(e, kBuy, 100, 1001), Reject::position);
    OT_REJECT(chk(e, kSell, 100, 1000), Reject::none);
    OT_REJECT(chk(e, kSell, 100, 1001), Reject::position);

    // 600 shares already working to buy: 600 + q <= 1000.
    e.on_order_open(kA, kBuy, 600);
    OT_REJECT(chk(e, kBuy, 100, 400), Reject::none);
    OT_REJECT(chk(e, kBuy, 100, 401), Reject::position);
    // Working buys are not netted against a sell: the sell leg is still 0 - q.
    OT_REJECT(chk(e, kSell, 100, 1000), Reject::none);
    OT_REJECT(chk(e, kSell, 100, 1001), Reject::position);

    // 500 working sells tighten the sell leg only: |0 - 500 - q| <= 1000.
    e.on_order_open(kA, kSell, 500);
    OT_REJECT(chk(e, kSell, 100, 500), Reject::none);
    OT_REJECT(chk(e, kSell, 100, 501), Reject::position);
    OT_REJECT(chk(e, kBuy, 100, 400), Reject::none);

    // A fill of 300 long: buy leg 300 + 600 + q <= 1000 -> q <= 100; sell leg |300 - 500 - q|
    // = 200 + q <= 1000 -> q <= 800.
    e.on_fill(kA, kBuy, 300, 100);
    OT_REJECT(chk(e, kBuy, 100, 100), Reject::none);
    OT_REJECT(chk(e, kBuy, 100, 101), Reject::position);
    OT_REJECT(chk(e, kSell, 100, 800), Reject::none);
    OT_REJECT(chk(e, kSell, 100, 801), Reject::position);

    // Releasing the working buys frees the buy leg: 300 + q <= 1000.
    e.on_order_closed(kA, kBuy, 600);
    OT_REJECT(chk(e, kBuy, 100, 700), Reject::none);
    OT_REJECT(chk(e, kBuy, 100, 701), Reject::position);

    // The limit is per instrument.
    OT_REJECT(e.check(kB, kBuy, 100, 1000, 0, 0), Reject::none);
}

OT_TEST(position_limit_short_side) {
    Limits l = wide();
    l.max_position = 1000;
    RiskEngine e(l, 4);
    e.on_fill(kA, kSell, 300, 100);  // short 300
    OT_CHECK_EQ(e.position(kA).qty, -300);
    e.on_order_open(kA, kSell, 500);

    // Sell leg: |-300 - 500 - q| <= 1000 -> q <= 200.
    OT_REJECT(chk(e, kSell, 100, 200), Reject::none);
    OT_REJECT(chk(e, kSell, 100, 201), Reject::position);
    // Buy leg: |-300 + q| <= 1000 -> q <= 1300; the open sells do not help a buy.
    OT_REJECT(chk(e, kBuy, 100, 1300), Reject::none);
    OT_REJECT(chk(e, kBuy, 100, 1301), Reject::position);
}

OT_TEST(gross_position_limit) {
    Limits l = wide();
    l.max_gross_position = 1000;
    RiskEngine e(l, 4);
    e.on_fill(kA, kBuy, 400, 100);   // A long 400
    e.on_fill(kB, kSell, 300, 100);  // B short 300
    OT_CHECK_EQ(e.gross_exposure(), 700);

    OT_REJECT(e.check(kA, kBuy, 100, 300, 0, 0), Reject::none);  // 700 - 400 + 700 = 1000
    OT_REJECT(e.check(kA, kBuy, 100, 301, 0, 0), Reject::gross_position);
    OT_REJECT(e.check(kB, kSell, 100, 300, 0, 0), Reject::none);  // 700 - 300 + 600 = 1000
    OT_REJECT(e.check(kB, kSell, 100, 301, 0, 0), Reject::gross_position);
    // Reducing orders shrink the gross and flips count at their new size.
    OT_REJECT(e.check(kA, kSell, 100, 400, 0, 0), Reject::none);   // A -> 0: 700 - 400 + 0
    OT_REJECT(e.check(kA, kSell, 100, 500, 0, 0), Reject::none);   // A -> -100: 700 - 400 + 100
    OT_REJECT(e.check(kA, kSell, 100, 1100, 0, 0), Reject::none);  // A -> -700: 700 - 400 + 700
    OT_REJECT(e.check(kA, kSell, 100, 1101, 0, 0), Reject::gross_position);
}

OT_TEST(gross_position_counts_open_orders_and_releases_them) {
    Limits l = wide();
    l.max_gross_position = 1000;
    RiskEngine e(l, 4);
    e.on_fill(kA, kBuy, 400, 100);
    e.on_fill(kB, kSell, 300, 100);

    // Working buy 200 in A: exposure max(|400 + 200|, |400|) = 600, gross 600 + 300 = 900.
    e.on_order_open(kA, kBuy, 200);
    OT_CHECK_EQ(e.gross_exposure(), 900);
    // A working sell of 300 does not change it: max(600, |400 - 300|) = 600.
    e.on_order_open(kA, kSell, 300);
    OT_CHECK_EQ(e.gross_exposure(), 900);
    // buy 100 more in A: leg 400 + 200 + 100 = 700 -> 900 - 600 + 700 = 1000 passes, 101 fails.
    OT_REJECT(e.check(kA, kBuy, 100, 100, 0, 0), Reject::none);
    OT_REJECT(e.check(kA, kBuy, 100, 101, 0, 0), Reject::gross_position);

    e.on_order_closed(kA, kBuy, 200);
    e.on_order_closed(kA, kSell, 300);
    OT_CHECK_EQ(e.gross_exposure(), 700);
    OT_REJECT(e.check(kA, kBuy, 100, 300, 0, 0), Reject::none);
}

// Fills are facts and are never refused, so a book can end up beyond a cap. The check judges
// the worst case after the order literally (contract 4.4): there is no reduce-only exception,
// and a reducing order passes only once it brings the book back to the cap.
OT_TEST(caps_are_judged_on_the_state_after_the_order_even_when_already_exceeded) {
    Limits l = wide();
    l.max_position = 100;
    RiskEngine e(l, 4);
    e.on_fill(kA, kBuy, 150, 1000);
    OT_REJECT(chk(e, kSell, 1000, 10), Reject::position);  // 140 > 100
    OT_REJECT(chk(e, kSell, 1000, 50), Reject::none);      // 100 meets the cap

    Limits g = wide();
    g.max_gross_position = 200;
    RiskEngine f(g, 4);
    f.on_fill(kA, kSell, 150, 1000);
    f.on_order_open(kA, kSell, 100);  // worst case |-150 - 100| = 250, already beyond 200
    OT_CHECK_EQ(f.gross_exposure(), 250);
    // A buy cuts the buy leg to 50 but not the sell leg, so the worst case stays 250.
    OT_REJECT(chk(f, kBuy, 1000, 100), Reject::gross_position);
    f.on_order_closed(kA, kSell, 100);
    OT_REJECT(chk(f, kBuy, 1000, 100), Reject::none);
}

OT_TEST(gross_position_disabled_when_zero) {
    RiskEngine e(wide(), 4);
    e.on_fill(kA, kBuy, 900'000, 100);
    e.on_fill(kB, kBuy, 900'000, 100);
    OT_REJECT(e.check(kA, kBuy, 100, 900'000, 0, 0), Reject::none);
}

OT_TEST(price_band_boundaries) {
    Limits l = wide();
    l.price_band_bps = 100;  // 1 %
    RiskEngine e(l, 4);

    const Price ref = 1'000'000;  // allowed |diff| = 1'000'000 * 100 / 10'000 = 10'000
    OT_REJECT(chk(e, kBuy, ref, 1, ref), Reject::none);
    OT_REJECT(chk(e, kBuy, 1'010'000, 1, ref), Reject::none);  // exactly at the band
    OT_REJECT(chk(e, kBuy, 1'010'001, 1, ref), Reject::price_band);
    OT_REJECT(chk(e, kSell, 990'000, 1, ref), Reject::none);
    OT_REJECT(chk(e, kSell, 989'999, 1, ref), Reject::price_band);

    // A band that is not a whole number of price units: 999 * 100 / 10'000 = 9.99, so 9 passes
    // (90'000 <= 99'900) and 10 fails (100'000 > 99'900).
    OT_REJECT(chk(e, kBuy, 1008, 1, 999), Reject::none);
    OT_REJECT(chk(e, kBuy, 1009, 1, 999), Reject::price_band);
    OT_REJECT(chk(e, kBuy, 990, 1, 999), Reject::none);
    OT_REJECT(chk(e, kBuy, 989, 1, 999), Reject::price_band);

    // 200 %: reference 100 allows up to 300 above, and everything positive below.
    l.price_band_bps = 20'000;
    RiskEngine wide_band(l, 4);
    OT_REJECT(chk(wide_band, kBuy, 300, 1, 100), Reject::none);
    OT_REJECT(chk(wide_band, kBuy, 301, 1, 100), Reject::price_band);
    OT_REJECT(chk(wide_band, kBuy, 1, 1, 100), Reject::none);
}

OT_TEST(price_band_reference_handling) {
    Limits l = wide();
    l.price_band_bps = 100;
    {
        RiskEngine e(l, 4);  // reference optional: unknown reference lets the order through
        OT_REJECT(chk(e, kBuy, 1, 1, 0), Reject::none);
        OT_REJECT(chk(e, kBuy, 999'999'999, 1, 0), Reject::none);
        OT_REJECT(chk(e, kBuy, 100, 1, -5), Reject::none);  // negative counts as unknown
    }
    {
        l.require_reference = true;
        RiskEngine e(l, 4);
        OT_REJECT(chk(e, kBuy, 100, 1, 0), Reject::no_reference);
        OT_REJECT(chk(e, kBuy, 100, 1, -5), Reject::no_reference);
        OT_REJECT(chk(e, kBuy, 100, 1, 100), Reject::none);
        OT_REJECT(chk(e, kBuy, 102, 1, 100), Reject::price_band);
    }
    {
        // require_reference is meaningless while the band itself is off.
        l.price_band_bps = 0;
        RiskEngine e(l, 4);
        OT_REJECT(chk(e, kBuy, 100, 1, 0), Reject::none);
        OT_REJECT(chk(e, kBuy, 1, 1, 1'000'000), Reject::none);
    }
    {
        // Values whose products do not fit in 64 bits.
        l.price_band_bps = std::numeric_limits<std::uint32_t>::max();
        RiskEngine e(l, 4);
        OT_REJECT(chk(e, kBuy, kI64Max, 1, 1), Reject::price_band);
        OT_REJECT(chk(e, kBuy, 1, 1, kI64Max), Reject::none);
        // 1 bp of INT64_MAX is floor(9'223'372'036'854'775'807 / 10'000) = 922'337'203'685'477.
        l.price_band_bps = 1;
        RiskEngine tight(l, 4);
        OT_REJECT(chk(tight, kBuy, kI64Max, 1, kI64Max), Reject::none);
        OT_REJECT(chk(tight, kBuy, kI64Max - 922'337'203'685'477, 1, kI64Max), Reject::none);
        OT_REJECT(chk(tight, kBuy, kI64Max - 922'337'203'685'478, 1, kI64Max), Reject::price_band);
    }
}

// The engine compares |p - r| * 10'000 with r * bps. Here the boundary is derived the other
// way round, r + floor(r * bps / 10'000), so an off-by-one in either formulation shows up.
OT_TEST(price_band_random_boundaries) {
    Rng rng(0xBA4Du);
    for (int i = 0; i < 4000; ++i) {
        Limits l = wide();
        l.price_band_bps = static_cast<std::uint32_t>(1 + rng.bounded(5000));
        RiskEngine e(l, 2);
        const Price ref = 1 + static_cast<Price>(rng.bounded(5'000'000'000ULL));
        const Price allowed = static_cast<Price>(I128{ref} * l.price_band_bps / 10'000);
        OT_REJECT(chk(e, kBuy, ref + allowed, 1, ref), Reject::none);
        OT_REJECT(chk(e, kBuy, ref + allowed + 1, 1, ref), Reject::price_band);
        OT_REJECT(chk(e, kSell, ref - allowed, 1, ref), Reject::none);  // allowed < ref/2: price > 0
        OT_REJECT(chk(e, kSell, ref - allowed - 1, 1, ref),
                  ref - allowed - 1 > 0 ? Reject::price_band : Reject::invalid_order);
    }
}

// ---------------------------------------------------------------------------------------------
// Rate limit
// ---------------------------------------------------------------------------------------------

OT_TEST(rate_limit_window_edge) {
    Limits l = wide();
    l.max_orders_per_second = 3;
    RiskEngine e(l, 4);
    const Nanos t0 = 5 * kSec;

    OT_REJECT(chk(e, kBuy, 100, 1, 0, t0), Reject::none);
    OT_REJECT(chk(e, kBuy, 100, 1, 0, t0 + 100), Reject::none);
    OT_REJECT(chk(e, kBuy, 100, 1, 0, t0 + 200), Reject::none);
    OT_REJECT(chk(e, kBuy, 100, 1, 0, t0 + 300), Reject::rate_limit);
    // The first admission is still inside the window one nanosecond before it turns 1 s old...
    OT_REJECT(chk(e, kBuy, 100, 1, 0, t0 + kSec - 1), Reject::rate_limit);
    // ...and has left it at exactly 1'000'000'000 ns.
    OT_REJECT(chk(e, kBuy, 100, 1, 0, t0 + kSec), Reject::none);
    // Window now {t0+100, t0+200, t0+1e9}: the oldest expires at t0+100+1e9.
    OT_REJECT(chk(e, kBuy, 100, 1, 0, t0 + kSec + 99), Reject::rate_limit);
    OT_REJECT(chk(e, kBuy, 100, 1, 0, t0 + kSec + 100), Reject::none);
    OT_REJECT(chk(e, kBuy, 100, 1, 0, t0 + kSec + 199), Reject::rate_limit);
    OT_REJECT(chk(e, kBuy, 100, 1, 0, t0 + kSec + 200), Reject::none);
}

OT_TEST(rate_limit_edge_cases) {
    Limits l = wide();
    l.max_orders_per_second = 1;
    {
        RiskEngine e(l, 4);  // an admission at timestamp 0 is a real admission
        OT_REJECT(chk(e, kBuy, 100, 1, 0, 0), Reject::none);
        OT_REJECT(chk(e, kBuy, 100, 1, 0, 0), Reject::rate_limit);
        OT_REJECT(chk(e, kBuy, 100, 1, 0, kSec - 1), Reject::rate_limit);
        OT_REJECT(chk(e, kBuy, 100, 1, 0, kSec), Reject::none);
    }
    {
        Limits three = l;  // several orders in the same nanosecond share one expiry
        three.max_orders_per_second = 3;
        RiskEngine t(three, 4);
        for (int i = 0; i < 3; ++i) OT_REJECT(chk(t, kBuy, 100, 1, 0, 7), Reject::none);
        OT_REJECT(chk(t, kBuy, 100, 1, 0, 7), Reject::rate_limit);
        OT_REJECT(chk(t, kBuy, 100, 1, 0, 7 + kSec - 1), Reject::rate_limit);
        for (int i = 0; i < 3; ++i) OT_REJECT(chk(t, kBuy, 100, 1, 0, 7 + kSec), Reject::none);
        OT_REJECT(chk(t, kBuy, 100, 1, 0, 7 + kSec), Reject::rate_limit);
    }
    {
        // A clock that steps backwards is treated as no time elapsed, never as a fresh window.
        RiskEngine e(l, 4);
        OT_REJECT(chk(e, kBuy, 100, 1, 0, 5 * kSec), Reject::none);
        OT_REJECT(chk(e, kBuy, 100, 1, 0, 1 * kSec), Reject::rate_limit);
        OT_REJECT(chk(e, kBuy, 100, 1, 0, 6 * kSec - 1), Reject::rate_limit);
        OT_REJECT(chk(e, kBuy, 100, 1, 0, 6 * kSec), Reject::none);
    }
    {
        // Disabled: no state, no limit.
        RiskEngine e(wide(), 4);
        for (int i = 0; i < 1000; ++i) OT_REJECT(chk(e, kBuy, 100, 1, 0, 0), Reject::none);
    }
}

// A step back in time is recorded as "no time has passed", so the ring stays ordered and its
// oldest entry stays the one that expires first.
OT_TEST(rate_limit_survives_a_clock_step_back) {
    Limits l = wide();
    l.max_orders_per_second = 2;
    RiskEngine e(l, 4);
    OT_REJECT(chk(e, kBuy, 100, 1, 0, 10 * kSec), Reject::none);
    OT_REJECT(chk(e, kBuy, 100, 1, 0, 1 * kSec), Reject::none);  // room in the ring: admitted...
    // ...but stamped 10 s, so both count as the current second.
    OT_REJECT(chk(e, kBuy, 100, 1, 0, 10 * kSec + kSec / 2), Reject::rate_limit);
    OT_REJECT(chk(e, kBuy, 100, 1, 0, 11 * kSec), Reject::none);
    // The other 10 s entry is now the oldest and is 0.9 s old; a step back does not age it.
    OT_REJECT(chk(e, kBuy, 100, 1, 0, 10 * kSec + 900'000'000), Reject::rate_limit);
    OT_REJECT(chk(e, kBuy, 100, 1, 0, 11 * kSec), Reject::none);
    OT_REJECT(chk(e, kBuy, 100, 1, 0, 11 * kSec), Reject::rate_limit);
    // Stepping back while the ring is full must not open a window either.
    OT_REJECT(chk(e, kBuy, 100, 1, 0, 2 * kSec), Reject::rate_limit);
}

OT_TEST(rejected_orders_do_not_consume_rate_budget) {
    Limits l = wide();
    l.max_order_qty = 10;
    l.max_order_notional = 5000;
    l.max_position = 50;
    l.max_gross_position = 62;
    l.price_band_bps = 100;
    l.require_reference = true;
    l.max_orders_per_second = 1;
    RiskEngine e(l, 4);
    e.on_fill(kA, kBuy, 48, 1000);  // A long 48
    e.on_fill(kB, kBuy, 10, 1000);  // B long 10 -> gross 58

    OT_REJECT(e.check(kA, kBuy, 1000, 0, 1000, 0), Reject::invalid_order);
    OT_REJECT(e.check(kA, kBuy, 1000, 11, 1000, 0), Reject::order_qty);
    OT_REJECT(e.check(kC, kBuy, 1000, 6, 1000, 0), Reject::order_notional);  // 6000 > 5000
    OT_REJECT(e.check(kA, kBuy, 1000, 5, 1000, 0), Reject::position);        // 53 > 50
    OT_REJECT(e.check(kB, kBuy, 1000, 5, 1000, 0), Reject::gross_position);  // 58 - 10 + 15 = 63
    OT_REJECT(e.check(kC, kBuy, 1011, 4, 1000, 0), Reject::price_band);      // 1.1 % > 1 %
    OT_REJECT(e.check(kC, kBuy, 1000, 4, 0, 0), Reject::no_reference);
    e.set_kill_switch(true);
    OT_REJECT(e.check(kC, kBuy, 1000, 4, 1000, 0), Reject::kill_switch);
    e.set_kill_switch(false);

    // None of the above used the single slot of the window.
    OT_REJECT(e.check(kC, kBuy, 1000, 4, 1000, 0), Reject::none);
    OT_REJECT(e.check(kC, kBuy, 1000, 4, 1000, 0), Reject::rate_limit);
}

// The rate step is evaluated before open_orders, but an order that fails the later step must
// still leave the window untouched.
OT_TEST(order_rejected_after_the_rate_step_does_not_consume_budget) {
    Limits l = wide();
    l.max_orders_per_second = 1;
    l.max_open_orders = 1;
    RiskEngine e(l, 4);
    e.on_order_open(kA, kBuy, 10);
    OT_REJECT(chk(e, kBuy, 100, 1, 0, 0), Reject::open_orders);
    OT_REJECT(chk(e, kBuy, 100, 1, 0, 0), Reject::open_orders);  // rate_limit if the first had counted
    e.on_order_closed(kA, kBuy, 10);
    OT_REJECT(chk(e, kBuy, 100, 1, 0, 1), Reject::none);
    OT_REJECT(chk(e, kBuy, 100, 1, 0, 2), Reject::rate_limit);
}

// Differential test: a naive model keeps every admission and counts those younger than 1 s.
// Gaps are chosen to land on and next to the window edge, and a quarter of the orders are
// rejected by an earlier check, which must not enter the model's history.
OT_TEST(rate_limit_matches_a_naive_sliding_window) {
    const std::uint32_t limits[] = {1, 2, 3, 7, 50};
    std::uint64_t seed = 100;
    for (const std::uint32_t n : limits) {
        Limits l = wide();
        l.max_order_qty = 10;
        l.max_orders_per_second = n;
        RiskEngine e(l, 2);
        Rng rng(seed++);
        std::vector<Nanos> admitted;
        Nanos now = rng.bounded(3) * kSec;
        int mismatches = 0;
        std::uint64_t admits = 0;
        std::uint64_t rate_rejects = 0;
        for (int step = 0; step < 20'000 && mismatches == 0; ++step) {
            switch (rng.bounded(8)) {
                case 0: break;                                      // same timestamp
                case 1: now += 1; break;
                case 2: now += rng.bounded(1000); break;
                case 3: now += kSec / n + rng.bounded(3) - 1; break;
                case 4:
                case 5:
                    // Aim at the model's window edge: the moment the oldest of the last n
                    // admissions turns exactly 1 s old, give or take one nanosecond.
                    if (admitted.size() >= n) {
                        const Nanos edge = admitted[admitted.size() - n] + kSec + rng.bounded(3) - 1;
                        if (edge > now) now = edge;
                    }
                    break;
                case 6: now += rng.bounded(kSec / (2 * n) + 1); break;
                default:  // an occasional long pause, rarer for big limits so their window fills
                    if (rng.chance(1, n)) now += rng.bounded(2 * kSec);
                    break;
            }
            const bool oversized = rng.chance(1, 4);
            const Qty qty = oversized ? 11 : static_cast<Qty>(1 + rng.bounded(10));
            const Reject got = e.check(kA, kBuy, 100, qty, 0, now);

            Reject want = Reject::order_qty;
            if (!oversized) {
                std::uint32_t in_window = 0;
                for (auto it = admitted.rbegin(); it != admitted.rend() && now - *it < kSec; ++it) {
                    ++in_window;
                }
                want = in_window < n ? Reject::none : Reject::rate_limit;
                if (want == Reject::none) {
                    admitted.push_back(now);
                    ++admits;
                } else {
                    ++rate_rejects;
                }
            }
            if (got != want) {
                std::fprintf(stderr, "  n=%u step=%d now=%llu got=%s want=%s\n", n, step,
                             static_cast<unsigned long long>(now), risk::to_string(got),
                             risk::to_string(want));
                ++mismatches;
            }
        }
        OT_CHECK_EQ(mismatches, 0);
        OT_CHECK(admits > 100);        // the model is exercised, not vacuously rejecting
        OT_CHECK(rate_rejects > 100);
    }
}

// ---------------------------------------------------------------------------------------------
// Open orders
// ---------------------------------------------------------------------------------------------

OT_TEST(open_orders_limit) {
    Limits l = wide();
    l.max_open_orders = 2;
    RiskEngine e(l, 4);
    OT_REJECT(chk(e, kBuy, 100, 1), Reject::none);
    e.on_order_open(kA, kBuy, 10);
    OT_CHECK_EQ(e.open_orders(), 1u);
    OT_REJECT(chk(e, kBuy, 100, 1), Reject::none);
    e.on_order_open(kB, kSell, 10);  // the count spans instruments and sides
    OT_CHECK_EQ(e.open_orders(), 2u);
    OT_REJECT(chk(e, kBuy, 100, 1), Reject::open_orders);
    OT_REJECT(e.check(kC, kSell, 100, 1, 0, 0), Reject::open_orders);
    e.on_order_closed(kA, kBuy, 10);
    OT_CHECK_EQ(e.open_orders(), 1u);
    OT_REJECT(chk(e, kBuy, 100, 1), Reject::none);

    RiskEngine unlimited(wide(), 4);
    for (int i = 0; i < 100; ++i) unlimited.on_order_open(kA, kBuy, 1);
    OT_CHECK_EQ(unlimited.open_orders(), 100u);
    OT_REJECT(chk(unlimited, kBuy, 100, 1), Reject::none);
}

// ---------------------------------------------------------------------------------------------
// First failing check wins
// ---------------------------------------------------------------------------------------------

namespace {

enum Violation : unsigned {
    kVKill,
    kVInvalid,
    kVQty,
    kVNotional,
    kVPosition,
    kVGross,
    kVBand,
    kVNoRef,
    kVRate,
    kVOpen,
    kVCount
};

constexpr Reject kReasonOf[kVCount] = {
    Reject::kill_switch, Reject::invalid_order, Reject::order_qty, Reject::order_notional,
    Reject::position,    Reject::gross_position, Reject::price_band, Reject::no_reference,
    Reject::rate_limit,  Reject::open_orders,
};

constexpr unsigned bit(unsigned v) { return 1u << v; }

// One fixed order (locate A, buy 10 @ 10'000, reference 10'000) that passes every check under
// the baseline limits. Each violation is switched on by tightening exactly the knob its
// check reads, so the violations are independent and any subset can be combined. Rate and
// open-order violations need engine state, which is set up before the kill switch is thrown.
Reject run_with(unsigned mask) {
    constexpr Price kPx = 10'000;
    constexpr Qty kQty = 10;
    constexpr Nanos kNow = 3 * kSec;
    Limits l = wide();
    l.price_band_bps = 100;
    Locate locate = kA;
    Price ref = kPx;
    if (mask & bit(kVQty)) l.max_order_qty = 5;
    if (mask & bit(kVNotional)) l.max_order_notional = 50'000;  // order is 100'000
    if (mask & bit(kVPosition)) l.max_position = 5;
    if (mask & bit(kVGross)) l.max_gross_position = 5;
    if (mask & bit(kVBand)) ref = 20'000;
    if (mask & bit(kVNoRef)) {
        ref = 0;
        l.require_reference = true;
    }
    if (mask & bit(kVRate)) l.max_orders_per_second = 1;
    if (mask & bit(kVOpen)) l.max_open_orders = 1;
    if (mask & bit(kVInvalid)) locate = 9;

    RiskEngine e(l, 4);
    if (mask & bit(kVRate)) {
        // Use up the single slot with a small order on another instrument.
        if (e.check(kB, kBuy, kPx, 1, kPx, kNow) != Reject::none) return Reject::kill_switch;
    }
    if (mask & bit(kVOpen)) e.on_order_open(kB, kSell, 1);
    if (mask & bit(kVKill)) e.set_kill_switch(true);
    return e.check(locate, kBuy, kPx, kQty, ref, kNow);
}

}  // namespace

OT_TEST(baseline_order_passes) { OT_REJECT(run_with(0), Reject::none); }

OT_TEST(each_reason_in_isolation) {
    for (unsigned v = 0; v < kVCount; ++v) {
        const Reject got = run_with(bit(v));
        if (got != kReasonOf[v]) std::fprintf(stderr, "  violation %u: got %s\n", v, risk::to_string(got));
        OT_CHECK(got == kReasonOf[v]);
    }
}

OT_TEST(first_failing_check_wins_for_every_pair) {
    for (unsigned i = 0; i < kVCount; ++i) {
        for (unsigned j = i + 1; j < kVCount; ++j) {
            if (i == kVBand && j == kVNoRef) continue;  // one step, mutually exclusive inputs
            const Reject got = run_with(bit(i) | bit(j));
            if (got != kReasonOf[i]) {
                std::fprintf(stderr, "  pair (%u,%u): got %s\n", i, j, risk::to_string(got));
            }
            OT_CHECK(got == kReasonOf[i]);
        }
    }
}

// Peeling violations off from the front reveals the next check in the documented order.
OT_TEST(violations_surface_in_the_documented_order) {
    for (const unsigned skipped : {static_cast<unsigned>(kVNoRef), static_cast<unsigned>(kVBand)}) {
        for (unsigned s = 0; s < kVCount; ++s) {
            if (s == skipped) continue;
            unsigned mask = 0;
            for (unsigned k = s; k < kVCount; ++k) {
                if (k != skipped) mask |= bit(k);
            }
            const Reject got = run_with(mask);
            if (got != kReasonOf[s]) {
                std::fprintf(stderr, "  suffix from %u (skip %u): got %s\n", s, skipped,
                             risk::to_string(got));
            }
            OT_CHECK(got == kReasonOf[s]);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Position accounting, hand-computed
// ---------------------------------------------------------------------------------------------

namespace {
void expect_position(const RiskEngine& e, Locate l, std::int64_t qty, Price avg,
                     std::int64_t realized) {
    OT_CHECK_EQ(e.position(l).qty, qty);
    OT_CHECK_EQ(e.position(l).avg_price, avg);
    OT_CHECK_EQ(e.position(l).realized, realized);
}
}  // namespace

OT_TEST(long_lifecycle_open_add_reduce_close) {
    RiskEngine e(wide(), 4);
    expect_position(e, kA, 0, 0, 0);

    e.on_fill(kA, kBuy, 100, 10'000);  // open
    expect_position(e, kA, 100, 10'000, 0);

    // add 50 @ 10'030: (10'000*100 + 10'030*50) / 150 = 1'501'500 / 150 = 10'010 exactly
    e.on_fill(kA, kBuy, 50, 10'030);
    expect_position(e, kA, 150, 10'010, 0);

    // add 30 @ 10'001: (10'010*150 + 10'001*30) / 180 = 1'801'530 / 180 = 10'008.5 -> 10'008
    e.on_fill(kA, kBuy, 30, 10'001);
    expect_position(e, kA, 180, 10'008, 0);

    // sell 80 @ 10'100: realized (10'100 - 10'008) * 80 = 92 * 80 = 7'360; average is unchanged
    e.on_fill(kA, kSell, 80, 10'100);
    expect_position(e, kA, 100, 10'008, 7'360);

    // sell 100 @ 9'998 closes it: (9'998 - 10'008) * 100 = -1'000 -> 6'360, flat, average reset
    e.on_fill(kA, kSell, 100, 9'998);
    expect_position(e, kA, 0, 0, 6'360);
    OT_CHECK_EQ(e.realized_pnl(), 6'360);
    OT_CHECK_EQ(e.unrealized_pnl(), 0);
    OT_CHECK_EQ(e.total_pnl(), 6'360);

    // Re-opening after a close starts from the new fill price, not the old average.
    e.on_fill(kA, kBuy, 10, 20'000);
    expect_position(e, kA, 10, 20'000, 6'360);
}

OT_TEST(flip_long_to_short_in_one_fill) {
    RiskEngine e(wide(), 4);
    e.on_fill(kB, kBuy, 100, 10'000);
    // sell 250 @ 10'200: closes 100 -> (10'200 - 10'000) * 100 = 20'000; the other 150 open a
    // short at the fill price.
    e.on_fill(kB, kSell, 250, 10'200);
    expect_position(e, kB, -150, 10'200, 20'000);

    // cover 60 @ 10'100: (avg - fill) * 60 = 100 * 60 = 6'000 -> 26'000; still short 90
    e.on_fill(kB, kBuy, 60, 10'100);
    expect_position(e, kB, -90, 10'200, 26'000);

    // cover 90 @ 10'300: (10'200 - 10'300) * 90 = -9'000 -> 17'000; flat
    e.on_fill(kB, kBuy, 90, 10'300);
    expect_position(e, kB, 0, 0, 17'000);
}

OT_TEST(flip_short_to_long_and_exact_close_boundary) {
    RiskEngine e(wide(), 4);
    e.on_fill(kA, kSell, 50, 100'000);
    // buy 80 @ 99'000: closes 50 -> (100'000 - 99'000) * 50 = 50'000; long 30 @ 99'000
    e.on_fill(kA, kBuy, 80, 99'000);
    expect_position(e, kA, 30, 99'000, 50'000);

    // Selling exactly the position is a close, not a flip: no phantom short, average reset.
    e.on_fill(kB, kBuy, 100, 10'000);
    e.on_fill(kB, kSell, 100, 10'000);
    expect_position(e, kB, 0, 0, 0);
    // One share more is a flip: short 1 at the fill price.
    e.on_fill(kC, kBuy, 100, 10'000);
    e.on_fill(kC, kSell, 101, 10'050);
    expect_position(e, kC, -1, 10'050, 5'000);  // (10'050 - 10'000) * 100
}

OT_TEST(short_positions_average_and_realize_with_the_right_sign) {
    RiskEngine e(wide(), 4);
    // Averaging into a short: (10'000*100 + 10'003*100) / 200 = 2'000'300 / 200 = 10'001.5 -> 10'001
    e.on_fill(kC, kSell, 100, 10'000);
    e.on_fill(kC, kSell, 100, 10'003);
    expect_position(e, kC, -200, 10'001, 0);
    // (10'001*200 + 10'000*1) / 201 = 2'010'200 / 201 = 10'000.995 -> 10'000 (truncation)
    e.on_fill(kC, kSell, 1, 10'000);
    expect_position(e, kC, -201, 10'000, 0);

    // A short profits when the price falls: sell 10 @ 500, buy 10 @ 450 -> +50 * 10.
    e.on_fill(kA, kSell, 10, 500);
    e.on_fill(kA, kBuy, 10, 450);
    expect_position(e, kA, 0, 0, 500);
    // ...and loses when it rises: sell 10 @ 500, buy 10 @ 550 -> -50 * 10.
    e.on_fill(kB, kSell, 10, 500);
    e.on_fill(kB, kBuy, 10, 550);
    expect_position(e, kB, 0, 0, -500);
    OT_CHECK_EQ(e.realized_pnl(), 0);
}

OT_TEST(invalid_fills_are_ignored) {
    RiskEngine e(wide(), 4);
    e.on_fill(kA, kBuy, 100, 10'000);
    e.on_fill(kA, kBuy, 0, 10'000);
    e.on_fill(kA, kBuy, 10, 0);
    e.on_fill(kA, kBuy, 10, -5);
    e.on_fill(kA, static_cast<Side>(2), 10, 10'000);
    expect_position(e, kA, 100, 10'000, 0);
}

OT_TEST(unrealized_pnl_follows_the_marks) {
    RiskEngine e(wide(), 4);
    e.on_fill(kA, kBuy, 100, 10'008);
    OT_CHECK_EQ(e.unrealized_pnl(), 0);  // never marked: valued at cost

    e.mark(kA, 10'058);  // (10'058 - 10'008) * 100
    OT_CHECK_EQ(e.unrealized_pnl(), 5'000);
    e.mark(kA, 9'908);   // (9'908 - 10'008) * 100
    OT_CHECK_EQ(e.unrealized_pnl(), -10'000);

    e.on_fill(kB, kSell, 90, 10'200);
    e.mark(kB, 10'150);  // (10'150 - 10'200) * -90 = +4'500
    OT_CHECK_EQ(e.unrealized_pnl(), -5'500);
    OT_CHECK_EQ(e.total_pnl(), -5'500);

    // Non-positive marks are not prices: they are ignored.
    e.mark(kA, 0);
    e.mark(kA, -1);
    OT_CHECK_EQ(e.unrealized_pnl(), -5'500);

    // Partial close of A: sell 40 @ 10'000 realizes (10'000 - 10'008) * 40 = -320, and the
    // remaining 60 are valued at the last mark: (9'908 - 10'008) * 60 = -6'000.
    e.on_fill(kA, kSell, 40, 10'000);
    OT_CHECK_EQ(e.realized_pnl(), -320);
    OT_CHECK_EQ(e.unrealized_pnl(), -6'000 + 4'500);
    OT_CHECK_EQ(e.total_pnl(), -320 - 1'500);

    // Flipping keeps using the same mark: sell 100 @ 9'950 closes 60 (realized (9'950 - 10'008) * 60
    // = -3'480) and opens short 40 @ 9'950, valued at 9'908: (9'908 - 9'950) * -40 = +1'680.
    e.on_fill(kA, kSell, 100, 9'950);
    OT_CHECK_EQ(e.realized_pnl(), -320 - 3'480);
    OT_CHECK_EQ(e.unrealized_pnl(), 1'680 + 4'500);

    // Closing everything leaves no unrealized PnL, and marking a flat book is harmless.
    e.on_fill(kA, kBuy, 40, 9'900);
    e.on_fill(kB, kBuy, 90, 10'150);
    e.mark(kA, 12'345);
    OT_CHECK_EQ(e.unrealized_pnl(), 0);
    OT_CHECK_EQ(e.total_pnl(), e.realized_pnl());
}

OT_TEST(max_drawdown_known_answer) {
    RiskEngine e(wide(), 4);
    e.on_fill(kA, kBuy, 100, 10'000);
    OT_CHECK_EQ(e.max_drawdown(), 0);
    e.mark(kA, 10'100);  // total +10'000, peak 10'000
    OT_CHECK_EQ(e.max_drawdown(), 0);
    e.mark(kA, 9'900);   // -10'000: 20'000 below the peak
    OT_CHECK_EQ(e.max_drawdown(), 20'000);
    e.mark(kA, 10'050);  // +5'000
    e.mark(kA, 9'950);   // -5'000: 15'000 below the peak, not a new maximum
    OT_CHECK_EQ(e.max_drawdown(), 20'000);
    e.mark(kA, 10'300);  // +30'000: new peak
    e.mark(kA, 10'200);  // +20'000: 10'000 below it
    OT_CHECK_EQ(e.max_drawdown(), 20'000);
    e.mark(kA, 9'800);   // -20'000: 50'000 below the peak of 30'000
    OT_CHECK_EQ(e.max_drawdown(), 50'000);
    e.mark(kA, 10'000);  // recovering never shrinks it
    OT_CHECK_EQ(e.max_drawdown(), 50'000);
}

OT_TEST(max_drawdown_counts_an_opening_loss_and_realized_losses) {
    RiskEngine a(wide(), 4);
    a.on_fill(kA, kBuy, 10, 1000);
    a.mark(kA, 990);  // -100 against the starting peak of 0
    OT_CHECK_EQ(a.max_drawdown(), 100);

    RiskEngine b(wide(), 4);
    b.on_fill(kA, kBuy, 100, 10'000);
    b.on_fill(kA, kSell, 100, 9'000);  // realized -100'000 at a fill
    OT_CHECK_EQ(b.max_drawdown(), 100'000);
    b.on_fill(kA, kBuy, 100, 9'000);
    b.on_fill(kA, kSell, 100, 12'000);  // +300'000: a recovery
    OT_CHECK_EQ(b.total_pnl(), 200'000);
    OT_CHECK_EQ(b.max_drawdown(), 100'000);
}

OT_TEST(loss_limit_trips_the_kill_switch) {
    Limits l = wide();
    l.max_loss = 5'000;
    RiskEngine e(l, 4);
    e.on_fill(kA, kBuy, 100, 10'000);
    e.mark(kA, 9'960);  // -4'000
    OT_CHECK(!e.kill_switch());
    e.mark(kA, 9'951);  // -4'900
    OT_CHECK(!e.kill_switch());
    OT_REJECT(chk(e, kBuy, 9'951, 1), Reject::none);
    e.mark(kA, 9'950);  // -5'000: the limit itself trips it
    OT_CHECK(e.kill_switch());
    OT_REJECT(chk(e, kBuy, 9'950, 1), Reject::kill_switch);

    // Recovery alone does not lift a trip.
    e.mark(kA, 10'100);
    OT_CHECK(e.kill_switch());
    e.set_kill_switch(false);
    OT_REJECT(chk(e, kBuy, 10'100, 1), Reject::none);
    // Below the limit again, the next mark trips it again.
    e.mark(kA, 9'900);
    OT_CHECK(e.kill_switch());
}

OT_TEST(loss_limit_trips_on_realized_and_combined_loss) {
    Limits l = wide();
    l.max_loss = 5'000;
    {
        RiskEngine e(l, 4);
        e.on_fill(kA, kBuy, 100, 10'000);
        e.on_fill(kA, kSell, 100, 9'951);  // realized -4'900
        OT_CHECK(!e.kill_switch());
        e.on_fill(kB, kBuy, 1, 100);
        e.on_fill(kB, kSell, 1, 99);       // another -1 -> -4'901
        OT_CHECK(!e.kill_switch());
        e.on_fill(kB, kBuy, 1, 100);
        e.on_fill(kB, kSell, 1, 1);        // -99 -> -5'000: trips inside on_fill
        OT_CHECK(e.kill_switch());
    }
    {
        RiskEngine e(l, 4);
        e.on_fill(kA, kBuy, 100, 10'000);
        e.on_fill(kA, kSell, 50, 9'940);   // realized -60 * 50 = -3'000
        e.mark(kA, 9'980);                 // 50 left: -20 * 50 = -1'000 -> total -4'000
        OT_CHECK(!e.kill_switch());
        e.mark(kA, 9'960);                 // -40 * 50 = -2'000 -> total -5'000
        OT_CHECK(e.kill_switch());
    }
    {
        Limits off = wide();
        RiskEngine e(off, 4);  // max_loss 0: disabled
        e.on_fill(kA, kBuy, 100'000, 10'000);
        e.mark(kA, 1);
        OT_CHECK(!e.kill_switch());
        off.max_loss = -5'000;  // a negative limit is also "disabled", never "always tripped"
        RiskEngine n(off, 4);
        n.on_fill(kA, kBuy, 100, 10'000);
        n.mark(kA, 9'000);
        OT_CHECK(!n.kill_switch());
    }
}

OT_TEST(out_of_range_locate_is_safe_everywhere) {
    RiskEngine e(wide(), 4);
    e.on_fill(kA, kBuy, 10, 100);
    for (const Locate bad : {Locate{4}, Locate{5}, Locate{1000}, Locate{65535}}) {
        e.on_fill(bad, kBuy, 100, 100);
        e.on_fill(bad, kSell, 100, 100);
        e.on_order_open(bad, kBuy, 10);
        e.on_order_closed(bad, kBuy, 10);
        e.mark(bad, 100);
        OT_CHECK_EQ(e.position(bad).qty, 0);
        OT_CHECK_EQ(e.position(bad).avg_price, 0);
        OT_CHECK_EQ(e.position(bad).realized, 0);
        OT_CHECK_EQ(e.open_qty(bad, kBuy), 0);
        OT_CHECK_EQ(e.open_qty(bad, kSell), 0);
        OT_REJECT(e.check(bad, kBuy, 100, 1, 0, 0), Reject::invalid_order);
    }
    OT_CHECK_EQ(e.open_orders(), 0u);
    OT_CHECK_EQ(e.gross_exposure(), 10);
    OT_CHECK_EQ(e.realized_pnl(), 0);
    OT_CHECK_EQ(e.unrealized_pnl(), 0);
    OT_CHECK_EQ(e.max_drawdown(), 0);
    expect_position(e, kA, 10, 100, 0);

    RiskEngine empty(wide(), 0);  // an engine with no table at all is still defined
    empty.on_fill(0, kBuy, 1, 1);
    empty.mark(0, 1);
    empty.on_order_open(0, kBuy, 1);
    empty.on_order_closed(0, kBuy, 1);
    OT_CHECK_EQ(empty.position(0).qty, 0);
    OT_CHECK_EQ(empty.open_orders(), 0u);
}

OT_TEST(invalid_arguments_to_bookkeeping_are_ignored) {
    RiskEngine e(wide(), 4);
    e.on_order_open(kA, static_cast<Side>(2), 10);
    e.on_order_open(kA, kBuy, 0);
    e.on_order_closed(kA, static_cast<Side>(2), 10);
    OT_CHECK_EQ(e.open_orders(), 0u);
    OT_CHECK_EQ(e.open_qty(kA, kBuy), 0);
    OT_CHECK_EQ(e.open_qty(kA, static_cast<Side>(2)), 0);
}

OT_TEST(open_order_bookkeeping_never_underflows) {
    RiskEngine e(wide(), 4);
    e.on_order_closed(kA, kBuy, 100);  // nothing open: must not wrap to a huge count or quantity
    OT_CHECK_EQ(e.open_qty(kA, kBuy), 0);
    OT_CHECK_EQ(e.open_orders(), 0u);

    e.on_order_open(kA, kBuy, 100);
    OT_CHECK_EQ(e.open_qty(kA, kBuy), 100);
    OT_CHECK_EQ(e.open_orders(), 1u);
    e.on_order_closed(kA, kBuy, 30);   // partial release keeps the order counted
    OT_CHECK_EQ(e.open_qty(kA, kBuy), 70);
    OT_CHECK_EQ(e.open_orders(), 1u);
    e.on_order_closed(kA, kBuy, 500);  // oversized release clamps at zero
    OT_CHECK_EQ(e.open_qty(kA, kBuy), 0);
    OT_CHECK_EQ(e.open_orders(), 0u);
    e.on_order_closed(kA, kBuy, 500);  // duplicate report
    e.on_order_closed(kA, kBuy, 0);
    OT_CHECK_EQ(e.open_qty(kA, kBuy), 0);
    OT_CHECK_EQ(e.open_orders(), 0u);
    OT_CHECK_EQ(e.gross_exposure(), 0);

    // Sides, instruments and the total are tracked separately.
    e.on_order_open(kA, kBuy, 10);
    e.on_order_open(kA, kSell, 20);
    e.on_order_open(kB, kBuy, 30);
    OT_CHECK_EQ(e.open_orders(), 3u);
    e.on_order_closed(kA, kSell, 20);
    OT_CHECK_EQ(e.open_qty(kA, kBuy), 10);
    OT_CHECK_EQ(e.open_qty(kA, kSell), 0);
    OT_CHECK_EQ(e.open_qty(kB, kBuy), 30);
    OT_CHECK_EQ(e.open_orders(), 2u);

    // Fills change the position only; releasing the working quantity is the caller's job.
    e.on_fill(kB, kBuy, 30, 100);
    OT_CHECK_EQ(e.open_qty(kB, kBuy), 30);
    e.on_order_closed(kB, kBuy, 30);
    OT_CHECK_EQ(e.open_qty(kB, kBuy), 0);
    OT_CHECK_EQ(e.open_orders(), 1u);
}

// With several working orders on one side the count cannot be told apart from the quantity
// alone; it errs on the high side and is exact again once the side is flat. It can never
// exceed the open share count.
OT_TEST(open_order_count_is_conservative_with_several_orders_per_side) {
    RiskEngine e(wide(), 4);
    for (int i = 0; i < 3; ++i) e.on_order_open(kA, kBuy, 10);
    OT_CHECK_EQ(e.open_orders(), 3u);
    e.on_order_closed(kA, kBuy, 10);  // true count is 2; reported 3
    OT_CHECK_EQ(e.open_qty(kA, kBuy), 20);
    OT_CHECK_EQ(e.open_orders(), 3u);
    e.on_order_closed(kA, kBuy, 20);
    OT_CHECK_EQ(e.open_orders(), 0u);

    for (int i = 0; i < 5; ++i) e.on_order_open(kA, kSell, 1);
    e.on_order_closed(kA, kSell, 3);  // 2 shares left cannot belong to more than 2 orders
    OT_CHECK_EQ(e.open_qty(kA, kSell), 2);
    OT_CHECK_EQ(e.open_orders(), 2u);
}

OT_TEST(extreme_values_saturate_instead_of_overflowing) {
    Limits l = wide();
    l.max_order_qty = kQtyMax;
    l.max_position = kI64Max;
    RiskEngine e(l, 4);
    e.on_fill(kA, kBuy, kQtyMax, kI64Max);
    expect_position(e, kA, kQtyMax, kI64Max, 0);
    e.mark(kA, kI64Max);
    OT_CHECK_EQ(e.unrealized_pnl(), 0);

    // (1 - INT64_MAX) * 4.29e9 is about -3.96e28: reported as the saturated -INT64_MAX.
    e.on_fill(kA, kSell, kQtyMax, 1);
    expect_position(e, kA, 0, 0, -kI64Max);
    OT_CHECK_EQ(e.realized_pnl(), -kI64Max);
    OT_CHECK_EQ(e.total_pnl(), -kI64Max);
    OT_CHECK_EQ(e.max_drawdown(), kI64Max);
    // Totals are the sum of what is stored per instrument, so later gains move a saturated
    // total by exactly their amount.
    e.on_fill(kA, kBuy, 10, 100);
    e.on_fill(kA, kSell, 10, 110);
    OT_CHECK_EQ(e.position(kA).realized, -kI64Max + 100);
    OT_CHECK_EQ(e.realized_pnl(), -kI64Max + 100);

    // Averaging huge positions does not overflow: with Q = 2^32 - 1 shares each,
    // (Q * (2^63 - 1) + Q * 1) / (2 * Q) = 2^62 exactly.
    e.on_fill(kB, kBuy, kQtyMax, kI64Max);
    e.on_fill(kB, kBuy, kQtyMax, 1);
    OT_CHECK_EQ(e.position(kB).qty, std::int64_t{kQtyMax} * 2);
    OT_CHECK_EQ(e.position(kB).avg_price, std::int64_t{1} << 62);
    e.mark(kB, 1);
    OT_CHECK(e.unrealized_pnl() < 0);
    OT_CHECK_EQ(e.total_pnl(), -kI64Max);  // sum saturates as well

    // Pre-trade arithmetic on the same extremes.
    l.max_order_notional = kI64Max;
    l.max_gross_position = kI64Max;
    l.price_band_bps = std::numeric_limits<std::uint32_t>::max();
    RiskEngine f(l, 4);
    OT_REJECT(f.check(kA, kBuy, kI64Max, kQtyMax, kI64Max, 0), Reject::order_notional);
    OT_REJECT(f.check(kA, kBuy, kI64Max / kQtyMax, kQtyMax, 1, 0), Reject::price_band);
    f.on_order_open(kA, kBuy, kQtyMax);
    f.on_order_open(kA, kBuy, kQtyMax);
    OT_CHECK_EQ(f.open_qty(kA, kBuy), std::int64_t{kQtyMax} * 2);
}

// ---------------------------------------------------------------------------------------------
// Randomized accounting invariants
// ---------------------------------------------------------------------------------------------

namespace {

constexpr int kLocs = 4;

I128 iabs(I128 v) { return v < 0 ? -v : v; }

struct RefSlot {
    I128 qty = 0, avg = 0, realized = 0, mark = 0;
    I128 open[2] = {0, 0};
};

// Reference model: the contract's rules transcribed with 128-bit arithmetic. It differs from
// the engine in structure: a fill through zero is split into an explicit close and an explicit
// open, so the engine's single-branch flip is checked against plain composition of the two
// simple cases. It also tracks cash, which gives a second, method-independent PnL:
//
//     total PnL = cash + sum(position * mark) + slack
//
// where `slack` is the signed sum of the remainders dropped by the truncating average
// (zero when every average is exact). This identity does not depend on how realized and
// unrealized PnL are split, so a sign error in either shows up as a violation.
struct Ref {
    RefSlot s[kLocs];
    I128 cash = 0, slack = 0, peak = 0, drawdown = 0, max_loss = 0;
    bool tripped = false;
    std::uint64_t inexact_adds = 0, moved_adds = 0, flips = 0;

    I128 realized() const {
        I128 t = 0;
        for (const RefSlot& x : s) t += x.realized;
        return t;
    }
    I128 unrealized() const {
        I128 t = 0;
        for (const RefSlot& x : s) t += x.mark > 0 ? (x.mark - x.avg) * x.qty : 0;
        return t;
    }
    I128 marked_value() const {
        I128 t = 0;
        for (const RefSlot& x : s) t += x.qty * x.mark;
        return t;
    }
    I128 gross() const {
        I128 t = 0;
        for (const RefSlot& x : s) {
            const I128 a = iabs(x.qty + x.open[0]);
            const I128 b = iabs(x.qty - x.open[1]);
            t += a > b ? a : b;
        }
        return t;
    }

    void sample() {
        const I128 total = realized() + unrealized();
        if (total > peak) peak = total;
        if (peak - total > drawdown) drawdown = peak - total;
        if (max_loss > 0 && total <= -max_loss) tripped = true;
    }

    void apply(RefSlot& x, bool buy, I128 q, I128 px) {
        const I128 held = iabs(x.qty);
        if (x.qty != 0 && (x.qty > 0) != buy) {
            if (q > held) {  // through zero: close everything, then open the rest
                ++flips;
                apply(x, buy, held, px);
                apply(x, buy, q - held, px);
                return;
            }
            x.realized += (x.qty > 0 ? px - x.avg : x.avg - px) * q;
            x.qty += buy ? q : -q;
            if (x.qty == 0) x.avg = 0;
            return;
        }
        const I128 num = x.avg * held + px * q;
        const I128 n = held + q;
        const I128 r = num % n;
        x.avg = num / n;
        slack += buy ? r : -r;
        if (r != 0) ++inexact_adds;
        x.qty += buy ? q : -q;
    }

    void fill(Locate l, bool buy, I128 q, I128 px) {
        apply(s[l], buy, q, px);
        cash -= (buy ? q : -q) * px;
        sample();
    }
    void set_mark(Locate l, I128 px) {
        s[l].mark = px;
        sample();
    }
    void on_open(Locate l, int side, I128 q) { s[l].open[side] += q; }
    void on_close(Locate l, int side, I128 q) {
        s[l].open[side] -= q < s[l].open[side] ? q : s[l].open[side];
    }
};

// Returns a description of the first difference between engine and reference, or nullptr.
const char* first_mismatch(const RiskEngine& e, const Ref& r) {
    I128 open_shares = 0;
    for (Locate l = 0; l < kLocs; ++l) {
        const Position& p = e.position(l);
        if (I128{p.qty} != r.s[l].qty) return "position qty";
        if (I128{p.avg_price} != r.s[l].avg) return "average price";
        if (I128{p.realized} != r.s[l].realized) return "position realized";
        if (I128{e.open_qty(l, kBuy)} != r.s[l].open[0]) return "open buy qty";
        if (I128{e.open_qty(l, kSell)} != r.s[l].open[1]) return "open sell qty";
        open_shares += r.s[l].open[0] + r.s[l].open[1];
    }
    if (I128{e.realized_pnl()} != r.realized()) return "total realized";
    if (I128{e.unrealized_pnl()} != r.unrealized()) return "total unrealized";
    if (I128{e.total_pnl()} != r.realized() + r.unrealized()) return "total pnl";
    if (I128{e.max_drawdown()} != r.drawdown) return "max drawdown";
    if (I128{e.gross_exposure()} != r.gross()) return "gross exposure";
    if (e.kill_switch() != r.tripped) return "kill switch";
    if (I128{e.total_pnl()} != r.cash + r.marked_value() + r.slack) return "cash identity";
    if ((e.open_orders() == 0) != (open_shares == 0)) return "open order count vs shares";
    if (I128{e.open_orders()} > open_shares) return "open order count above open shares";
    return nullptr;
}

std::int64_t gcd64(std::int64_t a, std::int64_t b) { return std::gcd(a, b); }

// Drives engine and reference with the same random fills, marks and order events and compares
// everything after every step. With `exact` set, every fill that extends a position is priced
// so the volume weighted average is a whole number: px = avg + m * n / gcd(q, n) gives
// (avg*held + px*q) / n = avg + m*q/gcd, an integer. Truncation is then never exercised and
// the cash identity must hold with zero slack, which pins the accounting down against pure
// cash flows. Without it, averages round and the identity must hold with the exact dropped
// remainders.
void run_random(std::uint64_t seed, int steps, bool exact, Ref& ref) {
    Limits l = wide();
    l.max_loss = 3'000'000;
    ref.max_loss = l.max_loss;
    RiskEngine e(l, kLocs);
    Rng rng(seed);

    for (Locate loc = 0; loc < kLocs; ++loc) {
        e.mark(loc, 100'000);
        ref.set_mark(loc, 100'000);
    }

    for (int step = 0; step < steps; ++step) {
        const Locate loc = static_cast<Locate>(rng.bounded(kLocs));
        RefSlot& x = ref.s[loc];
        const std::uint64_t action = rng.bounded(16);

        if (action < 9) {  // fill
            const std::int64_t held = static_cast<std::int64_t>(iabs(x.qty));
            const bool long_pos = x.qty > 0;
            bool buy = rng.chance(1, 2);
            Qty q = static_cast<Qty>(1 + rng.bounded(40));
            if (held > 150 && rng.chance(3, 4)) buy = !long_pos;  // keep positions bounded
            if (held > 0 && rng.chance(1, 6)) {                   // land on and around zero
                buy = !long_pos;
                q = static_cast<Qty>(held + static_cast<std::int64_t>(rng.bounded(3)) - 1);
                if (q == 0) q = 1;
            }
            Price px = 100'000 + rng.range(-5'000, 5'000);
            const bool extends = x.qty == 0 || (x.qty > 0) == buy;
            if (exact && extends && held > 0) {
                const std::int64_t n = held + q;
                const std::int64_t step_px = n / gcd64(q, n);
                const Price cand = static_cast<Price>(x.avg) + rng.range(-3, 3) * step_px;
                if (cand >= 1 && cand <= 2'000'000'000) px = cand;
                else px = static_cast<Price>(x.avg);
                if (px != static_cast<Price>(x.avg)) ++ref.moved_adds;
            }
            e.on_fill(loc, buy ? kBuy : kSell, q, px);
            ref.fill(loc, buy, q, px);
        } else if (action < 13) {  // mark
            const Price m = 100'000 + rng.range(-6'000, 6'000);
            e.mark(loc, m);
            ref.set_mark(loc, m);
        } else if (action == 13) {  // order events
            const int side = static_cast<int>(rng.bounded(2));
            const Qty q = static_cast<Qty>(1 + rng.bounded(50));
            if (rng.chance(1, 2)) {
                e.on_order_open(loc, side == 0 ? kBuy : kSell, q);
                ref.on_open(loc, side, q);
            } else {
                e.on_order_closed(loc, side == 0 ? kBuy : kSell, q);
                ref.on_close(loc, side, q);
            }
        } else if (action == 14) {  // garbage the engine must ignore
            e.on_fill(static_cast<Locate>(kLocs + rng.bounded(100)), kBuy, 5, 100'000);
            e.on_fill(loc, kBuy, 0, 100'000);
            e.on_fill(loc, kSell, 5, 0);
            e.mark(loc, -3);
            e.on_order_open(static_cast<Locate>(kLocs), kBuy, 5);
        } else if (rng.chance(1, 4)) {  // operator resets the switch now and then
            e.set_kill_switch(false);
            ref.tripped = false;
        }

        if (const char* what = first_mismatch(e, ref)) {
            std::fprintf(stderr, "  seed=%llu exact=%d step=%d: %s differs\n",
                         static_cast<unsigned long long>(seed), exact ? 1 : 0, step, what);
            OT_CHECK(false);
            return;
        }
    }
}

}  // namespace

OT_TEST(random_fills_exact_averages_match_cash_flows) {
    std::uint64_t moved = 0, flips = 0;
    for (const std::uint64_t seed : {1ULL, 2ULL, 3ULL, 0xC0FFEEULL}) {
        Ref ref;
        run_random(seed, 30'000, true, ref);
        OT_CHECK_EQ(ref.inexact_adds, std::uint64_t{0});  // the generator really avoided rounding
        OT_CHECK(ref.slack == 0);
        moved += ref.moved_adds;
        flips += ref.flips;
    }
    OT_CHECK(moved > 2'000);  // prices did move: not just fills at the average
    OT_CHECK(flips > 200);
}

OT_TEST(random_fills_with_rounding_match_reference_and_dropped_remainders) {
    std::uint64_t inexact = 0, flips = 0;
    for (const std::uint64_t seed : {11ULL, 12ULL, 13ULL, 0xFEEDULL}) {
        Ref ref;
        run_random(seed, 30'000, false, ref);
        inexact += ref.inexact_adds;
        flips += ref.flips;
    }
    OT_CHECK(inexact > 2'000);  // truncation happens all the time here
    OT_CHECK(flips > 200);
}

#if defined(__clang__)
#pragma clang attribute pop
#endif

OT_TEST_MAIN()

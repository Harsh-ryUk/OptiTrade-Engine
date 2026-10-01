// Tests for the microprice market maker.
//
// Reference book used by most scenarios ("base book"): best bid 100.0000 x 300 and best
// ask 100.0400 x 100, so the spread is 400 and
//     microprice = 1'000'000 + 400 * 300 / (300 + 100) = 1'000'300
// (textbook form: (1'000'000*100 + 1'000'400*300) / 400 = 400'120'000 / 400 = 1'000'300).
// With half_spread 200 the un-skewed quotes are bid 1'000'100 and ask 1'000'500, both
// already on the 100-unit tick grid. skew_per_share is 10, so every share of inventory
// moves both quotes by 10 price units.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <type_traits>
#include <utility>
#include <vector>

#include "check.hpp"
#include "optitrade/core/rng.hpp"
#include "optitrade/strategy/microprice_maker.hpp"
#include "test_strategy_util.hpp"

using namespace optitrade;
using namespace ot_strat;
using strategy::MicropriceMaker;
using K = Action::Kind;

namespace {

constexpr Locate kLoc = 1;

MicropriceMaker::Config config() {
    MicropriceMaker::Config c;
    c.max_locates = 4;
    c.half_spread = 200;
    c.tick = 100;
    c.skew_per_share = 10;
    c.quote_qty = 100;
    c.max_inventory = 300;
    c.requote_ticks = 2;
    c.max_spread = 1000;
    return c;
}

void base_book(Harness& h) { h.set_book(kLoc, {{1'000'000, 300}}, {{1'000'400, 100}}); }

std::size_t poke(Harness& h, MicropriceMaker& s, Nanos now = 1, Locate loc = kLoc) {
    const std::size_t before = h.orders.actions.size();
    h.now = now;
    s.on_book_update(loc, h.ctx());
    return h.orders.actions.size() - before;
}

// Both quotes acknowledged and resting.
void make_live(Harness& h, MicropriceMaker& s) {
    for (oms::OrderId id : {s.bid_id(kLoc), s.ask_id(kLoc)}) {
        if (id != 0) h.orders.set_status(id, oms::OrderStatus::live);
    }
}

}  // namespace

static_assert(strategy::Strategy<MicropriceMaker>);
static_assert(std::is_default_constructible_v<MicropriceMaker>);

OT_TEST(microprice_known_answers) {
    OT_CHECK_EQ(MicropriceMaker::microprice(1'000'000, 1'000'400, 300, 100), 1'000'300);
    OT_CHECK_EQ(MicropriceMaker::microprice(1'000'000, 1'000'400, 100, 300), 1'000'100);  // mirror
    OT_CHECK_EQ(MicropriceMaker::microprice(1'000'000, 1'000'100, 5, 5), 1'000'050);
    // Truncation: (1'000'000*2 + 1'000'100*1) / 3 = 3'000'100 / 3 = 1'000'033.33
    OT_CHECK_EQ(MicropriceMaker::microprice(1'000'000, 1'000'100, 1, 2), 1'000'033);
    // (1'000'000*1 + 1'000'100*2) / 3 = 3'000'200 / 3 = 1'000'066.67 -> 1'000'066
    OT_CHECK_EQ(MicropriceMaker::microprice(1'000'000, 1'000'100, 2, 1), 1'000'066);
    // Extremes stay in range: spread 2^31 against 2^32-1 shares.
    OT_CHECK_EQ(MicropriceMaker::microprice(1, 1 + (Price{1} << 31), 4'294'967'295u, 1),
                1 + (Price{1} << 31) * 4'294'967'295LL / 4'294'967'296LL);
}

OT_TEST(quotes_both_sides_around_microprice) {
    Harness h;
    MicropriceMaker s(config());
    base_book(h);
    OT_CHECK_EQ(poke(h, s), 2u);
    const Action& bid = h.orders.actions[0];
    const Action& ask = h.orders.actions[1];
    OT_CHECK(bid.kind == K::submit && bid.side == Side::buy);
    OT_CHECK_EQ(bid.price, 1'000'100);  // 1'000'300 - 200
    OT_CHECK_EQ(bid.qty, 100u);
    OT_CHECK(bid.tif == oms::Tif::day);
    OT_CHECK(ask.kind == K::submit && ask.side == Side::sell);
    OT_CHECK_EQ(ask.price, 1'000'500);  // 1'000'300 + 200
    OT_CHECK_EQ(ask.qty, 100u);
    OT_CHECK(ask.tif == oms::Tif::day);
    OT_CHECK_EQ(s.bid_id(kLoc), 1u);
    OT_CHECK_EQ(s.ask_id(kLoc), 2u);
    // Nothing more while both are unacknowledged, and nothing when live at the target.
    OT_CHECK_EQ(poke(h, s), 0u);
    make_live(h, s);
    OT_CHECK_EQ(poke(h, s), 0u);
}

OT_TEST(quotes_round_away_from_fair_value_onto_the_tick_grid) {
    Harness h;
    MicropriceMaker s(config());
    // bid 100.0050 x 100, ask 100.0250 x 100: fair = 1'000'050 + 200*100/200 = 1'000'150
    // raw bid 1'000'150 - 200 = 999'950 -> floor to 999'900; raw ask 1'000'350 -> ceil to 1'000'400
    h.set_book(kLoc, {{1'000'050, 100}}, {{1'000'250, 100}});
    OT_CHECK_EQ(poke(h, s), 2u);
    OT_CHECK_EQ(h.orders.actions[0].price, 999'900);
    OT_CHECK_EQ(h.orders.actions[1].price, 1'000'400);
}

OT_TEST(inventory_skew_moves_both_quotes_by_exactly_skew_times_inventory) {
    auto quotes = [](std::int64_t signed_inventory) {
        Harness h;
        MicropriceMaker s(config());
        base_book(h);
        if (signed_inventory > 0) h.fill(kLoc, Side::buy, static_cast<Qty>(signed_inventory), 1'000'000);
        if (signed_inventory < 0) h.fill(kLoc, Side::sell, static_cast<Qty>(-signed_inventory), 1'000'000);
        poke(h, s);
        return std::pair{h.orders.actions.at(0), h.orders.actions.at(1)};
    };
    // Long 30: skew = 10 * 30 = 300 -> bid 1'000'100 - 300, ask 1'000'500 - 300.
    auto [b30, a30] = quotes(30);
    OT_CHECK_EQ(b30.price, 999'800);
    OT_CHECK_EQ(a30.price, 1'000'200);
    OT_CHECK_EQ(b30.qty, 100u);  // room 300 - 30 = 270
    // Short 10: skew = -100 -> both quotes 100 higher.
    auto [bm10, am10] = quotes(-10);
    OT_CHECK_EQ(bm10.price, 1'000'200);
    OT_CHECK_EQ(am10.price, 1'000'600);
    // Long 10 and long 20 for the linear slope: 100 and 200 lower.
    OT_CHECK_EQ(quotes(10).first.price, 1'000'000);
    OT_CHECK_EQ(quotes(20).first.price, 999'900);
    OT_CHECK_EQ(quotes(20).second.price, 1'000'300);
}

OT_TEST(requote_hysteresis_is_inclusive_at_requote_ticks) {
    Harness h;
    MicropriceMaker s(config());  // requote threshold: 2 ticks = 200
    base_book(h);
    poke(h, s);  // bid 1'000'100 (id 1), ask 1'000'500 (id 2)
    make_live(h, s);

    // Inventory 10: targets move by 100 (one tick): below the threshold, stay put.
    h.fill(kLoc, Side::buy, 10, 1'000'000);
    OT_CHECK_EQ(poke(h, s), 0u);

    // Inventory 20: bid target 1'000'100 - 200 = 999'900, ask 1'000'500 - 200 = 1'000'300.
    // Each moved by exactly 200: replace both.
    h.fill(kLoc, Side::buy, 10, 1'000'000);
    OT_CHECK_EQ(poke(h, s), 2u);
    const Action& rb = h.orders.actions[2];
    const Action& ra = h.orders.actions[3];
    OT_CHECK(rb.kind == K::replace);
    OT_CHECK_EQ(rb.id, 1u);
    OT_CHECK_EQ(rb.price, 999'900);
    OT_CHECK_EQ(rb.qty, 100u);  // cum 0 + room 100
    OT_CHECK(ra.kind == K::replace);
    OT_CHECK_EQ(ra.id, 2u);
    OT_CHECK_EQ(ra.price, 1'000'300);
    OT_CHECK_EQ(ra.qty, 100u);

    // Waiting for the exchange: no further messages until acknowledged.
    OT_CHECK_EQ(poke(h, s), 0u);
    h.orders.ack_replace(1);
    h.orders.ack_replace(2);
    OT_CHECK_EQ(poke(h, s), 0u);  // at target
    // Back to flat inventory would move targets by 200 again: replaced once more.
    h.risk.on_fill(kLoc, Side::sell, 20, 1'000'000);
    OT_CHECK_EQ(poke(h, s), 2u);
    OT_CHECK_EQ(h.orders.actions[4].price, 1'000'100);
    OT_CHECK_EQ(h.orders.actions[5].price, 1'000'500);
}

OT_TEST(book_move_below_threshold_keeps_quotes_and_at_threshold_requotes) {
    Harness h;
    MicropriceMaker s(config());
    base_book(h);
    poke(h, s);
    make_live(h, s);
    // Ask queue 100 -> 70: fair = 1'000'000 + 400*300/370 = 1'000'000 + 324 = 1'000'324;
    // bid target floor(1'000'124) = 1'000'100 (unchanged), ask ceil(1'000'524) = 1'000'600 (moved 100).
    h.set_book(kLoc, {{1'000'000, 300}}, {{1'000'400, 70}});
    OT_CHECK_EQ(poke(h, s), 0u);
    // Ask queue 100 -> 30 and bid 300: fair = 1'000'000 + 400*300/330 = 1'000'363;
    // bid floor(1'000'163) = 1'000'100, ask ceil(1'000'563) = 1'000'600: still one tick.
    h.set_book(kLoc, {{1'000'000, 300}}, {{1'000'400, 30}});
    OT_CHECK_EQ(poke(h, s), 0u);
    // Bid queue 300 -> 100 and ask 100: fair 1'000'200, bid 1'000'000 (moved 100), ask 1'000'400 (moved 100).
    h.set_book(kLoc, {{1'000'000, 100}}, {{1'000'400, 100}});
    OT_CHECK_EQ(poke(h, s), 0u);
    // Ask queue 100 -> 300 with bid 100: fair = 1'000'000 + 400*100/400 = 1'000'100;
    // bid 999'900 (moved 200), ask 1'000'300 (moved 200): both replaced.
    h.set_book(kLoc, {{1'000'000, 100}}, {{1'000'400, 300}});
    OT_CHECK_EQ(poke(h, s), 2u);
    OT_CHECK_EQ(h.orders.actions[2].price, 999'900);
    OT_CHECK_EQ(h.orders.actions[3].price, 1'000'300);
}

OT_TEST(replacement_after_partial_fill_keeps_a_full_clip_quoting) {
    Harness h;
    MicropriceMaker s(config());
    base_book(h);
    poke(h, s);
    make_live(h, s);
    h.orders.mut(1).cum_qty = 40;  // the bid was hit for 40
    h.orders.mut(1).leaves_qty = 60;
    h.fill(kLoc, Side::buy, 40, 1'000'100);
    // skew = 10 * 40 = 400: bid 1'000'100 - 400 = 999'700, ask 1'000'500 - 400 = 1'000'100 (>= best bid + tick).
    OT_CHECK_EQ(poke(h, s), 2u);
    OT_CHECK_EQ(h.orders.actions[2].id, 1u);
    OT_CHECK_EQ(h.orders.actions[2].price, 999'700);
    OT_CHECK_EQ(h.orders.actions[2].qty, 140u);  // total intended size = 40 executed + 100 open
    OT_CHECK_EQ(h.orders.actions[3].price, 1'000'100);
    OT_CHECK_EQ(h.orders.actions[3].qty, 100u);
}

OT_TEST(long_at_max_inventory_stops_bidding_and_never_prices_through_the_bid) {
    Harness h;
    MicropriceMaker s(config());
    base_book(h);
    poke(h, s);
    make_live(h, s);
    h.fill(kLoc, Side::buy, 300, 1'000'000);  // at max_inventory
    // Bid side pulled. Ask: raw 1'000'500 - 10*300 = 997'500 would sit below the best bid, so it is
    // held at best bid + one tick = 1'000'100 (moved 400 from 1'000'500: replace).
    OT_CHECK_EQ(poke(h, s), 2u);
    OT_CHECK(h.orders.actions[2].kind == K::cancel);
    OT_CHECK_EQ(h.orders.actions[2].id, 1u);
    OT_CHECK(h.orders.actions[3].kind == K::replace);
    OT_CHECK_EQ(h.orders.actions[3].id, 2u);
    OT_CHECK_EQ(h.orders.actions[3].price, 1'000'100);
    // The cancel is pending; asking again does not cancel twice.
    OT_CHECK_EQ(poke(h, s), 0u);
}

OT_TEST(fresh_quotes_respect_inventory_room) {
    // Long 299: only one share of room on the bid side.
    Harness h;
    MicropriceMaker s(config());
    base_book(h);
    h.fill(kLoc, Side::buy, 299, 1'000'000);
    OT_CHECK_EQ(poke(h, s), 2u);
    OT_CHECK_EQ(h.orders.actions[0].qty, 1u);
    OT_CHECK_EQ(h.orders.actions[0].price, 997'100);   // 1'000'100 - 2990 = 997'110 -> floor tick
    OT_CHECK_EQ(h.orders.actions[1].price, 1'000'100);  // clamped: best bid + tick
    OT_CHECK_EQ(h.orders.actions[1].qty, 100u);

    // Long 300: no bid at all.
    Harness h2;
    MicropriceMaker s2(config());
    base_book(h2);
    h2.fill(kLoc, Side::buy, 300, 1'000'000);
    OT_CHECK_EQ(poke(h2, s2), 1u);
    OT_CHECK(h2.orders.actions[0].side == Side::sell);
    OT_CHECK_EQ(s2.bid_id(kLoc), 0u);

    // Short 300: no ask; the bid is raised but held one tick under the best ask.
    // raw 1'000'100 + 3000 = 1'003'100 -> min(., 1'000'400 - 100) = 1'000'300.
    Harness h3;
    MicropriceMaker s3(config());
    base_book(h3);
    h3.fill(kLoc, Side::sell, 300, 1'000'000);
    OT_CHECK_EQ(poke(h3, s3), 1u);
    OT_CHECK(h3.orders.actions[0].side == Side::buy);
    OT_CHECK_EQ(h3.orders.actions[0].price, 1'000'300);
    OT_CHECK_EQ(h3.orders.actions[0].qty, 100u);
    OT_CHECK_EQ(s3.ask_id(kLoc), 0u);
}

OT_TEST(cancels_both_quotes_on_crossed_one_sided_and_wide_books) {
    // Crossed: a new bid at the ask price locks the book.
    {
        Harness h;
        MicropriceMaker s(config());
        base_book(h);
        poke(h, s);
        make_live(h, s);
        h.add(kLoc, Side::buy, 1'000'400, 50);
        OT_CHECK_EQ(poke(h, s), 2u);
        OT_CHECK(h.orders.actions[2].kind == K::cancel && h.orders.actions[2].id == 1);
        OT_CHECK(h.orders.actions[3].kind == K::cancel && h.orders.actions[3].id == 2);
        OT_CHECK_EQ(poke(h, s), 0u);  // pending_cancel: nothing more
    }
    // One-sided: the ask side disappears.
    {
        Harness h;
        MicropriceMaker s(config());
        base_book(h);
        poke(h, s);
        make_live(h, s);
        h.clear_side(kLoc, Side::sell);
        OT_CHECK_EQ(poke(h, s), 2u);
        OT_CHECK_EQ(h.orders.count(K::cancel), 2u);
    }
    // Empty side on the bid instead.
    {
        Harness h;
        MicropriceMaker s(config());
        base_book(h);
        poke(h, s);
        make_live(h, s);
        h.clear_side(kLoc, Side::buy);
        OT_CHECK_EQ(poke(h, s), 2u);
        OT_CHECK_EQ(h.orders.count(K::cancel), 2u);
    }
    // Wide: spread 1001 > max_spread 1000 cancels, 1000 quotes.
    {
        Harness h;
        MicropriceMaker s(config());
        h.set_book(kLoc, {{1'000'000, 100}}, {{1'001'001, 100}});
        OT_CHECK_EQ(poke(h, s), 0u);  // nothing to cancel yet, and nothing is quoted
        Harness h2;
        MicropriceMaker s2(config());
        h2.set_book(kLoc, {{1'000'000, 100}}, {{1'001'000, 100}});
        // fair 1'000'500; bid 1'000'300, ask 1'000'700
        OT_CHECK_EQ(poke(h2, s2), 2u);
        OT_CHECK_EQ(h2.orders.actions[0].price, 1'000'300);
        OT_CHECK_EQ(h2.orders.actions[1].price, 1'000'700);
        make_live(h2, s2);
        h2.set_book(kLoc, {{1'000'000, 100}}, {{1'001'001, 100}});
        OT_CHECK_EQ(poke(h2, s2), 2u);
        OT_CHECK_EQ(h2.orders.count(K::cancel), 2u);
    }
}

OT_TEST(refused_cancel_is_retried_and_terminal_update_clears_the_id) {
    Harness h;
    MicropriceMaker s(config());
    base_book(h);
    poke(h, s);
    make_live(h, s);
    h.clear_side(kLoc, Side::sell);
    h.orders.cancel_status = oms::SubmitStatus::bad_state;
    OT_CHECK_EQ(poke(h, s), 2u);
    OT_CHECK_EQ(poke(h, s), 2u);  // still tracked, asked again
    h.orders.cancel_status = oms::SubmitStatus::ok;
    OT_CHECK_EQ(poke(h, s), 2u);
    OT_CHECK_EQ(poke(h, s), 0u);
    h.orders.set_status(1, oms::OrderStatus::canceled);
    s.on_order_update(*h.orders.find(1), h.ctx());
    OT_CHECK_EQ(s.bid_id(kLoc), 0u);
    OT_CHECK_EQ(s.ask_id(kLoc), 2u);
    h.orders.set_status(2, oms::OrderStatus::canceled);
    s.on_order_update(*h.orders.find(2), h.ctx());
    OT_CHECK_EQ(s.ask_id(kLoc), 0u);
}

OT_TEST(terminal_updates_clear_only_the_matching_quote) {
    Harness h;
    MicropriceMaker s(config());
    base_book(h);
    poke(h, s);
    make_live(h, s);

    // A non-terminal update or an unknown id changes nothing.
    oms::OrderInfo other = *h.orders.find(1);
    other.id = 99;
    other.status = oms::OrderStatus::filled;
    s.on_order_update(other, h.ctx());
    OT_CHECK_EQ(s.bid_id(kLoc), 1u);
    s.on_order_update(*h.orders.find(1), h.ctx());  // live
    OT_CHECK_EQ(s.bid_id(kLoc), 1u);

    // Bid filled: the bid id is cleared, the ask id is not, and a new bid is quoted.
    h.fill(kLoc, Side::buy, 100, 1'000'100);
    h.orders.mut(1).cum_qty = 100;
    h.orders.set_status(1, oms::OrderStatus::filled);
    s.on_order_update(*h.orders.find(1), h.ctx());
    OT_CHECK_EQ(s.bid_id(kLoc), 0u);
    OT_CHECK_EQ(s.ask_id(kLoc), 2u);
    // Inventory 100 -> skew 1000. bid raw 1'000'100 - 1000 = 999'100 (fresh quote at target),
    // ask target 1'000'500 - 1000 = 999'500 -> held at best bid + tick = 1'000'100 (replace).
    OT_CHECK_EQ(poke(h, s), 2u);
    OT_CHECK(h.orders.actions[2].kind == K::submit);
    OT_CHECK_EQ(h.orders.actions[2].price, 999'100);
    OT_CHECK_EQ(h.orders.actions[2].qty, 100u);  // room 300 - 100 = 200 -> clip 100
    OT_CHECK(h.orders.actions[3].kind == K::replace);
    OT_CHECK_EQ(s.bid_id(kLoc), 3u);

    // A quote that went terminal without an update reaching the strategy is forgotten and re-quoted.
    Harness h2;
    MicropriceMaker s2(config());
    base_book(h2);
    poke(h2, s2);
    h2.orders.set_status(1, oms::OrderStatus::rejected);  // no update delivered
    poke(h2, s2);
    OT_CHECK_EQ(s2.bid_id(kLoc), 3u);
}

OT_TEST(never_quotes_a_non_positive_price) {
    Harness h;
    MicropriceMaker s(config());
    // bid 0.0100, ask 0.0200: fair = 100 + 100*100/200 = 150. Bid raw = min(150 - 200, 200 - 100) = -50
    // (skipped); ask raw = max(150 + 200, 100 + 100) = 350 -> ceil 400.
    h.set_book(kLoc, {{100, 100}}, {{200, 100}});
    OT_CHECK_EQ(poke(h, s), 1u);
    OT_CHECK(h.orders.actions[0].side == Side::sell);
    OT_CHECK_EQ(h.orders.actions[0].price, 400);
    OT_CHECK_EQ(s.bid_id(kLoc), 0u);
}

OT_TEST(refused_submit_is_not_tracked_and_is_retried) {
    Harness h;
    MicropriceMaker s(config());
    base_book(h);
    h.orders.submit_status = oms::SubmitStatus::rejected_by_risk;
    OT_CHECK_EQ(poke(h, s), 2u);
    OT_CHECK_EQ(s.bid_id(kLoc), 0u);
    OT_CHECK_EQ(s.ask_id(kLoc), 0u);
    h.orders.submit_status = oms::SubmitStatus::ok;
    OT_CHECK_EQ(poke(h, s), 2u);
    OT_CHECK_EQ(s.bid_id(kLoc), 1u);
}

OT_TEST(locates_at_or_above_max_locates_are_ignored) {
    Harness h;
    MicropriceMaker s(config());  // max_locates = 4
    for (Locate loc : {Locate{3}, Locate{4}, Locate{65535}}) {
        h.set_book(loc, {{1'000'000, 300}}, {{1'000'400, 100}});
    }
    OT_CHECK_EQ(poke(h, s, 1, 4), 0u);
    OT_CHECK_EQ(poke(h, s, 1, 65535), 0u);
    OT_CHECK_EQ(s.bid_id(4), 0u);
    OT_CHECK_EQ(s.bid_id(65535), 0u);
    OT_CHECK_EQ(poke(h, s, 1, 3), 2u);
    oms::OrderInfo o;
    o.id = 5;
    o.req.locate = 65535;
    o.status = oms::OrderStatus::canceled;
    s.on_order_update(o, h.ctx());
}

OT_TEST(config_is_sanitised) {
    MicropriceMaker::Config c;
    c.half_spread = 0;
    c.tick = 0;
    c.quote_qty = 0;
    c.requote_ticks = 0;
    c.skew_per_share = -5;
    MicropriceMaker s(c);
    OT_CHECK(s.config().half_spread >= 1);
    OT_CHECK(s.config().tick >= 1);
    OT_CHECK(s.config().quote_qty >= 1);
    OT_CHECK(s.config().requote_ticks >= 1);
    OT_CHECK(s.config().skew_per_share >= 0);
}

OT_TEST(random_books_never_yield_bad_orders_or_inventory_beyond_the_limit) {
    MicropriceMaker::Config c = config();
    Harness h;
    MicropriceMaker s(c);
    Responder<MicropriceMaker> ex(h, s);
    Rng rng(0xA11CE);
    int fills = 0;
    for (int i = 0; i < 20'000; ++i) {
        std::vector<std::pair<Price, Qty>> bids, asks;
        // Now and then a penny-priced book, to reach the non-positive quote paths.
        const Price base = rng.chance(1, 10) ? 300 : 1'000'000;
        const Price bid_top = base + rng.range(0, 5) * 100;
        const Price ask_top = bid_top + rng.range(-1, 14) * 100;
        for (std::uint64_t k = 0, n = rng.bounded(4); k < n; ++k) {
            bids.push_back({bid_top - static_cast<Price>(k) * 100, static_cast<Qty>(rng.range(1, 900))});
        }
        for (std::uint64_t k = 0, n = rng.bounded(4); k < n; ++k) {
            asks.push_back({ask_top + static_cast<Price>(k) * 100, static_cast<Qty>(rng.range(1, 900))});
        }
        h.set_book(kLoc, bids, asks);
        h.now += 100;
        const std::size_t before = h.orders.actions.size();
        s.on_book_update(kLoc, h.ctx());
        for (std::size_t k = before; k < h.orders.actions.size(); ++k) {
            const Action& a = h.orders.actions[k];
            if (a.kind != K::cancel) {
                OT_CHECK(a.qty > 0);
                OT_CHECK(a.price > 0);
                OT_CHECK_EQ(a.price % 100, 0);
            }
        }
        ex.settle();
        // Random fills of resting quotes move inventory, always within the limit plus one clip.
        if (rng.chance(1, 4)) {
            const oms::OrderId id = rng.chance(1, 2) ? s.bid_id(kLoc) : s.ask_id(kLoc);
            if (id != 0) {
                const oms::OrderInfo* o = h.orders.find(id);
                const Qty q = std::min<Qty>(o->req.qty - o->cum_qty, static_cast<Qty>(rng.range(1, 100)));
                if (q > 0 && o->status == oms::OrderStatus::live) {
                    h.fill(kLoc, o->req.side, q, o->req.price);
                    h.orders.mut(id).cum_qty += q;
                    h.orders.mut(id).leaves_qty -= q;
                    if (h.orders.mut(id).leaves_qty == 0) h.orders.set_status(id, oms::OrderStatus::filled);
                    s.on_order_update(*h.orders.find(id), h.ctx());
                    ++fills;
                }
            }
        }
        OT_CHECK(std::abs(h.risk.position(kLoc).qty) <= c.max_inventory);
    }
    // The run has to reach every code path for the invariant checks to mean anything.
    OT_CHECK(fills > 500);
    OT_CHECK(h.orders.count(K::submit) > 500);
    OT_CHECK(h.orders.count(K::replace) > 500);
    OT_CHECK(h.orders.count(K::cancel) > 500);
}

OT_TEST_MAIN()

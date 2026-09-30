// Tests for the exchange simulator.
//
// Scripted tests are known-answer scenarios: the expected quantities and prices are worked out
// by hand in the comments next to them. The market data is built with itch::encode, reports are
// decoded with ouch::decode_outbound, and one test checks report bytes against offsets written
// out from the OUCH tables, so an encoder/decoder bug that is symmetric cannot pass.
//
// The last group is randomized: a closed-form queue model checked against the simulator, and a
// long mixed scenario that must replay byte for byte and keep the report stream self-consistent.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "check.hpp"
#include "optitrade/core/digest.hpp"
#include "optitrade/core/rng.hpp"
#include "optitrade/itch/encoder.hpp"
#include "optitrade/ouch/codec.hpp"
#include "optitrade/sim/exchange_sim.hpp"

using namespace optitrade;

namespace {

constexpr Nanos kAll = ~Nanos{0};
constexpr Price kP100 = 1'000'000;  // 100.0000
constexpr Locate kAapl = 1;
constexpr Locate kMsft = 2;

// One decoded report plus its delivery time and raw bytes.
struct Rep {
    char type{};
    Nanos deliver{};
    ouch::Accepted acc;
    ouch::Replaced rep;
    ouch::Canceled can;
    ouch::Executed exe;
    ouch::Rejected rej;
    std::vector<std::byte> raw;
};

struct Decoder {
    Rep* out;
    void on(const ouch::Accepted& m) noexcept { out->type = 'A'; out->acc = m; }
    void on(const ouch::Replaced& m) noexcept { out->type = 'U'; out->rep = m; }
    void on(const ouch::Canceled& m) noexcept { out->type = 'C'; out->can = m; }
    void on(const ouch::Executed& m) noexcept { out->type = 'E'; out->exe = m; }
    void on(const ouch::Rejected& m) noexcept { out->type = 'J'; out->rej = m; }
};

std::vector<Rep> take(sim::ExchangeSim& s, Nanos now = kAll) {
    std::vector<Rep> reps;
    s.drain_reports(now, [&](std::span<const std::byte> msg, Nanos deliver) {
        Rep r;
        r.deliver = deliver;
        r.raw.assign(msg.begin(), msg.end());
        Decoder d{&r};
        const DecodeStatus st = ouch::decode_outbound(msg, d);
        OT_CHECK_EQ(st, DecodeStatus::ok);
        reps.push_back(std::move(r));
    });
    return reps;
}

sim::SimConfig make_config(Nanos order_latency, Nanos report_latency, std::size_t max_orders) {
    sim::SimConfig c;
    c.order_latency_ns = order_latency;
    c.report_latency_ns = report_latency;
    c.max_orders = max_orders;
    c.books.max_orders = 1u << 14;
    c.books.max_levels_per_side = 32;
    c.books.max_symbols = 16;
    return c;
}

ouch::Token tok(std::uint64_t id) { return ouch::Token::from_id(id); }

ouch::EnterOrder enter(std::uint64_t id, Side side, Qty qty, Price px,
                       std::uint32_t tif = ouch::kTifSystemHours, const char* sym = "AAPL") {
    ouch::EnterOrder e;
    e.token = tok(id);
    e.side = side;
    e.shares = qty;
    e.stock = Symbol(sym);
    e.price = px;
    e.time_in_force = tif;
    return e;
}

// A simulator with two symbols listed and helpers that push ITCH messages into it.
struct Rig {
    sim::ExchangeSim sim;
    Nanos order_latency;

    explicit Rig(Nanos ol = 0, Nanos rl = 0, std::size_t max_orders = 64)
        : sim(make_config(ol, rl, max_orders)), order_latency(ol) {
        directory(kAapl, "AAPL");
        directory(kMsft, "MSFT");
    }

    template <class M>
    void feed(const M& m, Nanos ts) {
        std::array<std::byte, 64> buf{};
        const std::size_t n = itch::encode(m, buf);
        OT_CHECK(n > 0);
        sim.on_itch(std::span<const std::byte>(buf.data(), n), ts);
    }

    void directory(Locate loc, const char* sym) {
        itch::StockDirectory d;
        d.h.locate = loc;
        d.symbol = Symbol(sym);
        feed(d, 0);
    }
    void add(OrderRef ref, Side side, Qty qty, Price px, Nanos ts, Locate loc = kAapl) {
        itch::AddOrder a;
        a.h.locate = loc;
        a.ref = ref;
        a.side = side;
        a.shares = qty;
        a.symbol = Symbol(loc == kAapl ? "AAPL" : "MSFT");
        a.price = px;
        feed(a, ts);
    }
    void exec(OrderRef ref, Qty qty, Nanos ts) {
        itch::OrderExecuted e;
        e.ref = ref;
        e.shares = qty;
        feed(e, ts);
    }
    void exec_price(OrderRef ref, Qty qty, Price px, Nanos ts) {
        itch::OrderExecutedPrice e;
        e.ref = ref;
        e.shares = qty;
        e.printable = true;
        e.price = px;
        feed(e, ts);
    }
    void cancel(OrderRef ref, Qty qty, Nanos ts) {
        itch::OrderCancel c;
        c.ref = ref;
        c.shares = qty;
        feed(c, ts);
    }
    void del(OrderRef ref, Nanos ts) {
        itch::OrderDelete d;
        d.ref = ref;
        feed(d, ts);
    }
    void replace(OrderRef old_ref, OrderRef new_ref, Qty qty, Price px, Nanos ts) {
        itch::OrderReplace r;
        r.old_ref = old_ref;
        r.new_ref = new_ref;
        r.shares = qty;
        r.price = px;
        feed(r, ts);
    }
    void trade(Qty qty, Price px, Nanos ts) {
        itch::Trade t;
        t.h.locate = kAapl;
        t.side = Side::buy;
        t.shares = qty;
        t.symbol = Symbol("AAPL");
        t.price = px;
        feed(t, ts);
    }

    // Sends at `t` and processes the arrival (no market message needed).
    bool submit(const ouch::EnterOrder& e, Nanos t) {
        const bool ok = sim.send(e, t);
        sim.advance(t + order_latency);
        return ok;
    }
    bool cancel_order(std::uint64_t id, Qty shares, Nanos t) {
        const bool ok = sim.send(ouch::CancelOrder{tok(id), shares}, t);
        sim.advance(t + order_latency);
        return ok;
    }
    bool replace_order(std::uint64_t id, std::uint64_t new_id, Qty shares, Price px, Nanos t,
                       std::uint32_t tif = ouch::kTifSystemHours) {
        ouch::ReplaceOrder r;
        r.existing = tok(id);
        r.replacement = tok(new_id);
        r.shares = shares;
        r.price = px;
        r.time_in_force = tif;
        const bool ok = sim.send(r, t);
        sim.advance(t + order_latency);
        return ok;
    }
};

// ---- marketable orders ------------------------------------------------------------------

OT_TEST(marketable_buy_walks_two_levels_at_level_prices) {
    Rig rig;
    rig.add(1, Side::sell, 100, 1'000'000, 1);
    rig.add(2, Side::sell, 200, 1'000'100, 2);
    rig.add(3, Side::sell, 500, 1'000'200, 3);
    // 250 at limit 100.0100: 100 @100.0000, then 150 of the 200 @100.0100. The third level is
    // beyond the limit and untouched.
    OT_CHECK(rig.submit(enter(1, Side::buy, 250, 1'000'100), 10));
    const auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{3});
    OT_CHECK_EQ(reps[0].type, 'A');
    OT_CHECK(reps[0].acc.token == tok(1));
    OT_CHECK_EQ(reps[0].acc.shares, Qty{250});
    OT_CHECK_EQ(reps[0].acc.price, Price{1'000'100});
    OT_CHECK_EQ(reps[0].acc.order_state, 'L');
    OT_CHECK_EQ(reps[1].type, 'E');
    OT_CHECK_EQ(reps[1].exe.shares, Qty{100});
    OT_CHECK_EQ(reps[1].exe.price, Price{1'000'000});
    OT_CHECK_EQ(reps[1].exe.liquidity, 'R');
    OT_CHECK_EQ(reps[1].exe.match, std::uint64_t{1});
    OT_CHECK_EQ(reps[2].type, 'E');
    OT_CHECK_EQ(reps[2].exe.shares, Qty{150});
    OT_CHECK_EQ(reps[2].exe.price, Price{1'000'100});
    OT_CHECK_EQ(reps[2].exe.match, std::uint64_t{2});
    OT_CHECK_EQ(rig.sim.fills(), std::uint64_t{2});
    OT_CHECK_EQ(rig.sim.resting_orders(), std::size_t{0});

    // No market impact: the displayed book is unchanged, so an identical order fills identically.
    OT_CHECK_EQ(rig.sim.books().book(kAapl)->qty_at(Side::sell, 1'000'000), Qty{100});
    OT_CHECK(rig.submit(enter(2, Side::buy, 250, 1'000'100), 11));
    const auto again = take(rig.sim);
    OT_CHECK_EQ(again.size(), std::size_t{3});
    OT_CHECK_EQ(again[1].exe.shares, Qty{100});
    OT_CHECK_EQ(again[2].exe.shares, Qty{150});
}

OT_TEST(marketable_sell_walks_bids_and_ioc_remainder_is_canceled) {
    Rig rig;
    rig.add(1, Side::buy, 100, 1'000'000, 1);
    rig.add(2, Side::buy, 60, 999'950, 2);
    rig.add(3, Side::buy, 500, 999'800, 3);
    // IOC sell 200 at limit 99.9900: 100 @100.0000, 60 @99.9950; 99.9800 is below the limit.
    // The remaining 40 are canceled with reason 'I'.
    OT_CHECK(rig.submit(enter(5, Side::sell, 200, 999'900, ouch::kTifIoc), 10));
    const auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{4});
    OT_CHECK_EQ(reps[0].type, 'A');
    OT_CHECK_EQ(reps[1].exe.shares, Qty{100});
    OT_CHECK_EQ(reps[1].exe.price, Price{1'000'000});
    OT_CHECK_EQ(reps[2].exe.shares, Qty{60});
    OT_CHECK_EQ(reps[2].exe.price, Price{999'950});
    OT_CHECK_EQ(reps[3].type, 'C');
    OT_CHECK_EQ(reps[3].can.decrement, Qty{40});
    OT_CHECK_EQ(reps[3].can.reason, 'I');
    OT_CHECK(reps[3].can.token == tok(5));
    OT_CHECK_EQ(rig.sim.resting_orders(), std::size_t{0});
}

OT_TEST(ioc_with_nothing_to_hit_is_accepted_then_canceled) {
    Rig rig;
    rig.add(1, Side::sell, 100, 1'000'100, 1);
    OT_CHECK(rig.submit(enter(1, Side::buy, 30, 1'000'000, ouch::kTifIoc), 2));
    const auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{2});
    OT_CHECK_EQ(reps[0].type, 'A');
    OT_CHECK_EQ(reps[1].type, 'C');
    OT_CHECK_EQ(reps[1].can.decrement, Qty{30});
    OT_CHECK_EQ(reps[1].can.reason, 'I');
}

OT_TEST(fill_is_capped_by_displayed_size_and_day_remainder_rests) {
    Rig rig;
    rig.add(1, Side::sell, 40, 1'000'000, 1);
    OT_CHECK(rig.submit(enter(1, Side::buy, 100, 1'000'000), 2));
    const auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{2});
    OT_CHECK_EQ(reps[1].exe.shares, Qty{40});
    OT_CHECK_EQ(rig.sim.resting_orders(), std::size_t{1});  // 60 rest at 100.0000
}

// ---- queue position ---------------------------------------------------------------------

OT_TEST(passive_order_fills_only_after_queue_ahead_is_consumed) {
    Rig rig;
    rig.add(10, Side::buy, 100, kP100, 1);
    rig.add(11, Side::buy, 50, kP100, 2);
    rig.add(12, Side::sell, 1000, 1'000'500, 3);
    OT_CHECK(rig.submit(enter(1, Side::buy, 40, kP100), 4));
    auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});  // Accepted only; 150 shares are queued ahead of us

    rig.exec(10, 100, 10);  // ahead 150 -> 50
    OT_CHECK_EQ(take(rig.sim).size(), std::size_t{0});

    // Other participants cancelling never moves us up: 20 of ref 11 cancelled, then the rest
    // deleted, and 50 shares are still counted ahead.
    rig.cancel(11, 20, 11);
    rig.del(11, 12);
    OT_CHECK_EQ(take(rig.sim).size(), std::size_t{0});

    rig.add(13, Side::buy, 100, kP100, 13);  // joins behind us
    rig.exec(13, 30, 14);                    // ahead 50 -> 20, still no fill
    OT_CHECK_EQ(take(rig.sim).size(), std::size_t{0});
    rig.exec(13, 30, 15);                    // 30 > 20: our first 10 shares
    reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK_EQ(reps[0].type, 'E');
    OT_CHECK_EQ(reps[0].exe.shares, Qty{10});
    OT_CHECK_EQ(reps[0].exe.price, kP100);
    OT_CHECK_EQ(reps[0].exe.liquidity, 'A');
    OT_CHECK_EQ(reps[0].exe.ts, Nanos{15});
    OT_CHECK_EQ(rig.sim.resting_orders(), std::size_t{1});

    rig.exec(13, 40, 16);  // 40 available, 30 left on our order
    reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK_EQ(reps[0].exe.shares, Qty{30});
    OT_CHECK_EQ(reps[0].exe.match, std::uint64_t{2});
    OT_CHECK_EQ(rig.sim.resting_orders(), std::size_t{0});
    OT_CHECK_EQ(rig.sim.fills(), std::uint64_t{2});
}

OT_TEST(executions_at_other_prices_and_hidden_trades_do_not_fill) {
    Rig rig;
    rig.add(10, Side::buy, 50, kP100, 1);
    rig.add(11, Side::buy, 50, kP100 - 100, 2);
    rig.add(12, Side::sell, 500, 1'000'500, 3);
    OT_CHECK(rig.submit(enter(1, Side::buy, 10, kP100), 4));
    take(rig.sim);
    rig.exec(11, 50, 5);        // a worse bid level
    rig.trade(1000, kP100, 6);  // non-displayed trade at our price
    rig.exec(12, 100, 7);       // the other side of the book
    OT_CHECK_EQ(take(rig.sim).size(), std::size_t{0});
    OT_CHECK_EQ(rig.sim.fills(), std::uint64_t{0});
    rig.exec(10, 50, 8);        // queue ahead exhausted exactly: still no fill
    OT_CHECK_EQ(take(rig.sim).size(), std::size_t{0});
}

OT_TEST(executed_with_price_fills_at_the_c_price) {
    Rig rig;
    rig.add(1, Side::buy, 10, 999'000, 1);
    rig.add(20, Side::sell, 20, 1'000'100, 2);
    rig.add(21, Side::sell, 50, 1'000'100, 3);
    OT_CHECK(rig.submit(enter(1, Side::sell, 30, 1'000'100), 4));  // ahead = 70
    take(rig.sim);
    rig.exec_price(20, 20, 1'000'090, 5);  // ahead 70 -> 50
    rig.exec_price(21, 45, 1'000'090, 6);  // 50 -> 5
    rig.exec_price(21, 5, 1'000'095, 7);   // 5 -> 0, exactly exhausted
    OT_CHECK_EQ(take(rig.sim).size(), std::size_t{0});
    rig.add(22, Side::sell, 100, 1'000'100, 8);
    rig.exec_price(22, 12, 1'000'080, 9);  // 12 fill us, at the C price, not our limit
    auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK_EQ(reps[0].exe.shares, Qty{12});
    OT_CHECK_EQ(reps[0].exe.price, Price{1'000'080});
    rig.exec(22, 50, 10);  // E fills at the limit: the remaining 18
    reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK_EQ(reps[0].exe.shares, Qty{18});
    OT_CHECK_EQ(reps[0].exe.price, Price{1'000'100});
}

OT_TEST(one_execution_is_split_over_our_orders_in_arrival_order) {
    Rig rig;
    rig.add(10, Side::buy, 5, kP100, 1);
    rig.add(11, Side::sell, 1000, 1'000'500, 2);
    OT_CHECK(rig.submit(enter(1, Side::buy, 10, kP100), 3));  // ahead 5
    OT_CHECK(rig.submit(enter(2, Side::buy, 10, kP100), 4));  // ahead 5 + 10 = 15
    rig.add(12, Side::buy, 100, kP100, 5);
    take(rig.sim);
    // 22 shares execute: 5 are ahead of order 1, its 10 are filled, order 2 needs 15 ahead of
    // it cleared and gets the last 22 - 15 = 7.
    rig.exec(12, 22, 6);
    const auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{2});
    OT_CHECK(reps[0].exe.token == tok(1));
    OT_CHECK_EQ(reps[0].exe.shares, Qty{10});
    OT_CHECK(reps[1].exe.token == tok(2));
    OT_CHECK_EQ(reps[1].exe.shares, Qty{7});
}

// ---- latency ----------------------------------------------------------------------------

OT_TEST(order_is_invisible_until_it_arrives_and_reports_follow_report_latency) {
    Rig rig(50'000, 30'000);
    rig.add(1, Side::sell, 100, 1'000'000, 1);
    OT_CHECK(rig.sim.send(enter(1, Side::buy, 100, 1'000'000), 1'000'000));  // arrives at 1'050'000
    rig.del(1, 1'020'000);      // liquidity vanishes while the order is in flight
    rig.sim.advance(1'049'999);
    OT_CHECK_EQ(rig.sim.pending_reports(), std::size_t{0});
    OT_CHECK_EQ(rig.sim.resting_orders(), std::size_t{0});
    rig.sim.advance(1'050'000);  // arrival: nothing to hit, so it rests
    OT_CHECK_EQ(rig.sim.pending_reports(), std::size_t{1});
    OT_CHECK_EQ(rig.sim.resting_orders(), std::size_t{1});
    OT_CHECK_EQ(take(rig.sim, 1'079'999).size(), std::size_t{0});
    const auto reps = take(rig.sim, 1'080'000);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK_EQ(reps[0].deliver, Nanos{1'080'000});
    OT_CHECK_EQ(reps[0].acc.ts, Nanos{1'050'000});
}

OT_TEST(arrival_at_a_message_timestamp_sees_the_book_before_that_message) {
    Rig rig(50'000, 30'000);
    rig.add(1, Side::sell, 100, 1'000'000, 1);
    OT_CHECK(rig.sim.send(enter(1, Side::buy, 100, 1'000'000), 1'000'000));
    rig.del(1, 1'050'000);  // same nanosecond as the arrival: the order gets there first
    const auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{2});
    OT_CHECK_EQ(reps[0].type, 'A');
    OT_CHECK_EQ(reps[1].type, 'E');
    OT_CHECK_EQ(reps[1].exe.ts, Nanos{1'050'000});
    OT_CHECK_EQ(reps[1].deliver, Nanos{1'080'000});
}

OT_TEST(fills_caused_by_market_messages_are_stamped_with_the_message_time) {
    Rig rig(50'000, 30'000);
    rig.add(10, Side::buy, 10, kP100, 1);
    rig.add(11, Side::sell, 1000, 1'000'500, 2);
    OT_CHECK(rig.sim.send(enter(1, Side::buy, 5, kP100), 100'000));
    rig.sim.advance(150'000);  // arrival, ahead = 10
    rig.add(12, Side::buy, 100, kP100, 200'000);
    rig.exec(12, 15, 2'000'000);  // 10 clear the queue, 5 fill us
    const auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{2});
    OT_CHECK_EQ(reps[0].deliver, Nanos{180'000});
    OT_CHECK_EQ(reps[1].type, 'E');
    OT_CHECK_EQ(reps[1].exe.ts, Nanos{2'000'000});
    OT_CHECK_EQ(reps[1].deliver, Nanos{2'030'000});
}

OT_TEST(a_stale_clock_never_reorders_reports) {
    Rig rig(0, 10);
    rig.add(1, Side::sell, 100, 1'000'000, 5'000);
    // Sent with a timestamp older than the feed's: the event clock does not run backwards.
    OT_CHECK(rig.sim.send(enter(1, Side::buy, 10, 1'000'000), 100));
    rig.sim.advance(100);
    const auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{2});
    OT_CHECK(reps[0].acc.ts >= 5'000);
    OT_CHECK(reps[1].deliver >= reps[0].deliver);
}

// ---- cancel and replace -----------------------------------------------------------------

OT_TEST(cancel_removes_the_order_and_stale_cancels_are_ignored) {
    Rig rig;
    rig.add(10, Side::buy, 5, kP100, 1);
    rig.add(11, Side::sell, 1000, 1'000'500, 2);
    OT_CHECK(rig.submit(enter(1, Side::buy, 40, kP100), 3));
    take(rig.sim);
    OT_CHECK(rig.cancel_order(1, 0, 4));
    auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK_EQ(reps[0].type, 'C');
    OT_CHECK_EQ(reps[0].can.decrement, Qty{40});
    OT_CHECK_EQ(reps[0].can.reason, 'U');
    OT_CHECK_EQ(rig.sim.resting_orders(), std::size_t{0});

    OT_CHECK(rig.cancel_order(1, 0, 5));    // already gone
    OT_CHECK(rig.cancel_order(99, 0, 6));   // never existed
    rig.add(12, Side::buy, 100, kP100, 7);
    rig.exec(12, 50, 8);                    // would have filled the cancelled order
    OT_CHECK_EQ(take(rig.sim).size(), std::size_t{0});
}

OT_TEST(cancel_of_a_filled_order_is_ignored) {
    Rig rig;
    rig.add(1, Side::sell, 100, 1'000'000, 1);
    OT_CHECK(rig.submit(enter(1, Side::buy, 100, 1'000'000), 2));
    take(rig.sim);
    OT_CHECK(rig.cancel_order(1, 0, 3));
    OT_CHECK_EQ(take(rig.sim).size(), std::size_t{0});
}

OT_TEST(partial_cancel_reduces_intended_size_and_keeps_queue_position) {
    Rig rig;
    rig.add(10, Side::buy, 0 + 4, kP100, 1);
    rig.add(11, Side::sell, 1000, 1'000'500, 2);
    OT_CHECK(rig.submit(enter(1, Side::buy, 100, kP100), 3));  // ahead 4
    rig.add(12, Side::buy, 500, kP100, 4);
    take(rig.sim);
    OT_CHECK(rig.cancel_order(1, 200, 5));  // cannot grow an order: ignored
    OT_CHECK_EQ(take(rig.sim).size(), std::size_t{0});
    OT_CHECK(rig.cancel_order(1, 60, 6));   // intended size 60 -> cancel 40
    auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK_EQ(reps[0].can.decrement, Qty{40});
    rig.exec(10, 4, 7);    // queue ahead exhausted exactly
    rig.exec(12, 30, 8);   // 30 fill us: position kept
    reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK_EQ(reps[0].exe.shares, Qty{30});
    // 30 executed, 30 open. An intended size of 50 keeps 20 open: cancel 10.
    OT_CHECK(rig.cancel_order(1, 50, 9));
    reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK_EQ(reps[0].can.decrement, Qty{10});
    // An intended size below what already executed cancels the rest.
    OT_CHECK(rig.cancel_order(1, 10, 10));
    reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK_EQ(reps[0].can.decrement, Qty{20});
    OT_CHECK_EQ(rig.sim.resting_orders(), std::size_t{0});
}

OT_TEST(cancelling_an_earlier_order_moves_up_the_orders_behind_it) {
    Rig rig;
    rig.add(10, Side::buy, 5, kP100, 1);
    rig.add(11, Side::sell, 1000, 1'000'500, 2);
    OT_CHECK(rig.submit(enter(1, Side::buy, 10, kP100), 3));  // ahead 5
    OT_CHECK(rig.submit(enter(2, Side::buy, 10, kP100), 4));  // ahead 15
    rig.add(12, Side::buy, 100, kP100, 5);
    take(rig.sim);
    OT_CHECK(rig.cancel_order(1, 0, 6));  // order 2 now has only the 5 displayed shares ahead
    take(rig.sim);
    rig.exec(12, 6, 7);                   // 5 clear the queue, 1 fills order 2
    const auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK(reps[0].exe.token == tok(2));
    OT_CHECK_EQ(reps[0].exe.shares, Qty{1});
}

OT_TEST(replace_loses_time_priority_and_reports_open_quantity) {
    Rig rig;
    rig.add(1, Side::buy, 50, kP100, 1);
    rig.add(9, Side::sell, 1000, 1'000'500, 2);
    OT_CHECK(rig.submit(enter(1, Side::buy, 10, kP100), 3));  // A: ahead 50
    OT_CHECK(rig.submit(enter(2, Side::buy, 10, kP100), 4));  // B: ahead 60
    take(rig.sim);
    OT_CHECK(rig.replace_order(1, 3, 10, kP100, 5));          // A -> token 3, back of the queue
    auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK_EQ(reps[0].type, 'U');
    OT_CHECK(reps[0].rep.a.token == tok(3));
    OT_CHECK(reps[0].rep.previous == tok(1));
    OT_CHECK_EQ(reps[0].rep.a.shares, Qty{10});
    OT_CHECK_EQ(reps[0].rep.a.price, kP100);
    OT_CHECK_EQ(reps[0].rep.a.order_state, 'L');
    OT_CHECK(reps[0].rep.a.ref != 0);

    rig.add(2, Side::buy, 100, kP100, 6);
    rig.exec(1, 50, 7);  // B ahead 0, A' ahead 10
    OT_CHECK_EQ(take(rig.sim).size(), std::size_t{0});
    rig.exec(2, 10, 8);  // B fills; without priority loss A would have been first
    reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK(reps[0].exe.token == tok(2));
    OT_CHECK_EQ(reps[0].exe.shares, Qty{10});
    rig.exec(2, 5, 9);   // now the replacement
    reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK(reps[0].exe.token == tok(3));
    OT_CHECK_EQ(reps[0].exe.shares, Qty{5});

    // The old token is dead: cancelling it does nothing, cancelling the new one works.
    OT_CHECK(rig.cancel_order(1, 0, 10));
    OT_CHECK_EQ(take(rig.sim).size(), std::size_t{0});

    // Replace carries the executed quantity: total 8 liable, 5 executed, 3 open.
    OT_CHECK(rig.replace_order(3, 4, 8, kP100, 11));
    reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK_EQ(reps[0].type, 'U');
    OT_CHECK_EQ(reps[0].rep.a.shares, Qty{3});
}

OT_TEST(replace_to_a_new_price_requeues_at_that_prices_back) {
    Rig rig;
    rig.add(9, Side::sell, 1000, 1'001'000, 1);
    OT_CHECK(rig.submit(enter(1, Side::buy, 10, kP100), 2));  // nothing displayed: ahead 0
    take(rig.sim);
    rig.add(1, Side::buy, 30, kP100 + 100, 3);                // 100.0100 has 30 displayed
    OT_CHECK(rig.replace_order(1, 2, 10, kP100 + 100, 4));
    take(rig.sim);
    rig.exec(1, 30, 5);   // queue at the new price cleared
    OT_CHECK_EQ(take(rig.sim).size(), std::size_t{0});
    rig.add(2, Side::buy, 20, kP100 + 100, 6);
    rig.exec(2, 4, 7);
    const auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK_EQ(reps[0].exe.shares, Qty{4});
    OT_CHECK_EQ(reps[0].exe.price, Price{kP100 + 100});
}

OT_TEST(replace_to_a_marketable_price_matches_immediately) {
    Rig rig;
    rig.add(1, Side::sell, 50, 1'000'000, 1);
    OT_CHECK(rig.submit(enter(1, Side::buy, 30, 999'000), 2));
    take(rig.sim);
    OT_CHECK(rig.replace_order(1, 2, 30, 1'000'000, 3));
    const auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{2});
    OT_CHECK_EQ(reps[0].type, 'U');
    OT_CHECK_EQ(reps[1].type, 'E');
    OT_CHECK_EQ(reps[1].exe.shares, Qty{30});
    OT_CHECK_EQ(reps[1].exe.price, Price{1'000'000});
    OT_CHECK_EQ(reps[1].exe.liquidity, 'R');
    OT_CHECK(reps[1].exe.token == tok(2));
    OT_CHECK_EQ(rig.sim.resting_orders(), std::size_t{0});
}

OT_TEST(replace_with_invalid_terms_cancels_and_bad_targets_are_ignored) {
    Rig rig;
    rig.add(9, Side::sell, 1000, 1'001'000, 1);
    OT_CHECK(rig.submit(enter(1, Side::buy, 10, kP100), 2));
    OT_CHECK(rig.submit(enter(2, Side::buy, 10, kP100), 3));
    take(rig.sim);
    // Unknown existing token, and a replacement token that is already in use: silently ignored.
    OT_CHECK(rig.replace_order(77, 78, 10, kP100, 4));
    OT_CHECK(rig.replace_order(1, 2, 10, kP100, 5));
    OT_CHECK_EQ(take(rig.sim).size(), std::size_t{0});
    OT_CHECK_EQ(rig.sim.resting_orders(), std::size_t{2});
    // Live order, invalid details (price 0): the order is canceled, as the venue does.
    OT_CHECK(rig.replace_order(1, 5, 10, 0, 6));
    const auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK_EQ(reps[0].type, 'C');
    OT_CHECK(reps[0].can.token == tok(1));
    OT_CHECK_EQ(reps[0].can.decrement, Qty{10});
    OT_CHECK_EQ(rig.sim.resting_orders(), std::size_t{1});
}

// ---- validation -------------------------------------------------------------------------

OT_TEST(invalid_orders_are_rejected_with_the_right_reason) {
    Rig rig;
    rig.add(1, Side::sell, 100, 1'000'000, 1);
    struct Case {
        ouch::EnterOrder order;
        char reason;
    };
    ouch::EnterOrder blank_token = enter(1, Side::buy, 10, kP100);
    blank_token.token = ouch::Token::from_text("");
    ouch::EnterOrder hash_token = enter(1, Side::buy, 10, kP100);
    hash_token.token = ouch::Token::invalid();
    ouch::EnterOrder negative = enter(1, Side::buy, 10, -5);
    const Case cases[] = {
        {enter(1, Side::buy, 10, kP100, ouch::kTifSystemHours, "ZZZZ"), 'S'},
        {enter(2, Side::buy, 10, 0), 'X'},
        {negative, 'X'},
        {enter(3, Side::buy, 10, 2'000'000'001), 'X'},
        {enter(4, Side::buy, 0, kP100), 'O'},
        {enter(5, Side::buy, 1'000'000, kP100), 'Z'},
        {blank_token, 'O'},
        {hash_token, 'O'},
    };
    Nanos t = 10;
    for (const Case& c : cases) {
        OT_CHECK(rig.submit(c.order, t++));
        const auto reps = take(rig.sim);
        OT_CHECK_EQ(reps.size(), std::size_t{1});
        OT_CHECK_EQ(reps[0].type, 'J');
        OT_CHECK_EQ(reps[0].rej.reason, c.reason);
        OT_CHECK(reps[0].rej.token == c.order.token);
    }
    OT_CHECK_EQ(rig.sim.resting_orders(), std::size_t{0});
    OT_CHECK_EQ(rig.sim.fills(), std::uint64_t{0});
    // The largest legal price is accepted (200'000.0000).
    OT_CHECK(rig.submit(enter(9, Side::sell, 10, 2'000'000'000), t++));
    const auto ok = take(rig.sim);
    OT_CHECK_EQ(ok.size(), std::size_t{1});
    OT_CHECK_EQ(ok[0].type, 'A');
}

OT_TEST(duplicate_token_is_rejected_and_the_original_is_untouched) {
    Rig rig;
    rig.add(10, Side::buy, 5, kP100, 1);
    OT_CHECK(rig.submit(enter(1, Side::buy, 10, kP100), 2));
    OT_CHECK_EQ(take(rig.sim).size(), std::size_t{1});
    OT_CHECK(rig.submit(enter(1, Side::sell, 3, 1'200'000), 3));
    const auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK_EQ(reps[0].type, 'J');
    OT_CHECK_EQ(reps[0].rej.reason, 'O');
    OT_CHECK(reps[0].rej.token == tok(1));
    OT_CHECK_EQ(rig.sim.resting_orders(), std::size_t{1});
    rig.add(11, Side::buy, 100, kP100, 4);
    rig.exec(11, 8, 5);  // 5 ahead, then 3 fill the original
    const auto fills = take(rig.sim);
    OT_CHECK_EQ(fills.size(), std::size_t{1});
    OT_CHECK_EQ(fills[0].exe.shares, Qty{3});
}

OT_TEST(orders_on_different_symbols_do_not_interact) {
    Rig rig;
    rig.add(1, Side::sell, 100, 1'000'000, 1, kMsft);
    OT_CHECK(rig.submit(enter(1, Side::buy, 10, 1'000'000), 2));  // AAPL: nothing to hit
    auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK(rig.submit(enter(2, Side::buy, 10, 1'000'000, ouch::kTifSystemHours, "MSFT"), 3));
    reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{2});
    OT_CHECK_EQ(reps[1].type, 'E');
    OT_CHECK(reps[1].exe.token == tok(2));
    OT_CHECK(reps[0].acc.stock == Symbol("MSFT"));
}

// ---- trade-through ----------------------------------------------------------------------

OT_TEST(market_moving_through_a_resting_limit_fills_at_the_limit) {
    Rig rig;
    rig.add(1, Side::sell, 100, 1'000'500, 1);
    rig.add(2, Side::buy, 100, 999'000, 2);
    OT_CHECK(rig.submit(enter(1, Side::buy, 40, kP100), 3));
    OT_CHECK(rig.submit(enter(2, Side::sell, 25, 1'001'000), 4));
    take(rig.sim);
    rig.add(3, Side::sell, 10, 1'000'001, 5);  // above our bid, below our ask: nothing
    OT_CHECK_EQ(take(rig.sim).size(), std::size_t{0});
    rig.add(4, Side::sell, 10, 999'900, 6);    // ask 99.9900 <= our bid 100.0000: filled at 100.0000
    auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK(reps[0].exe.token == tok(1));
    OT_CHECK_EQ(reps[0].exe.shares, Qty{40});  // the whole remaining quantity
    OT_CHECK_EQ(reps[0].exe.price, kP100);
    OT_CHECK_EQ(reps[0].exe.liquidity, 'A');
    OT_CHECK_EQ(rig.sim.resting_orders(), std::size_t{1});

    rig.add(5, Side::buy, 10, 1'001'000, 7);   // bid == our ask: locked counts as through
    reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK(reps[0].exe.token == tok(2));
    OT_CHECK_EQ(reps[0].exe.price, Price{1'001'000});
    OT_CHECK_EQ(rig.sim.resting_orders(), std::size_t{0});
}

OT_TEST(an_itch_replace_that_moves_through_the_limit_also_fills) {
    Rig rig;
    rig.add(1, Side::sell, 100, 1'000'500, 1);
    OT_CHECK(rig.submit(enter(1, Side::buy, 40, kP100), 2));
    take(rig.sim);
    rig.replace(1, 2, 100, 999'500, 3);  // the ask jumps below our bid
    const auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK_EQ(reps[0].exe.shares, Qty{40});
    OT_CHECK_EQ(reps[0].exe.price, kP100);
}

// ---- capacity ---------------------------------------------------------------------------

OT_TEST(full_queues_refuse_or_reject_instead_of_growing) {
    Rig rig(0, 0, 2);
    rig.add(9, Side::sell, 1000, 1'001'000, 1);
    OT_CHECK(rig.sim.send(enter(1, Side::buy, 10, kP100), 2));
    OT_CHECK(rig.sim.send(enter(2, Side::buy, 10, kP100), 2));
    OT_CHECK(!rig.sim.send(enter(3, Side::buy, 10, kP100), 2));  // request queue full
    rig.sim.advance(2);
    take(rig.sim);
    OT_CHECK_EQ(rig.sim.resting_orders(), std::size_t{2});
    OT_CHECK(rig.submit(enter(3, Side::buy, 10, kP100), 3));     // order table full at arrival
    const auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{1});
    OT_CHECK_EQ(reps[0].type, 'J');
    OT_CHECK_EQ(reps[0].rej.reason, 'O');
    // A finished order frees its slot.
    OT_CHECK(rig.cancel_order(1, 0, 4));
    take(rig.sim);
    OT_CHECK(rig.submit(enter(4, Side::buy, 10, kP100), 5));
    const auto after = take(rig.sim);
    OT_CHECK_EQ(after.size(), std::size_t{1});
    OT_CHECK_EQ(after[0].type, 'A');
}

OT_TEST(report_queue_overflow_is_counted_not_grown) {
    Rig rig(0, 0, 4);  // report queue holds max(1024, 8) = 1024
    for (std::uint64_t i = 1; i <= 1100; ++i) {
        OT_CHECK(rig.submit(enter(i, Side::buy, 10, kP100, ouch::kTifSystemHours, "NOPE"), i));
    }
    OT_CHECK_EQ(rig.sim.pending_reports(), std::size_t{1024});
    OT_CHECK_EQ(rig.sim.dropped_reports(), std::uint64_t{76});
    OT_CHECK_EQ(take(rig.sim).size(), std::size_t{1024});
    OT_CHECK_EQ(rig.sim.pending_reports(), std::size_t{0});
}

OT_TEST(malformed_and_irrelevant_feed_messages_are_ignored) {
    Rig rig;
    rig.sim.on_itch({}, 1);
    const std::array<std::byte, 3> junk{std::byte{'Z'}, std::byte{1}, std::byte{2}};
    rig.sim.on_itch(junk, 2);
    rig.exec(12345, 10, 3);  // unknown ref
    rig.del(999, 4);
    OT_CHECK_EQ(rig.sim.pending_reports(), std::size_t{0});
    OT_CHECK(rig.submit(enter(1, Side::buy, 10, kP100), 5));  // still works afterwards
    const auto after = take(rig.sim);
    OT_CHECK_EQ(after.size(), std::size_t{1});
    OT_CHECK_EQ(after[0].type, 'A');
}

// ---- wire bytes -------------------------------------------------------------------------

std::uint64_t be_at(const std::vector<std::byte>& b, std::size_t off, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = (v << 8) | std::to_integer<unsigned>(b[off + static_cast<std::size_t>(i)]);
    return v;
}

OT_TEST(report_bytes_match_the_ouch_tables) {
    Rig rig(0, 0);
    rig.add(1, Side::sell, 100, 1'000'000, 1);
    // Enter order: token "00000000000007", buy 100, AAPL, 100.0000, system hours.
    OT_CHECK(rig.submit(enter(7, Side::buy, 100, 1'000'000), 0x1388));
    const auto reps = take(rig.sim);
    OT_CHECK_EQ(reps.size(), std::size_t{2});

    const std::vector<std::byte>& a = reps[0].raw;  // Accepted, 66 bytes
    OT_CHECK_EQ(a.size(), std::size_t{66});
    OT_CHECK_EQ(std::to_integer<char>(a[0]), 'A');
    OT_CHECK_EQ(be_at(a, 1, 8), std::uint64_t{0x1388});      // timestamp
    OT_CHECK_EQ(std::to_integer<char>(a[9 + 13]), '7');      // token @9, last digit
    OT_CHECK_EQ(std::to_integer<char>(a[9]), '0');
    OT_CHECK_EQ(std::to_integer<char>(a[23]), 'B');          // side
    OT_CHECK_EQ(be_at(a, 24, 4), std::uint64_t{100});        // shares
    OT_CHECK_EQ(std::to_integer<char>(a[28]), 'A');          // stock "AAPL    "
    OT_CHECK_EQ(std::to_integer<char>(a[32]), ' ');
    OT_CHECK_EQ(be_at(a, 36, 4), std::uint64_t{0x000F4240});  // 1'000'000
    OT_CHECK_EQ(be_at(a, 40, 4), std::uint64_t{99999});      // time in force
    OT_CHECK_EQ(std::to_integer<char>(a[64]), 'L');          // order state

    const std::vector<std::byte>& e = reps[1].raw;  // Executed, 40 bytes
    OT_CHECK_EQ(e.size(), std::size_t{40});
    OT_CHECK_EQ(std::to_integer<char>(e[0]), 'E');
    OT_CHECK_EQ(be_at(e, 1, 8), std::uint64_t{0x1388});
    OT_CHECK_EQ(std::to_integer<char>(e[9 + 13]), '7');
    OT_CHECK_EQ(be_at(e, 23, 4), std::uint64_t{100});        // executed shares
    OT_CHECK_EQ(be_at(e, 27, 4), std::uint64_t{0x000F4240}); // price
    OT_CHECK_EQ(std::to_integer<char>(e[31]), 'R');          // liquidity flag
    OT_CHECK_EQ(be_at(e, 32, 8), std::uint64_t{1});          // match number
}

// ---- randomized -------------------------------------------------------------------------

// Closed-form model of one bid level with no asks. Order j (arrival order) fills
//   clamp(S_j - ahead0_j, 0, size_j)
// where S_j is the displayed execution volume at the level since j arrived and ahead0_j is the
// displayed size at arrival plus the still-open size of our earlier orders. The reference keeps
// its own tally of the displayed size and never looks at the simulator's queue.
OT_TEST(queue_model_matches_closed_form_reference) {
    for (std::uint64_t seed = 1; seed <= 300; ++seed) {
        Rng rng(seed);
        Rig rig(0, 0, 256);
        rig.add(1, Side::sell, 1, 1'900'000, 1);  // far ask so nothing crosses
        struct Mine {
            Qty size{};
            std::uint64_t ahead0{};
            std::uint64_t seen{};  // displayed volume executed since arrival
            std::uint64_t id{};
        };
        struct Market {
            OrderRef ref{};
            Qty qty{};
        };
        std::vector<Mine> mine;
        std::vector<Market> market;
        std::uint64_t displayed = 0;
        OrderRef next_ref = 100;
        std::uint64_t next_id = 1;
        std::map<std::uint64_t, Qty> got;
        Nanos t = 10;

        auto cum = [&](const Mine& m) {
            const std::uint64_t s = m.seen > m.ahead0 ? m.seen - m.ahead0 : 0;
            return std::min<std::uint64_t>(s, m.size);
        };
        for (int step = 0; step < 60; ++step) {
            ++t;
            const std::uint64_t pick = rng.bounded(10);
            if (pick < 3 || market.empty()) {
                const Qty q = static_cast<Qty>(rng.range(1, 40));
                rig.add(next_ref, Side::buy, q, kP100, t);
                market.push_back({next_ref++, q});
                displayed += q;
            } else if (pick < 6) {
                const std::size_t i = rng.bounded(market.size());
                const Qty s = static_cast<Qty>(rng.range(1, market[i].qty));
                rig.exec(market[i].ref, s, t);
                market[i].qty -= s;
                displayed -= s;
                for (Mine& m : mine) m.seen += s;
                if (market[i].qty == 0) market.erase(market.begin() + static_cast<std::ptrdiff_t>(i));
            } else if (pick < 7) {
                const std::size_t i = rng.bounded(market.size());
                const Qty s = static_cast<Qty>(rng.range(1, market[i].qty));
                rig.cancel(market[i].ref, s, t);  // others' cancels: displayed shrinks, our ahead does not
                market[i].qty -= s;
                displayed -= s;
                if (market[i].qty == 0) market.erase(market.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                Mine m;
                m.size = static_cast<Qty>(rng.range(1, 30));
                m.id = next_id++;
                m.ahead0 = displayed;
                for (const Mine& e : mine) m.ahead0 += e.size - cum(e);
                OT_CHECK(rig.submit(enter(m.id, Side::buy, m.size, kP100), t));
                mine.push_back(m);
            }
            for (const auto& r : take(rig.sim)) {
                if (r.type == 'E') got[*r.exe.token.to_id()] += r.exe.shares;
            }
            for (const Mine& m : mine) {
                OT_CHECK_EQ(static_cast<std::uint64_t>(got[m.id]), cum(m));
            }
        }
    }
}

// Random mixed traffic over two symbols. Returns a digest of every delivered report and its
// delivery time; checks per-report invariants along the way.
struct RunResult {
    std::uint64_t digest{};
    std::uint64_t fills{};
    std::size_t reports{};
};

RunResult run_random(std::uint64_t seed, bool drain_each_step) {
    Rng rng(seed);
    Rig rig(700, 300, 4096);
    struct Mo {
        OrderRef ref{};
        Locate loc{};
        Qty qty{};
    };
    std::vector<Mo> market;
    OrderRef next_ref = 1;
    std::uint64_t next_tok = 1;
    Digest digest;
    RunResult res;

    struct Open {
        std::int64_t open{};
        Price limit{};
        Side side{};
    };
    std::map<std::uint64_t, Open> orders;
    Nanos last_deliver = 0;

    auto consume = [&](Nanos now) {
        rig.sim.drain_reports(now, [&](std::span<const std::byte> msg, Nanos deliver) {
            OT_CHECK(deliver <= now);
            OT_CHECK(deliver >= last_deliver);
            last_deliver = deliver;
            digest.update(deliver);
            digest.update(msg.data(), msg.size());
            ++res.reports;
            Rep r;
            Decoder d{&r};
            OT_CHECK_EQ(ouch::decode_outbound(msg, d), DecodeStatus::ok);
            switch (r.type) {
                case 'A':
                    orders[*r.acc.token.to_id()] = {r.acc.shares, r.acc.price, r.acc.side};
                    break;
                case 'U': {
                    const auto prev = orders.find(*r.rep.previous.to_id());
                    OT_CHECK(prev != orders.end());
                    if (prev == orders.end()) break;
                    Open o = prev->second;
                    orders.erase(prev);
                    o.open = r.rep.a.shares;
                    o.limit = r.rep.a.price;
                    orders[*r.rep.a.token.to_id()] = o;
                    break;
                }
                case 'C': {
                    const auto it = orders.find(*r.can.token.to_id());
                    OT_CHECK(it != orders.end());
                    if (it == orders.end()) break;
                    OT_CHECK(r.can.decrement > 0);
                    it->second.open -= r.can.decrement;
                    OT_CHECK(it->second.open >= 0);
                    break;
                }
                case 'E': {
                    const auto it = orders.find(*r.exe.token.to_id());
                    OT_CHECK(it != orders.end());
                    if (it == orders.end()) break;
                    OT_CHECK(r.exe.shares > 0);
                    it->second.open -= r.exe.shares;
                    OT_CHECK(it->second.open >= 0);
                    if (r.exe.liquidity == 'R') {  // took liquidity: never worse than the limit
                        if (it->second.side == Side::buy) OT_CHECK(r.exe.price <= it->second.limit);
                        else OT_CHECK(r.exe.price >= it->second.limit);
                    }
                    break;
                }
                case 'J':
                    break;
                default:
                    OT_CHECK(false);
            }
        });
    };

    Nanos t = 1000;
    for (int step = 0; step < 2000; ++step) {
        t += 1 + rng.bounded(400);
        const std::uint64_t pick = rng.bounded(100);
        const Locate loc = rng.chance(1, 4) ? kMsft : kAapl;
        const Price mid = 1'000'000;
        if (pick < 30 || market.empty()) {
            const Side s = rng.chance(1, 2) ? Side::buy : Side::sell;
            const Price px = mid + (s == Side::buy ? rng.range(-6, 2) : rng.range(-2, 6)) * 100;
            const Qty q = static_cast<Qty>(rng.range(1, 60));
            rig.add(next_ref, s, q, px, t, loc);
            market.push_back({next_ref++, loc, q});
        } else if (pick < 45) {
            const std::size_t i = rng.bounded(market.size());
            const Qty s = static_cast<Qty>(rng.range(1, market[i].qty));
            rig.exec(market[i].ref, s, t);
            market[i].qty -= s;
            if (market[i].qty == 0) market.erase(market.begin() + static_cast<std::ptrdiff_t>(i));
        } else if (pick < 50) {
            const std::size_t i = rng.bounded(market.size());
            const Qty s = static_cast<Qty>(rng.range(1, market[i].qty));
            rig.exec_price(market[i].ref, s, mid + rng.range(-5, 5) * 100, t);
            market[i].qty -= s;
            if (market[i].qty == 0) market.erase(market.begin() + static_cast<std::ptrdiff_t>(i));
        } else if (pick < 58) {
            const std::size_t i = rng.bounded(market.size());
            const Qty s = static_cast<Qty>(rng.range(1, market[i].qty));
            rig.cancel(market[i].ref, s, t);
            market[i].qty -= s;
            if (market[i].qty == 0) market.erase(market.begin() + static_cast<std::ptrdiff_t>(i));
        } else if (pick < 63) {
            const std::size_t i = rng.bounded(market.size());
            rig.del(market[i].ref, t);
            market.erase(market.begin() + static_cast<std::ptrdiff_t>(i));
        } else if (pick < 67) {
            const std::size_t i = rng.bounded(market.size());
            const Qty q = static_cast<Qty>(rng.range(1, 60));
            rig.replace(market[i].ref, next_ref, q, mid + rng.range(-6, 6) * 100, t);
            market[i] = {next_ref++, market[i].loc, q};
        } else if (pick < 69) {
            rig.trade(10, mid, t);
        } else if (pick < 84) {
            const Side s = rng.chance(1, 2) ? Side::buy : Side::sell;
            const Price px = mid + rng.range(-6, 6) * 100;
            const Qty q = static_cast<Qty>(rng.range(1, 40));
            const std::uint32_t tif = rng.chance(1, 4) ? ouch::kTifIoc : ouch::kTifSystemHours;
            const char* sym = loc == kMsft ? "MSFT" : "AAPL";
            ouch::EnterOrder e = enter(next_tok++, s, q, px, tif, sym);
            if (rng.chance(1, 40)) e.stock = Symbol("NOPE");
            rig.sim.send(e, t);
        } else if (pick < 91) {
            const std::uint64_t id = 1 + rng.bounded(next_tok);
            rig.sim.send(ouch::CancelOrder{tok(id), static_cast<Qty>(rng.bounded(3) == 0 ? 0 : rng.range(1, 40))}, t);
        } else if (pick < 97) {
            ouch::ReplaceOrder r;
            r.existing = tok(1 + rng.bounded(next_tok));
            r.replacement = tok(next_tok++);
            r.shares = static_cast<Qty>(rng.range(1, 60));
            r.price = mid + rng.range(-6, 6) * 100;
            rig.sim.send(r, t);
        }
        rig.sim.advance(t);
        if (drain_each_step) consume(t);
    }
    consume(kAll);
    res.digest = digest.value();
    res.fills = rig.sim.fills();
    OT_CHECK_EQ(rig.sim.dropped_reports(), std::uint64_t{0});
    return res;
}

OT_TEST(random_traffic_keeps_reports_consistent) {
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        const RunResult r = run_random(seed, true);
        OT_CHECK(r.reports > 500);
        OT_CHECK(r.fills > 20);
    }
}

OT_TEST(replay_is_deterministic_and_independent_of_drain_timing) {
    const RunResult a = run_random(42, true);
    const RunResult b = run_random(42, true);
    const RunResult c = run_random(42, false);  // drain everything once at the end
    OT_CHECK_EQ(a.digest, b.digest);
    OT_CHECK_EQ(a.digest, c.digest);
    OT_CHECK_EQ(a.reports, c.reports);
    OT_CHECK_EQ(a.fills, c.fills);
    OT_CHECK(a.fills > 20);
    const RunResult other = run_random(43, true);
    OT_CHECK(other.digest != a.digest);
}

}  // namespace

OT_TEST_MAIN()

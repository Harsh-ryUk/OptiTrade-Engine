// Tests for the engine glue.
//
// The scripted tests drive the engine with a recording strategy and a recording gateway, so every
// expectation is a hand-counted callback or counter. Wire messages are built with the codecs, but
// the malformed inputs are hand-mutated bytes. The last group plugs the three real strategies into
// the template, wires the engine to the exchange simulator over a seeded synthetic feed, and checks
// determinism and cross-module consistency; another counts heap allocations on the message path.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <new>
#include <span>
#include <vector>

#include "check.hpp"
#include "optitrade/engine/engine.hpp"
#include "optitrade/itch/encoder.hpp"
#include "optitrade/ouch/codec.hpp"
#include "optitrade/sim/exchange_sim.hpp"
#include "optitrade/sim/synthetic_market.hpp"
#include "optitrade/strategy/ema_cross.hpp"
#include "optitrade/strategy/imbalance_taker.hpp"
#include "optitrade/strategy/microprice_maker.hpp"

using namespace optitrade;

// Sanitizer runtimes bring their own allocator; replacing operator new next to them
// fails to link, so the allocation check only runs in plain builds.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define OT_UNDER_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define OT_UNDER_SANITIZER 1
#endif
#endif

// ---- allocation counter ------------------------------------------------------------------------
// Replaces global operator new so a test can assert that a stretch of code allocates nothing. It is
// only ever read inside a measured window; everything else pays one relaxed increment.

namespace {
std::uint64_t g_allocations = 0;
}

// GCC pairs the replaced operator new with std::free() at inlined call sites and
// warns about a "mismatch" although both replacements below use malloc/free.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

#ifndef OT_UNDER_SANITIZER
void* operator new(std::size_t n) {
    ++g_allocations;
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif

namespace {

constexpr Locate kAapl = 1;
constexpr Locate kMsft = 2;
constexpr Price kPx = 1'000'000;  // 100.0000

// ---- fakes ---------------------------------------------------------------------------------------

struct Gateway final : oms::OrderGateway {
    std::vector<ouch::EnterOrder> enters;
    std::vector<ouch::CancelOrder> cancels;
    std::vector<ouch::ReplaceOrder> replaces;
    std::vector<Nanos> enter_times;
    bool accept{true};

    bool send(const ouch::EnterOrder& m, Nanos now) override {
        if (!accept) return false;
        enters.push_back(m);
        enter_times.push_back(now);
        return true;
    }
    bool send(const ouch::CancelOrder& m, Nanos) override {
        if (!accept) return false;
        cancels.push_back(m);
        return true;
    }
    bool send(const ouch::ReplaceOrder& m, Nanos) override {
        if (!accept) return false;
        replaces.push_back(m);
        return true;
    }
};

// Counts only, so it can sit under the allocation counter.
struct CountingGateway final : oms::OrderGateway {
    std::uint64_t enters{}, cancels{}, replaces{};
    bool send(const ouch::EnterOrder&, Nanos) override { return ++enters != 0; }
    bool send(const ouch::CancelOrder&, Nanos) override { return ++cancels != 0; }
    bool send(const ouch::ReplaceOrder&, Nanos) override { return ++replaces != 0; }
};

// Records every callback and runs an optional test-provided action inside it.
struct Scripted {
    std::vector<Locate> book_calls;
    std::vector<Nanos> book_nows;
    std::vector<oms::Fill> fills;
    std::vector<oms::OrderInfo> updates;
    std::function<void(Locate, const strategy::Context&)> on_book;
    std::function<void(const oms::Fill&, const strategy::Context&)> on_fill_hook;

    void on_book_update(Locate l, const strategy::Context& c) {
        book_calls.push_back(l);
        book_nows.push_back(c.now);
        if (on_book) on_book(l, c);
    }
    void on_fill(const oms::Fill& f, const strategy::Context& c) {
        fills.push_back(f);
        if (on_fill_hook) on_fill_hook(f, c);
    }
    void on_order_update(const oms::OrderInfo& o, const strategy::Context&) { updates.push_back(o); }
};
static_assert(strategy::Strategy<Scripted>);

// Counters only, so it can run inside an allocation-counting window.
struct Quiet {
    std::uint64_t calls{}, fills{}, updates{}, rejected{};
    void on_book_update(Locate l, const strategy::Context& c) {
        if (++calls % 4 != 0) return;
        const oms::SubmitResult r = c.orders.submit({l, Side::buy, kPx, 1, oms::Tif::ioc}, c.now);
        if (r.status != oms::SubmitStatus::ok) ++rejected;
    }
    void on_fill(const oms::Fill&, const strategy::Context&) { ++fills; }
    void on_order_update(const oms::OrderInfo&, const strategy::Context&) { ++updates; }
};
static_assert(strategy::Strategy<Quiet>);

engine::Config small_config() {
    engine::Config c;
    c.books.max_orders = 1024;
    c.books.max_levels_per_side = 16;
    c.books.max_symbols = 16;
    c.oms.max_orders = 256;
    c.max_locates = 8;
    return c;
}

// ---- wire helpers --------------------------------------------------------------------------------

struct Wire {
    std::array<std::byte, 128> bytes{};
    std::size_t n{};
    std::span<const std::byte> view() const { return {bytes.data(), n}; }
};

template <class M>
Wire iw(const M& m) {
    Wire w;
    w.n = itch::encode(m, w.bytes);
    OT_CHECK(w.n != 0);
    return w;
}
template <class M>
Wire ow(const M& m) {
    Wire w;
    w.n = ouch::encode(m, w.bytes);
    OT_CHECK(w.n != 0);
    return w;
}

itch::Header hdr(Locate l, Nanos ts = 0) { return itch::Header{l, 0, ts}; }

Wire directory(Locate l, const char* sym) {
    itch::StockDirectory m;
    m.h = hdr(l);
    m.symbol = Symbol(sym);
    m.market_category = 'Q';
    m.financial_status = 'N';
    m.round_lot_size = 100;
    return iw(m);
}
Wire add(Locate l, OrderRef ref, Side s, Qty q, Price px) {
    itch::AddOrder m;
    m.h = hdr(l);
    m.ref = ref;
    m.side = s;
    m.shares = q;
    m.symbol = Symbol("AAPL");
    m.price = px;
    return iw(m);
}
Wire del(Locate l, OrderRef ref) { return iw(itch::OrderDelete{hdr(l), ref}); }
Wire trade(Locate l, Price px) {
    itch::Trade m;
    m.h = hdr(l);
    m.ref = 0;
    m.side = Side::buy;
    m.shares = 10;
    m.symbol = Symbol("AAPL");
    m.price = px;
    m.match = 1;
    return iw(m);
}

Wire accepted(std::uint64_t token, OrderRef ref, Qty q = 50, Price px = kPx, Side s = Side::buy) {
    ouch::Accepted a;
    a.token = ouch::Token::from_id(token);
    a.side = s;
    a.shares = q;
    a.stock = Symbol("AAPL");
    a.price = px;
    a.ref = ref;
    a.order_state = 'L';
    return ow(a);
}
Wire executed(std::uint64_t token, Qty q, Price px, std::uint64_t match = 1) {
    ouch::Executed e;
    e.token = ouch::Token::from_id(token);
    e.shares = q;
    e.price = px;
    e.match = match;
    return ow(e);
}

// Engine plus its recording gateway, with the common preamble helpers.
template <class S = Scripted>
struct Rig {
    Gateway gw;
    engine::Engine<S> eng;
    Nanos t{1000};

    explicit Rig(const engine::Config& c = small_config()) : eng(c, gw) {}

    DecodeStatus itch(const Wire& w) { return eng.on_itch(w.view(), t += 10); }
    DecodeStatus ouch(const Wire& w) { return eng.on_ouch(w.view(), t += 10); }
    // Two-sided book on AAPL: bid 100.0000 x 100 (ref 1), ask 101.0000 x 100 (ref 2).
    void aapl_book() {
        itch(directory(kAapl, "AAPL"));
        itch(add(kAapl, 1, Side::buy, 100, kPx));
        itch(add(kAapl, 2, Side::sell, 100, 1'010'000));
    }
};

// ---- market data path ------------------------------------------------------------------------------

OT_TEST(strategy_woken_only_by_book_changing_messages_for_the_right_locate) {
    Rig<> r;
    auto& calls = r.eng.strategy().book_calls;

    OT_CHECK_EQ(r.itch(iw(itch::SystemEvent{hdr(0), 'O'})), DecodeStatus::ok);
    OT_CHECK_EQ(r.itch(directory(kAapl, "AAPL")), DecodeStatus::ok);
    OT_CHECK_EQ(r.itch(directory(kMsft, "MSFT")), DecodeStatus::ok);
    OT_CHECK_EQ(calls.size(), std::size_t{0});  // no order flow yet

    r.itch(add(kAapl, 1, Side::buy, 100, kPx));         // call 1: AAPL
    r.itch(add(kAapl, 2, Side::sell, 100, 1'010'000));  // call 2: AAPL
    r.itch(add(kMsft, 3, Side::buy, 100, 500'000));     // call 3: MSFT
    r.itch(trade(kAapl, 1'005'000));                    // trade: no call, but it is the last trade
    OT_CHECK_EQ(calls.size(), std::size_t{3});
    OT_CHECK_EQ(r.eng.books().last_trade(kAapl), Price{1'005'000});

    // Header locate says MSFT, the stored order lives on AAPL: the books act on AAPL, and so must the
    // strategy notification.
    r.itch(iw(itch::OrderExecuted{hdr(kMsft), 1, 40, 7}));                        // call 4: AAPL
    OT_CHECK_EQ(r.eng.books().order(1)->qty, Qty{60});
    r.itch(iw(itch::OrderExecutedPrice{hdr(kAapl), 2, 10, 8, true, 1'010'500}));  // call 5: AAPL
    r.itch(iw(itch::OrderCancel{hdr(kAapl), 1, 20}));                             // call 6: AAPL
    r.itch(iw(itch::OrderReplace{hdr(kAapl), 2, 4, 50, 1'011'000}));              // call 7: AAPL
    r.itch(del(kAapl, 4));                                                        // call 8: AAPL

    // Refused by the books: no strategy call, one book_errors each.
    r.itch(del(kAapl, 999));                              // unknown order
    r.itch(add(kMsft, 3, Side::buy, 100, 500'000));       // duplicate ref
    r.itch(iw(itch::OrderExecuted{hdr(kMsft), 3, 101, 9}));  // more than remains
    r.itch(directory(kMsft, "AAPL"));                        // symbol already bound to locate 1

    const std::vector<Locate> expected{1, 1, 2, 1, 1, 1, 1, 1};
    OT_CHECK(calls == expected);

    const engine::Stats& s = r.eng.stats();
    OT_CHECK_EQ(s.itch_messages, std::uint64_t{16});  // S R R A A A P E C X U D | D A E R
    OT_CHECK_EQ(s.itch_skipped, std::uint64_t{0});
    OT_CHECK_EQ(s.book_updates, std::uint64_t{8});
    OT_CHECK_EQ(s.book_errors, std::uint64_t{4});
    OT_CHECK_EQ(r.eng.books().applied(), std::uint64_t{8});
    OT_CHECK_EQ(s.orders_sent, std::uint64_t{0});
}

OT_TEST(strategy_clock_is_the_message_time) {
    Rig<> r;
    r.itch(directory(kAapl, "AAPL"));
    r.eng.on_itch(add(kAapl, 1, Side::buy, 100, kPx).view(), 123456);
    OT_CHECK_EQ(r.eng.strategy().book_nows.size(), std::size_t{1});
    OT_CHECK_EQ(r.eng.strategy().book_nows[0], Nanos{123456});
}

OT_TEST(unsupported_and_malformed_itch_are_counted_and_have_no_side_effects) {
    Rig<> r;
    r.aapl_book();
    const engine::Stats before = r.eng.stats();
    const std::size_t calls_before = r.eng.strategy().book_calls.size();
    const std::size_t live_before = r.eng.books().live_orders();
    const std::uint64_t applied_before = r.eng.books().applied();

    // Stock Trading Action 'H' exists in ITCH 5.0 but is not supported here.
    std::array<std::byte, 25> h{};
    h[0] = std::byte{'H'};
    OT_CHECK_EQ(r.eng.on_itch(h, 1), DecodeStatus::unknown_type);

    OT_CHECK_EQ(r.eng.on_itch({}, 1), DecodeStatus::truncated);

    Wire a = add(kAapl, 50, Side::buy, 10, kPx);
    OT_CHECK_EQ(a.n, std::size_t{36});
    OT_CHECK_EQ(r.eng.on_itch(std::span<const std::byte>(a.bytes.data(), 35), 1), DecodeStatus::truncated);
    OT_CHECK_EQ(r.eng.on_itch(std::span<const std::byte>(a.bytes.data(), 37), 1), DecodeStatus::bad_length);

    Wire bad_side = a;
    bad_side.bytes[19] = std::byte{'X'};  // side field
    OT_CHECK_EQ(r.eng.on_itch(bad_side.view(), 1), DecodeStatus::bad_field);

    Wire zero_shares = a;
    for (int i = 20; i < 24; ++i) zero_shares.bytes[static_cast<std::size_t>(i)] = std::byte{0};
    OT_CHECK_EQ(r.eng.on_itch(zero_shares.view(), 1), DecodeStatus::bad_field);

    const engine::Stats& s = r.eng.stats();
    OT_CHECK_EQ(s.itch_skipped, std::uint64_t{6});
    OT_CHECK_EQ(s.itch_messages, before.itch_messages);
    OT_CHECK_EQ(s.book_updates, before.book_updates);
    OT_CHECK_EQ(s.book_errors, before.book_errors);
    OT_CHECK_EQ(r.eng.strategy().book_calls.size(), calls_before);
    OT_CHECK_EQ(r.eng.books().live_orders(), live_before);
    OT_CHECK_EQ(r.eng.books().applied(), applied_before);
    OT_CHECK(r.eng.books().order(50) == nullptr);
}

OT_TEST(locates_beyond_max_locates_feed_the_books_but_not_the_strategy) {
    engine::Config c = small_config();
    c.max_locates = 4;
    Rig<> r(c);
    r.itch(directory(9, "ZZZ"));
    r.itch(directory(3, "AAPL"));
    r.itch(add(9, 1, Side::buy, 10, kPx));
    OT_CHECK_EQ(r.eng.books().applied(), std::uint64_t{1});
    OT_CHECK_EQ(r.eng.stats().book_updates, std::uint64_t{1});
    OT_CHECK_EQ(r.eng.strategy().book_calls.size(), std::size_t{0});
    r.itch(add(3, 2, Side::buy, 10, kPx));
    OT_CHECK_EQ(r.eng.strategy().book_calls.size(), std::size_t{1});
    OT_CHECK_EQ(r.eng.strategy().book_calls[0], Locate{3});
}

// ---- order path ----------------------------------------------------------------------------------

OT_TEST(order_flows_strategy_to_gateway_and_reports_back_to_strategy_and_risk) {
    Rig<> r;
    r.itch(directory(kAapl, "AAPL"));
    bool sent = false;
    oms::SubmitResult result;
    r.eng.strategy().on_book = [&](Locate l, const strategy::Context& c) {
        if (sent) return;
        sent = true;
        result = c.orders.submit({l, Side::buy, kPx, 50, oms::Tif::day}, c.now);
    };
    r.eng.on_itch(add(kAapl, 1, Side::buy, 100, kPx).view(), 5000);

    OT_CHECK(result.status == oms::SubmitStatus::ok);
    OT_CHECK_EQ(result.id, oms::OrderId{1});
    OT_CHECK_EQ(r.gw.enters.size(), std::size_t{1});
    const ouch::EnterOrder& e = r.gw.enters[0];
    OT_CHECK(e.token == ouch::Token::from_id(1));
    OT_CHECK(e.side == Side::buy);
    OT_CHECK_EQ(e.shares, Qty{50});
    OT_CHECK(e.stock == Symbol("AAPL"));  // symbol came from the books' directory
    OT_CHECK_EQ(e.price, kPx);
    OT_CHECK_EQ(e.time_in_force, ouch::kTifSystemHours);
    OT_CHECK_EQ(r.gw.enter_times[0], Nanos{5000});
    OT_CHECK_EQ(r.eng.stats().orders_sent, std::uint64_t{1});
    OT_CHECK_EQ(r.eng.risk().open_qty(kAapl, Side::buy), std::int64_t{50});

    auto& upd = r.eng.strategy().updates;
    OT_CHECK_EQ(upd.size(), std::size_t{1});
    OT_CHECK(upd.back().status == oms::OrderStatus::pending_new);
    OT_CHECK_EQ(upd.back().last_update, Nanos{5000});

    // Accepted -> live, exchange reference stored.
    OT_CHECK_EQ(r.eng.on_ouch(accepted(1, 777).view(), 6000), DecodeStatus::ok);
    OT_CHECK(upd.back().status == oms::OrderStatus::live);
    OT_CHECK_EQ(upd.back().exchange_ref, OrderRef{777});

    // Partial fill: 20 @ 100.0000.
    OT_CHECK_EQ(r.eng.on_ouch(executed(1, 20, kPx, 11).view(), 7000), DecodeStatus::ok);
    OT_CHECK_EQ(r.eng.strategy().fills.size(), std::size_t{1});
    const oms::Fill& f = r.eng.strategy().fills[0];
    OT_CHECK_EQ(f.id, oms::OrderId{1});
    OT_CHECK_EQ(f.locate, kAapl);
    OT_CHECK(f.side == Side::buy);
    OT_CHECK_EQ(f.qty, Qty{20});
    OT_CHECK_EQ(f.price, kPx);
    OT_CHECK_EQ(f.ts, Nanos{7000});
    OT_CHECK_EQ(f.match, std::uint64_t{11});
    OT_CHECK(upd.back().status == oms::OrderStatus::live);
    OT_CHECK_EQ(upd.back().cum_qty, Qty{20});
    OT_CHECK_EQ(upd.back().leaves_qty, Qty{30});
    OT_CHECK_EQ(r.eng.risk().position(kAapl).qty, std::int64_t{20});
    OT_CHECK_EQ(r.eng.risk().open_qty(kAapl, Side::buy), std::int64_t{30});

    // Final fill: 30 @ 100.1000. Average = (20*1000000 + 30*1001000) / 50 = 1000600.
    r.eng.on_ouch(executed(1, 30, 1'001'000, 12).view(), 8000);
    OT_CHECK(upd.back().status == oms::OrderStatus::filled);
    OT_CHECK_EQ(r.eng.orders().find(1)->avg_price, Price{1'000'600});
    OT_CHECK_EQ(r.eng.risk().position(kAapl).qty, std::int64_t{50});
    OT_CHECK_EQ(r.eng.risk().position(kAapl).avg_price, Price{1'000'600});
    OT_CHECK_EQ(r.eng.risk().open_qty(kAapl, Side::buy), std::int64_t{0});

    // Sell everything at 100.2000: realized = (1002000 - 1000600) * 50 = 70000.
    const oms::SubmitResult sell = r.eng.orders().submit({kAapl, Side::sell, 1'002'000, 50, oms::Tif::day}, 9000);
    OT_CHECK(sell.status == oms::SubmitStatus::ok);
    r.eng.on_ouch(accepted(2, 778, 50, 1'002'000, Side::sell).view(), 9100);
    r.eng.on_ouch(executed(2, 50, 1'002'000, 13).view(), 9200);
    OT_CHECK_EQ(r.eng.risk().position(kAapl).qty, std::int64_t{0});
    OT_CHECK_EQ(r.eng.risk().realized_pnl(), std::int64_t{70'000});

    // A report for a token nobody minted is decoded, handed to the manager and ignored there.
    r.eng.on_ouch(executed(4242, 5, kPx).view(), 9300);
    OT_CHECK_EQ(r.eng.orders().stats().unknown_tokens, std::uint64_t{1});

    const engine::Stats& s = r.eng.stats();
    OT_CHECK_EQ(s.fills, std::uint64_t{3});
    OT_CHECK_EQ(s.ouch_reports, std::uint64_t{6});  // A E E A E E(unknown token)
    OT_CHECK_EQ(s.ouch_errors, std::uint64_t{0});
    OT_CHECK_EQ(s.orders_sent, std::uint64_t{1});   // the direct sell bypassed the strategy's facade
    OT_CHECK_EQ(s.risk_rejects, std::uint64_t{0});
}

OT_TEST(exchange_reject_and_cancel_reports_release_exposure_and_notify) {
    Rig<> r;
    r.aapl_book();
    r.eng.orders().submit({kAapl, Side::buy, kPx, 40, oms::Tif::day}, 100);
    r.eng.orders().submit({kAapl, Side::sell, 1'010'000, 30, oms::Tif::day}, 101);
    OT_CHECK_EQ(r.eng.risk().open_qty(kAapl, Side::buy), std::int64_t{40});

    ouch::Rejected rej;
    rej.token = ouch::Token::from_id(1);
    rej.reason = 'X';
    OT_CHECK_EQ(r.eng.on_ouch(ow(rej).view(), 200), DecodeStatus::ok);
    OT_CHECK(r.eng.orders().find(1)->status == oms::OrderStatus::rejected);
    OT_CHECK_EQ(r.eng.orders().find(1)->reason, 'X');
    OT_CHECK_EQ(r.eng.risk().open_qty(kAapl, Side::buy), std::int64_t{0});

    r.eng.on_ouch(accepted(2, 5, 30, 1'010'000, Side::sell).view(), 210);
    ouch::Canceled c;
    c.token = ouch::Token::from_id(2);
    c.decrement = 30;
    c.reason = 'U';
    r.eng.on_ouch(ow(c).view(), 220);
    OT_CHECK(r.eng.orders().find(2)->status == oms::OrderStatus::canceled);
    OT_CHECK_EQ(r.eng.risk().open_qty(kAapl, Side::sell), std::int64_t{0});
    OT_CHECK(r.eng.strategy().updates.back().status == oms::OrderStatus::canceled);
    OT_CHECK_EQ(r.eng.stats().ouch_reports, std::uint64_t{3});
}

OT_TEST(unsupported_and_malformed_ouch_are_counted_as_errors_only) {
    Rig<> r;
    r.aapl_book();
    r.eng.orders().submit({kAapl, Side::buy, kPx, 40, oms::Tif::day}, 100);
    const std::size_t updates = r.eng.strategy().updates.size();

    std::array<std::byte, 1> d{std::byte{'D'}};  // Order Aiq Canceled: not supported
    OT_CHECK_EQ(r.eng.on_ouch(d, 1), DecodeStatus::unknown_type);
    OT_CHECK_EQ(r.eng.on_ouch({}, 1), DecodeStatus::truncated);
    Wire a = accepted(1, 9);
    OT_CHECK_EQ(a.n, std::size_t{66});
    OT_CHECK_EQ(r.eng.on_ouch(std::span<const std::byte>(a.bytes.data(), 65), 1), DecodeStatus::truncated);
    OT_CHECK_EQ(r.eng.on_ouch(std::span<const std::byte>(a.bytes.data(), 67), 1), DecodeStatus::bad_length);
    Wire dead = a;
    dead.bytes[64] = std::byte{'Z'};  // order state must be L or D
    OT_CHECK_EQ(r.eng.on_ouch(dead.view(), 1), DecodeStatus::bad_field);

    OT_CHECK_EQ(r.eng.stats().ouch_errors, std::uint64_t{5});
    OT_CHECK_EQ(r.eng.stats().ouch_reports, std::uint64_t{0});
    OT_CHECK(r.eng.orders().find(1)->status == oms::OrderStatus::pending_new);
    OT_CHECK_EQ(r.eng.strategy().updates.size(), updates);
}

// ---- risk ----------------------------------------------------------------------------------------

OT_TEST(risk_rejects_reach_the_strategy_and_are_counted_gateway_untouched) {
    engine::Config c = small_config();
    c.limits.max_order_qty = 100;
    Rig<> r(c);
    r.aapl_book();

    std::vector<oms::SubmitResult> results;
    r.eng.strategy().on_book = [&](Locate l, const strategy::Context& ctx) {
        results.push_back(ctx.orders.submit({l, Side::buy, kPx, 200, oms::Tif::day}, ctx.now));
    };
    r.itch(add(kAapl, 3, Side::buy, 10, 990'000));
    OT_CHECK_EQ(results.size(), std::size_t{1});
    OT_CHECK(results[0].status == oms::SubmitStatus::rejected_by_risk);
    OT_CHECK(results[0].risk_reason == risk::Reject::order_qty);
    OT_CHECK_EQ(r.gw.enters.size(), std::size_t{0});
    OT_CHECK_EQ(r.eng.stats().risk_rejects, std::uint64_t{1});
    OT_CHECK_EQ(r.eng.stats().orders_sent, std::uint64_t{0});

    // Manual kill switch: every submit is refused with kill_switch.
    r.eng.risk().set_kill_switch(true);
    r.itch(add(kAapl, 4, Side::buy, 10, 980'000));
    OT_CHECK_EQ(results.size(), std::size_t{2});
    r.eng.strategy().on_book = [&](Locate l, const strategy::Context& ctx) {
        results.push_back(ctx.orders.submit({l, Side::buy, kPx, 10, oms::Tif::day}, ctx.now));
    };
    r.itch(add(kAapl, 5, Side::buy, 10, 970'000));
    OT_CHECK_EQ(results.size(), std::size_t{3});
    OT_CHECK(results[2].risk_reason == risk::Reject::kill_switch);
    OT_CHECK_EQ(r.eng.stats().risk_rejects, std::uint64_t{3});
    OT_CHECK_EQ(r.gw.enters.size(), std::size_t{0});

    // Releasing the switch lets the next order through.
    r.eng.risk().set_kill_switch(false);
    r.itch(add(kAapl, 6, Side::buy, 10, 960'000));
    OT_CHECK(results[3].status == oms::SubmitStatus::ok);
    OT_CHECK_EQ(r.gw.enters.size(), std::size_t{1});
    OT_CHECK_EQ(r.eng.stats().orders_sent, std::uint64_t{1});
}

OT_TEST(replace_through_the_strategy_is_risk_checked_and_counted) {
    engine::Config c = small_config();
    c.limits.max_order_qty = 100;
    Rig<> r(c);
    r.aapl_book();
    oms::SubmitResult sub = r.eng.orders().submit({kAapl, Side::buy, kPx, 50, oms::Tif::day}, 100);
    r.eng.on_ouch(accepted(1, 5).view(), 110);

    std::vector<oms::SubmitStatus> st;
    r.eng.strategy().on_book = [&](Locate, const strategy::Context& ctx) {
        st.push_back(ctx.orders.replace(sub.id, kPx, 250, ctx.now));  // growth 200 > 100
        st.push_back(ctx.orders.replace(sub.id, kPx, 120, ctx.now));  // growth 70 <= 100
    };
    r.itch(add(kAapl, 3, Side::buy, 10, 990'000));
    OT_CHECK_EQ(st.size(), std::size_t{2});
    OT_CHECK(st[0] == oms::SubmitStatus::rejected_by_risk);
    OT_CHECK(st[1] == oms::SubmitStatus::ok);
    OT_CHECK_EQ(r.gw.replaces.size(), std::size_t{1});
    OT_CHECK_EQ(r.gw.replaces[0].shares, Qty{120});
    OT_CHECK_EQ(r.eng.stats().risk_rejects, std::uint64_t{1});
}

OT_TEST(mark_follows_the_book_mid_and_holds_when_no_mid_exists) {
    Rig<> r;
    r.aapl_book();  // bid 1000000, ask 1010000: mid 1005000
    r.eng.orders().submit({kAapl, Side::buy, 1'010'000, 10, oms::Tif::day}, 1);
    r.eng.on_ouch(accepted(1, 5, 10, 1'010'000).view(), 2);
    r.eng.on_ouch(executed(1, 10, 1'010'000).view(), 3);  // long 10 @ 1010000

    r.itch(add(kAapl, 10, Side::buy, 5, 900'000));  // not the best bid: mid unchanged
    OT_CHECK_EQ(r.eng.risk().unrealized_pnl(), std::int64_t{-50'000});  // (1005000-1010000)*10

    r.itch(add(kAapl, 11, Side::buy, 5, 1'002'000));  // new best bid: mid = 1006000
    OT_CHECK_EQ(r.eng.risk().unrealized_pnl(), std::int64_t{-40'000});

    r.itch(del(kAapl, 2));  // ask side empty: no mid, the last mark stands
    OT_CHECK_EQ(r.eng.risk().unrealized_pnl(), std::int64_t{-40'000});

    r.itch(add(kAapl, 12, Side::sell, 5, 1'004'000));  // mid = (1002000+1004000)/2 = 1003000
    OT_CHECK_EQ(r.eng.risk().unrealized_pnl(), std::int64_t{-70'000});

    // A locked/crossed touch has no mid either: an ask at 1001000 sits below the bid.
    r.itch(add(kAapl, 13, Side::sell, 5, 1'001'000));
    OT_CHECK_EQ(r.eng.risk().unrealized_pnl(), std::int64_t{-70'000});
}

OT_TEST(loss_limit_trips_the_kill_switch_from_book_marks) {
    engine::Config c = small_config();
    c.limits.max_loss = 50'000;
    Rig<> r(c);
    r.aapl_book();  // mid 1005000
    r.eng.orders().submit({kAapl, Side::buy, 1'010'000, 10, oms::Tif::day}, 1);
    r.eng.on_ouch(accepted(1, 5, 10, 1'010'000).view(), 2);
    r.eng.on_ouch(executed(1, 10, 1'010'000).view(), 3);
    r.itch(add(kAapl, 10, Side::buy, 5, 900'000));  // triggers a mark at mid 1005000: pnl -50000
    OT_CHECK_EQ(r.eng.risk().total_pnl(), std::int64_t{-50'000});
    OT_CHECK(r.eng.risk().kill_switch());
    const oms::SubmitResult again = r.eng.orders().submit({kAapl, Side::buy, kPx, 1, oms::Tif::day}, 4);
    OT_CHECK(again.risk_reason == risk::Reject::kill_switch);
}

OT_TEST(reference_price_is_mid_then_last_trade_then_nothing) {
    engine::Config c = small_config();
    c.limits.price_band_bps = 100;  // 1 %
    c.limits.require_reference = true;
    Rig<> r(c);
    r.itch(directory(kAapl, "AAPL"));
    auto try_buy = [&](Price px) {
        const oms::SubmitResult s = r.eng.orders().submit({kAapl, Side::buy, px, 10, oms::Tif::day}, 1);
        return s.status == oms::SubmitStatus::ok ? risk::Reject::none : s.risk_reason;
    };

    // Nothing known: the band cannot be evaluated.
    OT_CHECK(try_buy(kPx) == risk::Reject::no_reference);

    // Last trade only: reference 1000000.
    r.itch(trade(kAapl, kPx));
    OT_CHECK(try_buy(1'005'000) == risk::Reject::none);        // 50 bps
    OT_CHECK(try_buy(1'020'000) == risk::Reject::price_band);  // 200 bps

    // Two-sided book far from the trade: the mid (2000000) wins over the last trade.
    r.itch(add(kAapl, 1, Side::buy, 100, 1'990'000));
    r.itch(add(kAapl, 2, Side::sell, 100, 2'010'000));
    OT_CHECK(try_buy(2'000'000) == risk::Reject::none);
    OT_CHECK(try_buy(kPx) == risk::Reject::price_band);

    // One-sided again: back to the last trade.
    r.itch(del(kAapl, 2));
    OT_CHECK(try_buy(kPx) == risk::Reject::none);
    OT_CHECK(try_buy(2'000'000) == risk::Reject::price_band);

    // Crossed book: no trustworthy mid, so the last trade again.
    r.itch(add(kAapl, 3, Side::sell, 100, 1'980'000));
    OT_CHECK(r.eng.books().book(kAapl)->crossed());
    OT_CHECK(try_buy(kPx) == risk::Reject::none);
    OT_CHECK(try_buy(2'000'000) == risk::Reject::price_band);
}

// ---- feed gap ------------------------------------------------------------------------------------

OT_TEST(feed_gap_cancels_everything_and_silences_the_strategy_until_resume) {
    Rig<> r;
    r.aapl_book();
    const oms::OrderId a = r.eng.orders().submit({kAapl, Side::buy, kPx, 10, oms::Tif::day}, 100).id;
    const oms::OrderId b = r.eng.orders().submit({kAapl, Side::sell, 1'010'000, 10, oms::Tif::day}, 101).id;
    r.eng.on_ouch(accepted(1, 5, 10).view(), 110);  // a is live, b is still pending_new
    OT_CHECK(r.eng.trading_enabled());
    const std::size_t calls = r.eng.strategy().book_calls.size();

    r.eng.on_feed_gap(200);
    OT_CHECK(!r.eng.trading_enabled());
    OT_CHECK_EQ(r.eng.stats().feed_gaps, std::uint64_t{1});
    OT_CHECK_EQ(r.gw.cancels.size(), std::size_t{2});
    OT_CHECK(r.gw.cancels[0].token == ouch::Token::from_id(1));
    OT_CHECK(r.gw.cancels[1].token == ouch::Token::from_id(2));
    OT_CHECK_EQ(r.gw.cancels[0].shares, Qty{0});  // 0 = cancel everything that is left
    OT_CHECK(r.eng.orders().find(a)->status == oms::OrderStatus::pending_cancel);
    OT_CHECK(r.eng.orders().find(b)->status == oms::OrderStatus::pending_cancel);

    // A second gap notice does not send the cancels again.
    r.eng.on_feed_gap(201);
    OT_CHECK_EQ(r.gw.cancels.size(), std::size_t{2});
    OT_CHECK_EQ(r.eng.stats().feed_gaps, std::uint64_t{2});

    // Market data keeps updating the books (so they can be rebuilt) but the strategy sleeps.
    const std::uint64_t updates = r.eng.stats().book_updates;
    r.itch(add(kAapl, 20, Side::buy, 10, 990'000));
    OT_CHECK_EQ(r.eng.stats().book_updates, updates + 1);
    OT_CHECK(r.eng.books().order(20) != nullptr);
    OT_CHECK_EQ(r.eng.strategy().book_calls.size(), calls);

    // Fills in flight still reach the strategy, but it cannot open new exposure from the callback.
    std::vector<oms::SubmitStatus> attempts;
    r.eng.strategy().on_fill_hook = [&](const oms::Fill& f, const strategy::Context& c) {
        attempts.push_back(c.orders.submit({f.locate, Side::buy, kPx, 5, oms::Tif::ioc}, c.now).status);
        attempts.push_back(c.orders.replace(f.id, kPx, 8, c.now));
    };
    r.eng.on_ouch(executed(1, 4, kPx).view(), 300);
    OT_CHECK_EQ(r.eng.strategy().fills.size(), std::size_t{1});
    OT_CHECK_EQ(attempts.size(), std::size_t{2});
    OT_CHECK(attempts[0] == oms::SubmitStatus::bad_state);
    OT_CHECK(attempts[1] == oms::SubmitStatus::bad_state);
    OT_CHECK_EQ(r.gw.enters.size(), std::size_t{2});  // nothing new left the engine

    // Cancels acknowledged: the orders end up canceled and the strategy sees it.
    ouch::Canceled c1;
    c1.token = ouch::Token::from_id(1);
    c1.decrement = 6;
    c1.reason = 'U';
    r.eng.on_ouch(ow(c1).view(), 310);
    OT_CHECK(r.eng.orders().find(a)->status == oms::OrderStatus::canceled);
    OT_CHECK_EQ(r.eng.orders().find(a)->cum_qty, Qty{4});
    OT_CHECK(r.eng.strategy().updates.back().status == oms::OrderStatus::canceled);

    // Resume: strategy is called again and may trade.
    r.eng.resume_trading();
    OT_CHECK(r.eng.trading_enabled());
    oms::SubmitStatus after = oms::SubmitStatus::bad_state;
    r.eng.strategy().on_book = [&](Locate l, const strategy::Context& ctx) {
        after = ctx.orders.submit({l, Side::buy, kPx, 5, oms::Tif::day}, ctx.now).status;
    };
    r.itch(add(kAapl, 21, Side::buy, 10, 980'000));
    OT_CHECK_EQ(r.eng.strategy().book_calls.size(), calls + 1);
    OT_CHECK(after == oms::SubmitStatus::ok);
    OT_CHECK_EQ(r.gw.enters.size(), std::size_t{3});
}

OT_TEST(feed_gap_with_a_refusing_gateway_can_be_retried) {
    Rig<> r;
    r.aapl_book();
    r.eng.orders().submit({kAapl, Side::buy, kPx, 10, oms::Tif::day}, 100);
    r.gw.accept = false;
    r.eng.on_feed_gap(200);
    OT_CHECK_EQ(r.gw.cancels.size(), std::size_t{0});
    OT_CHECK(!r.eng.trading_enabled());
    r.gw.accept = true;
    r.eng.on_feed_gap(201);
    OT_CHECK_EQ(r.gw.cancels.size(), std::size_t{1});
}

// ---- allocation ----------------------------------------------------------------------------------

OT_TEST(message_path_does_not_allocate) {
    struct Local {
        CountingGateway gw;
        engine::Engine<Quiet> eng;
        Nanos t{1000};
        Local() : eng(small_config(), gw) {}
        void itch(const Wire& w) { eng.on_itch(w.view(), t += 10); }
    };
    Local l;
    l.itch(directory(kAapl, "AAPL"));
    l.itch(add(kAapl, 1, Side::buy, 1000, kPx));
    l.itch(add(kAapl, 2, Side::sell, 1000, 1'010'000));

    // Pre-build the traffic so building it is not counted.
    std::vector<Wire> feed;
    for (OrderRef ref = 10; ref < 90; ++ref) {
        feed.push_back(add(kAapl, ref, ref % 2 ? Side::buy : Side::sell, 10,
                           ref % 2 ? kPx - Price(ref) * 100 : 1'010'000 + Price(ref) * 100));
        feed.push_back(iw(itch::OrderCancel{hdr(kAapl), ref, 4}));
        feed.push_back(iw(itch::OrderExecuted{hdr(kAapl), ref, 2, ref}));
        feed.push_back(iw(itch::OrderReplace{hdr(kAapl), ref, ref + 1000, 3, kPx - 200}));
        feed.push_back(del(kAapl, ref + 1000));
        feed.push_back(trade(kAapl, kPx));
    }
    std::vector<Wire> reports;
    for (std::uint64_t tok = 1; tok <= 20; ++tok) {
        reports.push_back(accepted(tok, 500 + tok, 1));
        reports.push_back(executed(tok, 1, kPx, tok));
    }

    const std::uint64_t before = g_allocations;
    Nanos now = 5000;
    std::size_t next_report = 0;
    for (const Wire& w : feed) {
        l.eng.on_itch(w.view(), now += 10);
        if (next_report < reports.size() && l.eng.orders().orders_submitted() * 2 > next_report) {
            l.eng.on_ouch(reports[next_report++].view(), now += 10);
        }
    }
    while (next_report < reports.size()) l.eng.on_ouch(reports[next_report++].view(), now += 10);
    l.eng.on_feed_gap(now += 10);
    const std::uint64_t allocs = g_allocations - before;

#ifndef OT_UNDER_SANITIZER
    OT_CHECK_EQ(allocs, std::uint64_t{0});
#else
    (void)allocs;
#endif
    OT_CHECK(l.eng.strategy().calls > 100);   // the loop really exercised the strategy path
    OT_CHECK(l.eng.orders().orders_submitted() >= 20);
    OT_CHECK(l.eng.strategy().fills > 0);
}

// ---- real strategies over a simulated exchange ---------------------------------------------------

struct RunResult {
    engine::Stats stats;
    std::int64_t realized{};
    std::int64_t total{};
    std::uint64_t sim_fills{};
    std::uint64_t submitted{};
    std::uint64_t pending{};
    oms::OrderManager::Stats oms;
    std::size_t open_orders{};
};

// The same loop the backtester runs: advance the simulator, hand it due reports, feed the message
// to the simulator and to the engine.
template <class S>
RunResult run_synthetic(std::uint64_t seed, S strategy) {
    sim::SimConfig sc;
    sc.max_orders = 1u << 14;
    sc.books.max_orders = 1u << 13;
    sc.books.max_levels_per_side = 64;
    sc.books.max_symbols = 16;
    sim::ExchangeSim exchange(sc);

    engine::Config ec;
    ec.books = sc.books;
    ec.oms.max_orders = 1u << 14;
    ec.max_locates = 16;
    engine::Engine<S> eng(ec, exchange, std::move(strategy));

    sim::SyntheticConfig gc;
    gc.seed = seed;
    gc.symbols = 8;
    gc.messages = 40'000;
    sim::SyntheticMarket market(gc);

    auto deliver = [&](Nanos upto) {
        exchange.drain_reports(upto, [&](std::span<const std::byte> m, Nanos at) {
            OT_CHECK_EQ(eng.on_ouch(m, at), DecodeStatus::ok);
        });
    };
    std::array<std::byte, 128> buf{};
    std::size_t len = 0;
    Nanos ts = 0;
    while (market.next(buf, len, ts)) {
        exchange.advance(ts);
        deliver(ts);
        const std::span<const std::byte> msg(buf.data(), len);
        exchange.on_itch(msg, ts);
        OT_CHECK_EQ(eng.on_itch(msg, ts), DecodeStatus::ok);
    }
    const Nanos end = std::numeric_limits<Nanos>::max();
    for (int i = 0; i < 8 && (exchange.pending_reports() != 0 || i == 0); ++i) {
        exchange.advance(end);
        deliver(end);
    }

    RunResult r;
    r.stats = eng.stats();
    r.realized = eng.risk().realized_pnl();
    r.total = eng.risk().total_pnl();
    r.sim_fills = exchange.fills();
    r.submitted = eng.orders().orders_submitted();
    r.pending = exchange.pending_reports();
    r.oms = eng.orders().stats();
    r.open_orders = eng.orders().open_orders();
    return r;
}

bool same(const engine::Stats& a, const engine::Stats& b) {
    return a.itch_messages == b.itch_messages && a.itch_skipped == b.itch_skipped &&
           a.book_updates == b.book_updates && a.book_errors == b.book_errors &&
           a.ouch_reports == b.ouch_reports && a.ouch_errors == b.ouch_errors &&
           a.feed_gaps == b.feed_gaps && a.orders_sent == b.orders_sent &&
           a.risk_rejects == b.risk_rejects && a.fills == b.fills;
}

template <class S>
void check_real_strategy(std::uint64_t min_orders) {
    static_assert(strategy::Strategy<S>);
    const RunResult a = run_synthetic<S>(42, S{});
    const RunResult b = run_synthetic<S>(42, S{});

    OT_CHECK(same(a.stats, b.stats));  // deterministic
    OT_CHECK_EQ(a.realized, b.realized);
    OT_CHECK_EQ(a.total, b.total);

    // Clean input, clean output.
    OT_CHECK_EQ(a.stats.itch_skipped, std::uint64_t{0});
    OT_CHECK_EQ(a.stats.ouch_errors, std::uint64_t{0});
    OT_CHECK_EQ(a.stats.book_errors, std::uint64_t{0});
    OT_CHECK(a.stats.book_updates > 30'000);

    // The strategy traded, every order it sent is in the order manager, and every report the
    // simulator generated reached the manager without being dropped as inconsistent.
    OT_CHECK(a.stats.orders_sent >= min_orders);
    OT_CHECK_EQ(a.submitted, a.stats.orders_sent);
    OT_CHECK_EQ(a.pending, std::uint64_t{0});
    OT_CHECK_EQ(a.stats.fills, a.sim_fills);
    OT_CHECK_EQ(a.oms.unknown_tokens, std::uint64_t{0});
    OT_CHECK_EQ(a.oms.invalid_reports, std::uint64_t{0});
    OT_CHECK_EQ(a.oms.late_reports, std::uint64_t{0});

    // A different seed gives a different run.
    const RunResult c = run_synthetic<S>(43, S{});
    OT_CHECK(!same(a.stats, c.stats) || a.total != c.total);
}

OT_TEST(imbalance_taker_runs_in_the_engine) { check_real_strategy<strategy::ImbalanceTaker>(100); }
OT_TEST(microprice_maker_runs_in_the_engine) { check_real_strategy<strategy::MicropriceMaker>(100); }
OT_TEST(ema_cross_runs_in_the_engine) { check_real_strategy<strategy::EmaCross>(100); }

}  // namespace

OT_TEST_MAIN()

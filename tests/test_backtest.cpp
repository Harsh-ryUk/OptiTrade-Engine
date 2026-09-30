// Tests for the backtester.
//
// The first group replays a seven-message market that is small enough to work out by hand: a
// scripted strategy buys 100 shares at the touch, the market then lifts the offer and the bid, and
// every quantity the report contains (fills, volume, realized, unrealized, drawdown) is derived on
// paper in the comments. Latency tests move the order's arrival across the message that removes the
// liquidity, at nanosecond boundaries. The second group checks reproducibility and source
// equivalence (memory vs capture file), and the last one runs the three real strategies over a
// synthetic feed and watches their positions from inside the run.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "check.hpp"
#include "optitrade/itch/encoder.hpp"
#include "optitrade/replay/backtest.hpp"
#include "optitrade/replay/capture.hpp"
#include "optitrade/sim/synthetic_market.hpp"
#include "optitrade/strategy/ema_cross.hpp"
#include "optitrade/strategy/imbalance_taker.hpp"
#include "optitrade/strategy/microprice_maker.hpp"

using namespace optitrade;

namespace {

// ---- configuration -----------------------------------------------------------------------------------

replay::BacktestConfig small_config(Nanos order_latency = 50'000, Nanos report_latency = 50'000) {
    replay::BacktestConfig c;
    c.sim.order_latency_ns = order_latency;
    c.sim.report_latency_ns = report_latency;
    c.sim.max_orders = 1u << 10;
    c.sim.books.max_orders = 1u << 13;
    c.sim.books.max_levels_per_side = 64;
    c.sim.books.max_symbols = 16;
    c.engine.books = c.sim.books;
    c.engine.oms.max_orders = 1u << 10;
    c.engine.max_locates = 16;
    return c;
}

// ---- the hand-built market ---------------------------------------------------------------------------

struct Wire {
    std::array<std::byte, 64> bytes{};
    std::size_t n{};
};

template <class M>
Wire enc(const M& m) {
    Wire w;
    w.n = itch::encode(m, w.bytes);
    OT_CHECK(w.n != 0);
    return w;
}

constexpr Price kBid = 1'000'000;      // 100.0000
constexpr Price kAsk = 1'010'000;      // 101.0000, the offer that gets lifted
constexpr Price kFarAsk = 1'020'000;   // 102.0000
constexpr Price kNewBid = 1'015'000;   // 101.5000

itch::Header hdr(Nanos ts) { return itch::Header{1, 0, ts}; }

Wire directory() {
    itch::StockDirectory m;
    m.h = hdr(0);
    m.symbol = Symbol("AAPL");
    m.market_category = 'Q';
    m.financial_status = 'N';
    m.round_lot_size = 100;
    return enc(m);
}

Wire add(Nanos ts, OrderRef ref, Side side, Qty qty, Price px) {
    itch::AddOrder m;
    m.h = hdr(ts);
    m.ref = ref;
    m.side = side;
    m.shares = qty;
    m.symbol = Symbol("AAPL");
    m.price = px;
    return enc(m);
}

struct Feed {
    replay::MemorySource src;
    void put(Nanos ts, const Wire& w) { src.add(ts, std::span<const std::byte>(w.bytes.data(), w.n)); }
};

// Timeline (nanoseconds):
//      0  directory AAPL
//   1'000  bid 500 @ 100.0000 (ref 1)
//   2'000  offer 500 @ 101.0000 (ref 2)       -> strategy sends IOC buy 100 @ 101.0000
//   3'000  offer 500 @ 102.0000 (ref 3)
//  `lift`  delete ref 2: the 101.0000 offer is gone (default 70'000)
// 120'000  bid 100 @ 101.5000 (ref 4)
// 200'000  bid 10  @ 100.0000 (ref 5), or with `pull_bid` delete ref 4 (the new bid disappears)
//
// With the default 50 us latencies the buy arrives at 52'000, i.e. before the offer goes.
Feed scenario(Nanos lift = 70'000, bool pull_bid = false) {
    Feed f;
    f.put(0, directory());
    f.put(1'000, add(1'000, 1, Side::buy, 500, kBid));
    f.put(2'000, add(2'000, 2, Side::sell, 500, kAsk));
    f.put(3'000, add(3'000, 3, Side::sell, 500, kFarAsk));
    f.put(lift, enc(itch::OrderDelete{hdr(lift), 2}));
    f.put(120'000, add(120'000, 4, Side::buy, 100, kNewBid));
    if (pull_bid) {
        f.put(200'000, enc(itch::OrderDelete{hdr(200'000), 4}));
    } else {
        f.put(200'000, add(200'000, 5, Side::buy, 10, kBid));
    }
    return f;
}

// Buys 100 at the ask the first time both sides exist. If `sell_at` is set it sells the 100 back at
// the bid once it knows (from a delivered fill) that it is long and the bid has reached `sell_at`.
struct BuyThenSell {
    Price sell_at{0};
    bool bought{false};
    bool sold{false};

    void on_book_update(Locate l, const strategy::Context& c) {
        const book::OrderBook* b = c.books.book(l);
        if (b == nullptr) return;
        const auto bid = b->best(Side::buy);
        const auto ask = b->best(Side::sell);
        if (!bid || !ask) return;
        if (!bought) {
            bought = true;
            c.orders.submit({l, Side::buy, ask->price, 100, oms::Tif::ioc}, c.now);
        } else if (sell_at != 0 && !sold && c.risk.position(l).qty == 100 && bid->price >= sell_at) {
            sold = true;
            c.orders.submit({l, Side::sell, bid->price, 100, oms::Tif::ioc}, c.now);
        }
    }
    void on_fill(const oms::Fill&, const strategy::Context&) {}
    void on_order_update(const oms::OrderInfo&, const strategy::Context&) {}
};
static_assert(strategy::Strategy<BuyThenSell>);

replay::BacktestReport run_scenario(Feed& f, const replay::BacktestConfig& cfg, Price sell_at = 0) {
    f.src.rewind();
    return replay::run_backtest(f.src, cfg, BuyThenSell{sell_at});
}

// ---- known answers -----------------------------------------------------------------------------------

OT_TEST(round_trip_buy_then_sell_has_hand_computed_pnl) {
    Feed f = scenario(70'000, true);
    const replay::BacktestReport r = run_scenario(f, small_config(), kNewBid);

    // t=2'000 the strategy sends IOC buy 100 @ 101.0000; it arrives at 52'000 and takes the whole
    // clip from the 500 displayed. Reports are due at 102'000 and reach the engine before the
    // message at 120'000, which adds the 101.5000 bid. Long 100, the bid has reached sell_at, so at
    // 120'000 the strategy sends IOC sell 100 @ 101.5000; it arrives at 170'000 and is filled by the
    // 100 displayed at that price. Its reports (220'000) are delivered in the end-of-input flush.
    // The last message removes that bid: a strategy that heard about its fill only after the book
    // update at 120'000 (reports delivered too late) would sell into a market with no such bid.
    OT_CHECK_EQ(r.messages, std::uint64_t{7});
    OT_CHECK_EQ(r.book_updates, std::uint64_t{6});  // five adds and one delete; the directory is not one
    OT_CHECK_EQ(r.orders_sent, std::uint64_t{2});
    OT_CHECK_EQ(r.orders_rejected_risk, std::uint64_t{0});
    OT_CHECK_EQ(r.fills, std::uint64_t{2});
    OT_CHECK_EQ(r.volume, std::uint64_t{200});

    OT_CHECK_EQ(r.realized_pnl, std::int64_t{500'000});  // (1'015'000 - 1'010'000) * 100
    OT_CHECK_EQ(r.unrealized_pnl, std::int64_t{0});      // flat
    OT_CHECK_EQ(r.total_pnl, std::int64_t{500'000});
    // Total PnL peaks at 750'000 when the bid arrives (long 100 marked at mid 1'017'500, cost
    // 1'010'000). At 200'000 that bid is deleted while the sale is still in flight (its fill is
    // delivered only in the flush), so the position is marked at mid 1'010'000: total 0, a trough
    // 750'000 below the peak. The sale at the old bid then books 500'000 and total recovers.
    OT_CHECK_EQ(r.max_drawdown, std::int64_t{750'000});
    OT_CHECK_EQ(r.dropped_reports, std::uint64_t{0});
}

OT_TEST(open_position_is_marked_at_the_final_mid) {
    Feed f = scenario();
    const replay::BacktestReport r = run_scenario(f, small_config());  // never sells

    // Long 100 @ 1'010'000. Final book: bid 1'015'000, ask 1'020'000, mid 1'017'500.
    OT_CHECK_EQ(r.orders_sent, std::uint64_t{1});
    OT_CHECK_EQ(r.fills, std::uint64_t{1});
    OT_CHECK_EQ(r.volume, std::uint64_t{100});
    OT_CHECK_EQ(r.realized_pnl, std::int64_t{0});
    OT_CHECK_EQ(r.unrealized_pnl, std::int64_t{750'000});  // (1'017'500 - 1'010'000) * 100
    OT_CHECK_EQ(r.total_pnl, std::int64_t{750'000});
    OT_CHECK_EQ(r.max_drawdown, std::int64_t{0});
}

OT_TEST(print_report_shows_signed_price_units_exactly) {
    char buf[40];
    replay::detail::format_units(buf, sizeof buf, 0);
    OT_CHECK_EQ(std::string(buf), std::string("0.0000"));
    replay::detail::format_units(buf, sizeof buf, 500'000);
    OT_CHECK_EQ(std::string(buf), std::string("50.0000"));
    replay::detail::format_units(buf, sizeof buf, -1);
    OT_CHECK_EQ(std::string(buf), std::string("-0.0001"));
    replay::detail::format_units(buf, sizeof buf, -123'456'789);
    OT_CHECK_EQ(std::string(buf), std::string("-12345.6789"));
    replay::detail::format_units(buf, sizeof buf, std::numeric_limits<std::int64_t>::min());
    OT_CHECK_EQ(std::string(buf), std::string("-922337203685477.5808"));

    replay::BacktestReport r;
    r.messages = 7;
    r.realized_pnl = 500'000;
    r.total_pnl = -120'345;
    r.digest = 0x00000000DEADBEEFULL;
    std::FILE* f = std::tmpfile();
    OT_CHECK(f != nullptr);
    if (f == nullptr) return;
    replay::print_report(r, "unit", f);
    std::rewind(f);
    char text[1024] = {};
    const std::size_t got = std::fread(text, 1, sizeof text - 1, f);
    std::fclose(f);
    const std::string s(text, got);
    OT_CHECK(s.find("== unit ==\n") == 0);
    OT_CHECK(s.find("messages        7\n") != std::string::npos);
    OT_CHECK(s.find("realized pnl    50.0000\n") != std::string::npos);
    OT_CHECK(s.find("total pnl       -12.0345\n") != std::string::npos);
    OT_CHECK(s.find("digest          00000000deadbeef\n") != std::string::npos);
    OT_CHECK(s.find("dropped") == std::string::npos);  // only shown when non-zero
}

OT_TEST(empty_source_gives_an_empty_report_and_the_fnv_offset_basis) {
    replay::MemorySource none;
    const replay::BacktestReport r = replay::run_backtest(none, small_config(), BuyThenSell{});
    replay::BacktestReport expected;
    expected.digest = 0xCBF29CE484222325ULL;  // nothing was folded in
    OT_CHECK(r == expected);
}

// ---- latency -----------------------------------------------------------------------------------------

OT_TEST(order_latency_decides_whether_the_offer_is_still_there) {
    // The buy is sent at 2'000 and the offer is deleted at 70'000. A request whose arrival equals a
    // message's timestamp is processed before that message, so 68'000 (arrival 70'000) still fills
    // and one nanosecond more misses the offer: the IOC finds nothing at or below 101.0000 and is
    // canceled.
    struct Case {
        Nanos latency;
        std::uint64_t fills;
    };
    const Case cases[] = {{0, 1}, {50'000, 1}, {67'999, 1}, {68'000, 1}, {68'001, 0}, {200'000, 0}};
    for (const Case& c : cases) {
        Feed f = scenario();
        const replay::BacktestReport r = run_scenario(f, small_config(c.latency));
        OT_CHECK_EQ(r.orders_sent, std::uint64_t{1});
        OT_CHECK_EQ(r.fills, c.fills);
        OT_CHECK_EQ(r.volume, c.fills * 100);
        OT_CHECK_EQ(r.total_pnl, c.fills != 0 ? std::int64_t{750'000} : std::int64_t{0});
        OT_CHECK_EQ(r.dropped_reports, std::uint64_t{0});
    }

    // Different latency, different conversation with the exchange, different digest.
    Feed a = scenario(), b = scenario();
    OT_CHECK(run_scenario(a, small_config(68'000)).digest != run_scenario(b, small_config(68'001)).digest);
}

OT_TEST(report_latency_delays_what_the_strategy_knows_not_what_the_exchange_does) {
    // With 1 s report latency the fill happens at the exchange as before, but the engine only hears
    // of it in the end-of-input flush. The strategy therefore never sees itself long while the
    // book is moving, never sells, and the position is simply marked at the end.
    Feed f = scenario();
    const replay::BacktestReport slow = run_scenario(f, small_config(50'000, 1'000'000'000), kNewBid);
    OT_CHECK_EQ(slow.orders_sent, std::uint64_t{1});
    OT_CHECK_EQ(slow.fills, std::uint64_t{1});
    OT_CHECK_EQ(slow.realized_pnl, std::int64_t{0});
    OT_CHECK_EQ(slow.unrealized_pnl, std::int64_t{750'000});

    // Same strategy with fast reports does sell.
    const replay::BacktestReport fast = run_scenario(f, small_config(50'000, 50'000), kNewBid);
    OT_CHECK_EQ(fast.orders_sent, std::uint64_t{2});
    OT_CHECK_EQ(fast.realized_pnl, std::int64_t{500'000});
    OT_CHECK(slow.digest != fast.digest);
}

// ---- time handling ----------------------------------------------------------------------------------

// Records the clock the engine shows it.
struct ClockRecorder {
    std::vector<Nanos>* nows;
    void on_book_update(Locate, const strategy::Context& c) { nows->push_back(c.now); }
    void on_fill(const oms::Fill&, const strategy::Context&) {}
    void on_order_update(const oms::OrderInfo&, const strategy::Context&) {}
};
static_assert(strategy::Strategy<ClockRecorder>);

OT_TEST(a_timestamp_that_steps_back_is_processed_at_the_previous_time) {
    // The delete is stamped 0 in the first feed. The backtester runs it at 3'000, the time of the
    // record before it, which is exactly what the second feed says explicitly.
    Feed stepped_back = scenario(0);
    Feed clamped = scenario(3'000);
    const replay::BacktestReport a = run_scenario(stepped_back, small_config());
    const replay::BacktestReport b = run_scenario(clamped, small_config());
    OT_CHECK(a == b);
    // The offer is gone before the order arrives, so the run differs from the ordinary one.
    OT_CHECK_EQ(a.fills, std::uint64_t{0});

    // What the strategy sees: a clock that never runs backwards, and the clamped value itself.
    std::vector<Nanos> nows;
    stepped_back.src.rewind();
    replay::run_backtest(stepped_back.src, small_config(), ClockRecorder{&nows});
    const std::vector<Nanos> expected{1'000, 2'000, 3'000, 3'000, 120'000, 200'000};  // A A A D A A
    OT_CHECK(nows == expected);
}

// ---- reproducibility and sources -----------------------------------------------------------------------

sim::SyntheticConfig synthetic(std::uint64_t seed, std::uint64_t messages) {
    sim::SyntheticConfig g;
    g.seed = seed;
    g.symbols = 8;
    g.messages = messages;
    return g;
}

// Runs the generator into memory and, if `path` is given, into a capture file as well.
replay::MemorySource generate(const sim::SyntheticConfig& g, const char* path = nullptr) {
    replay::MemorySource mem;
    replay::CaptureWriter writer(path != nullptr ? path : "");
    sim::SyntheticMarket market(g);
    std::array<std::byte, 128> buf{};
    std::size_t len = 0;
    Nanos ts = 0;
    while (market.next(buf, len, ts)) {
        const std::span<const std::byte> msg(buf.data(), len);
        mem.add(ts, msg);
        if (path != nullptr) OT_CHECK(writer.write(ts, msg));
    }
    if (path != nullptr) OT_CHECK(writer.close());
    return mem;
}

template <class S>
replay::BacktestReport run_mem(replay::MemorySource& mem, S s = S{}) {
    mem.rewind();
    return replay::run_backtest(mem, small_config(), std::move(s));
}

OT_TEST(same_input_twice_gives_identical_reports_and_digest) {
    replay::MemorySource mem = generate(synthetic(5, 20'000));
    const replay::BacktestReport a = run_mem<strategy::ImbalanceTaker>(mem);
    const replay::BacktestReport b = run_mem<strategy::ImbalanceTaker>(mem);
    OT_CHECK(a == b);
    OT_CHECK(a.orders_sent > 0);
    OT_CHECK(a.digest != 0xCBF29CE484222325ULL);

    // A different feed, or a different strategy on the same feed, must move the digest.
    replay::MemorySource other = generate(synthetic(6, 20'000));
    OT_CHECK(run_mem<strategy::ImbalanceTaker>(other).digest != a.digest);
    OT_CHECK(run_mem<strategy::EmaCross>(mem).digest != a.digest);
}

OT_TEST(memory_source_and_capture_file_are_equivalent) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "ot_test_backtest.otcap";
    const std::string p = path.string();
    replay::MemorySource mem = generate(synthetic(9, 20'000), p.c_str());

    const replay::BacktestReport from_mem = run_mem<strategy::ImbalanceTaker>(mem);
    replay::CaptureReader reader(p.c_str());
    OT_CHECK(reader.ok());
    const replay::BacktestReport from_file =
        replay::run_backtest(reader, small_config(), strategy::ImbalanceTaker{});
    OT_CHECK(reader.error() == DecodeStatus::ok);
    OT_CHECK_EQ(reader.records_read(), std::uint64_t{mem.size()});
    OT_CHECK(from_mem == from_file);
    OT_CHECK_EQ(from_file.messages, std::uint64_t{mem.size()});

    // A second pass over a fresh reader is identical as well.
    replay::CaptureReader again(p.c_str());
    OT_CHECK(replay::run_backtest(again, small_config(), strategy::ImbalanceTaker{}) == from_file);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// ---- real strategies ----------------------------------------------------------------------------------

constexpr std::int64_t kRiskMaxPosition = 500;

struct Invariants {
    std::uint64_t checks{};
    std::uint64_t violations{};
    std::int64_t max_abs_position{};
};

// Wraps a real strategy and, after every callback, checks the state the engine hands it: the
// position of each instrument stays within the risk limit and no open quantity is negative.
template <class S>
class Watched {
public:
    Watched(Invariants& inv, S s = S{}) : s_(std::move(s)), inv_(&inv) {}

    void on_book_update(Locate l, const strategy::Context& c) {
        s_.on_book_update(l, c);
        check(c);
    }
    void on_fill(const oms::Fill& f, const strategy::Context& c) {
        s_.on_fill(f, c);
        check(c);
    }
    void on_order_update(const oms::OrderInfo& o, const strategy::Context& c) {
        s_.on_order_update(o, c);
        check(c);
    }

private:
    void check(const strategy::Context& c) {
        ++inv_->checks;
        for (Locate l = 0; l < 16; ++l) {
            const std::int64_t pos = c.risk.position(l).qty;
            const std::int64_t mag = pos < 0 ? -pos : pos;
            inv_->max_abs_position = std::max(inv_->max_abs_position, mag);
            if (mag > kRiskMaxPosition || c.risk.open_qty(l, Side::buy) < 0 ||
                c.risk.open_qty(l, Side::sell) < 0) {
                ++inv_->violations;
            }
        }
    }

    S s_;
    Invariants* inv_;
};
static_assert(strategy::Strategy<Watched<strategy::EmaCross>>);

template <class S>
void check_strategy_on_50k_messages() {
    replay::MemorySource mem = generate(synthetic(42, 50'000));
    replay::BacktestConfig cfg = small_config();
    cfg.engine.limits.max_position = kRiskMaxPosition;

    Invariants inv;
    mem.rewind();
    const replay::BacktestReport r = replay::run_backtest(mem, cfg, Watched<S>(inv));

    OT_CHECK_EQ(r.messages, std::uint64_t{mem.size()});
    OT_CHECK(r.book_updates > 40'000);
    OT_CHECK(r.orders_sent >= 50);
    OT_CHECK(r.fills > 0);
    OT_CHECK(r.volume >= r.fills);
    OT_CHECK_EQ(r.dropped_reports, std::uint64_t{0});
    OT_CHECK_EQ(r.total_pnl, r.realized_pnl + r.unrealized_pnl);
    OT_CHECK(r.max_drawdown >= 0);
    OT_CHECK(inv.checks > 0);
    OT_CHECK_EQ(inv.violations, std::uint64_t{0});
    OT_CHECK(inv.max_abs_position <= kRiskMaxPosition);

    // Watching must not change behaviour: the bare strategy reproduces the run bit for bit.
    mem.rewind();
    OT_CHECK(replay::run_backtest(mem, cfg, S{}) == r);
}

OT_TEST(imbalance_taker_survives_50k_messages_within_limits) { check_strategy_on_50k_messages<strategy::ImbalanceTaker>(); }
OT_TEST(microprice_maker_survives_50k_messages_within_limits) { check_strategy_on_50k_messages<strategy::MicropriceMaker>(); }
OT_TEST(ema_cross_survives_50k_messages_within_limits) { check_strategy_on_50k_messages<strategy::EmaCross>(); }

}  // namespace

OT_TEST_MAIN()

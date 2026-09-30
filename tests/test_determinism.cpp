// Determinism golden test.
//
// Synthetic market seed 42, 8 symbols, 200'000 flow messages goes through each strategy in the
// backtester with a fixed configuration. The digest of every run (over every order sent and every
// report delivered, see replay/backtest.hpp) must equal the frozen constant below. Everything on
// that path is integer arithmetic on a seeded generator, so the same constants must come out on
// every compiler, optimisation level and operating system; a mismatch in CI means behaviour
// changed or a platform dependence crept in.
//
// The constants are supposed to change when the intended behaviour of the generator, the exchange
// simulator, the engine or a strategy changes. Update them then, in the same commit, and say why.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <utility>

#include "check.hpp"
#include "optitrade/replay/backtest.hpp"
#include "optitrade/replay/capture.hpp"
#include "optitrade/sim/synthetic_market.hpp"
#include "optitrade/strategy/ema_cross.hpp"
#include "optitrade/strategy/imbalance_taker.hpp"
#include "optitrade/strategy/microprice_maker.hpp"

using namespace optitrade;

namespace {

constexpr std::uint64_t kExpectedImbalanceTaker = 0x71b2bc32e2aaea05ULL;
constexpr std::uint64_t kExpectedMicropriceMaker = 0x9ddaaa919b1ee887ULL;
constexpr std::uint64_t kExpectedEmaCross = 0x8ae3940c57b17a0dULL;

// Part of the frozen configuration: changing any value here changes the digests.
replay::BacktestConfig golden_config() {
    replay::BacktestConfig c;
    c.sim.order_latency_ns = 50'000;
    c.sim.report_latency_ns = 50'000;
    c.sim.max_orders = 1u << 14;
    c.sim.books.max_orders = 1u << 13;
    c.sim.books.max_levels_per_side = 64;
    c.sim.books.max_symbols = 16;
    c.engine.books = c.sim.books;
    c.engine.oms.max_orders = 1u << 14;
    c.engine.max_locates = 16;
    return c;
}

replay::MemorySource golden_feed() {
    sim::SyntheticConfig g;
    g.seed = 42;
    g.symbols = 8;
    g.messages = 200'000;
    sim::SyntheticMarket market(g);
    replay::MemorySource mem;
    std::array<std::byte, 128> buf{};
    std::size_t len = 0;
    Nanos ts = 0;
    while (market.next(buf, len, ts)) mem.add(ts, std::span<const std::byte>(buf.data(), len));
    return mem;
}

template <class S>
void check_golden(const char* name, std::uint64_t expected) {
    replay::MemorySource feed = golden_feed();
    const replay::BacktestReport first = replay::run_backtest(feed, golden_config(), S{});
    feed.rewind();
    const replay::BacktestReport second = replay::run_backtest(feed, golden_config(), S{});

    std::printf("%-18s digest %016llx  orders %llu fills %llu volume %llu total_pnl %lld\n", name,
                static_cast<unsigned long long>(first.digest), static_cast<unsigned long long>(first.orders_sent),
                static_cast<unsigned long long>(first.fills), static_cast<unsigned long long>(first.volume),
                static_cast<long long>(first.total_pnl));

    OT_CHECK(first == second);                 // two identical runs agree on every field
    OT_CHECK_EQ(first.dropped_reports, std::uint64_t{0});
    OT_CHECK(first.orders_sent > 0);           // the digest covers real activity, not an idle run
    OT_CHECK(first.fills > 0);
    OT_CHECK_EQ(first.digest, expected);
}

OT_TEST(golden_digest_imbalance_taker) {
    check_golden<strategy::ImbalanceTaker>("imbalance_taker", kExpectedImbalanceTaker);
}
OT_TEST(golden_digest_microprice_maker) {
    check_golden<strategy::MicropriceMaker>("microprice_maker", kExpectedMicropriceMaker);
}
OT_TEST(golden_digest_ema_cross) { check_golden<strategy::EmaCross>("ema_cross", kExpectedEmaCross); }

OT_TEST(golden_digests_differ_between_strategies) {
    const std::uint64_t all[] = {kExpectedImbalanceTaker, kExpectedMicropriceMaker, kExpectedEmaCross};
    OT_CHECK(all[0] != all[1] && all[0] != all[2] && all[1] != all[2]);
}

}  // namespace

OT_TEST_MAIN()

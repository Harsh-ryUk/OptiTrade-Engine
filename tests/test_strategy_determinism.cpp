// Determinism and cross-strategy invariants.
//
// A seeded generator produces a long script of book updates over several instruments (with
// alternating buy/sell-heavy regimes and a drifting price, so every strategy has something
// to trade). The script is played into real MarketBooks, the strategy is called after each
// update, and a trivial exchange (Responder) answers every order at once. The sequence of
// emitted actions must be identical on a second run with fresh objects, must differ for a
// different seed, and must match a digest recorded once: everything involved is integer
// arithmetic, so the digest is the same on every compiler and platform.

#include <cstdint>
#include <cstdlib>
#include <utility>
#include <type_traits>
#include <vector>

#include "check.hpp"
#include "optitrade/core/digest.hpp"
#include "optitrade/core/rng.hpp"
#include "optitrade/strategy/ema_cross.hpp"
#include "optitrade/strategy/imbalance_taker.hpp"
#include "optitrade/strategy/microprice_maker.hpp"
#include "test_strategy_util.hpp"

using namespace optitrade;
using namespace ot_strat;

static_assert(strategy::Strategy<strategy::ImbalanceTaker>);
static_assert(strategy::Strategy<strategy::MicropriceMaker>);
static_assert(strategy::Strategy<strategy::EmaCross>);
static_assert(std::is_same_v<decltype(strategy::ImbalanceTaker::kName), const char* const>);
static_assert(std::is_same_v<decltype(strategy::MicropriceMaker::kName), const char* const>);
static_assert(std::is_same_v<decltype(strategy::EmaCross::kName), const char* const>);

namespace {

constexpr std::size_t kMaxLocates = 8;
constexpr int kSteps = 6000;

struct Result {
    std::vector<Action> actions;
    std::uint64_t digest{};
    std::int64_t final_position{};
};

template <class S>
Result play(std::uint64_t seed, const typename S::Config& cfg) {
    Harness h;
    S s(cfg);
    Responder<S> ex(h, s);
    Rng rng(seed);
    Price mid[4] = {1'000'000, 2'500'000, 300'000, 5'000'000};
    for (int i = 0; i < kSteps; ++i) {
        // Locates 1..3 are inside the strategies' range; locate 12 is beyond max_locates.
        static constexpr Locate kLocs[4] = {1, 2, 3, 12};
        const std::size_t k = rng.bounded(4);
        const Locate loc = kLocs[k];

        const bool bid_regime = (i / 200) % 2 == 0;
        mid[k] += (bid_regime ? 60 : -60) + rng.range(-150, 150);
        if (mid[k] < 50'000) mid[k] = 50'000;
        const Price bid_top = mid[k] / 100 * 100;
        const Price ask_top = bid_top + (1 + static_cast<Price>(rng.bounded(3))) * 100;

        std::vector<std::pair<Price, Qty>> bids, asks;
        for (Price d = 0; d < 3; ++d) {
            const auto heavy = static_cast<Qty>(rng.range(100, 1200));
            const auto light = static_cast<Qty>(rng.range(20, 300));
            bids.push_back({bid_top - d * 100, bid_regime ? heavy : light});
            asks.push_back({ask_top + d * 100, bid_regime ? light : heavy});
        }
        h.set_book(loc, bids, asks);
        h.now += 1 + rng.bounded(5000);
        s.on_book_update(loc, h.ctx());
        ex.settle();
    }
    Result r;
    r.actions = h.orders.actions;
    Digest d;
    for (const Action& a : r.actions) {
        d.update(static_cast<std::uint64_t>(a.kind));
        d.update(a.now);
        d.update(a.id);
        d.update(a.locate);
        d.update(static_cast<std::uint64_t>(a.side));
        d.update(static_cast<std::uint64_t>(a.price));
        d.update(a.qty);
        d.update(static_cast<std::uint64_t>(a.tif));
    }
    r.digest = d.value();
    r.final_position = h.risk.position(1).qty;
    return r;
}

template <class S>
void check_strategy(const typename S::Config& cfg, std::uint64_t expected_digest, std::size_t expected_actions) {
    const Result a = play<S>(7, cfg);
    const Result b = play<S>(7, cfg);
    OT_CHECK(a.actions.size() == b.actions.size());
    OT_CHECK(a.actions == b.actions);
    OT_CHECK_EQ(a.digest, b.digest);
    OT_CHECK(play<S>(8, cfg).digest != a.digest);

    OT_CHECK(a.actions.size() > 50);  // the script really exercises the strategy
    for (const Action& x : a.actions) {
        // Locate 12 is outside max_locates = 8; nothing may be sent for it.
        OT_CHECK(x.locate < kMaxLocates);
        if (x.kind == Action::Kind::cancel) continue;
        OT_CHECK(x.qty > 0);
        OT_CHECK(x.price > 0);
    }
    // Recorded from the first verified run; must reproduce on every compiler and platform.
    OT_CHECK_EQ(a.digest, expected_digest);
    OT_CHECK_EQ(a.actions.size(), expected_actions);
}

}  // namespace

OT_TEST(imbalance_taker_is_deterministic) {
    strategy::ImbalanceTaker::Config c;
    c.max_locates = kMaxLocates;
    c.cooldown_ns = 2000;
    check_strategy<strategy::ImbalanceTaker>(c, 0xafb64bae6c045958ULL, 919);
}

OT_TEST(microprice_maker_is_deterministic) {
    strategy::MicropriceMaker::Config c;
    c.max_locates = kMaxLocates;
    check_strategy<strategy::MicropriceMaker>(c, 0x42d807afd55f48afULL, 3180);
}

OT_TEST(ema_cross_is_deterministic) {
    strategy::EmaCross::Config c;
    c.max_locates = kMaxLocates;
    c.warmup = 16;
    c.fast_shift = 2;
    c.slow_shift = 4;
    c.cooldown_ns = 2000;
    check_strategy<strategy::EmaCross>(c, 0xabab5c5a35877a9cULL, 173);
}

OT_TEST_MAIN()

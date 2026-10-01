#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "optitrade/core/rng.hpp"
#include "optitrade/core/types.hpp"
#include "optitrade/itch/encoder.hpp"
#include "optitrade/itch/messages.hpp"

// Deterministic synthetic ITCH 5.0 market.
//
// The generator exists so that strategies, the exchange simulator and the
// benchmarks have a realistic, reproducible order flow that needs no data licence.
// It draws only from core/rng.hpp and uses integer arithmetic throughout, so a
// seed yields the same byte stream on every compiler and platform.
//
// Stream layout
//   1. System Event 'O'.
//   2. One Stock Directory per symbol ("SYM00", "SYM01", ...; locate = index + 1).
//   3. Preamble: 6 price levels x 2 orders on each side of every symbol, so every
//      book starts with 6 levels of depth per side.
//   4. `messages` flow messages (the nominal mix below).
//
// Flow mix (per mille of flow messages, before the fallbacks described below)
//     Add A/F (one in ten carries a market participant id)   410
//     Order Delete D                                          226
//     Order Cancel X (partial, or full when it exhausts the order)   82
//     Order Executed E                                        144
//     Order Replace U                                          82
//     Executed With Price C                                    21
//     Trade P (non-displayed, no book effect)                  31
//     System Event S (alternating 'Q' / 'M')                    4
//
// Shadow model. The generator keeps its own compact copy of every live order
// (reference, price, remaining quantity), in fixed arrays sized at construction:
// kCap orders per side per symbol. Cancels, deletes, executions and replaces are
// only ever aimed at orders in that model, so a consumer never sees an unknown
// reference. Executions always take the best-priced resting order on the side
// opposite the (random) aggressor, earliest reference first at equal price, and
// they report a quantity no larger than what remains.
//
// Prices. A symbol has a fair value that follows a bounded random walk of one tick
// with probability 1/32 per message that touches the symbol. A new order is placed
// (1 + g) ticks away from fair value on its own side, g geometric with mean 2, so
// the touch is dense and depth thins out. Orders never cross: an Add that would
// reach the opposite best is emitted as an execution against that order instead,
// and a Replace that would cross keeps its old price. Best price and time priority
// are found by scanning the (small) per-symbol arrays; nothing here is on a
// latency-critical path.
//
// Regimes. Flow alternates between a buy-heavy and a sell-heavy regime every
// `regime_period` flow messages (buy-heavy first). In a buy-heavy regime 60 % of
// new orders are bids, 60 % of aggressors are buyers (executions eat the asks) and
// fair value drifts up (55 % of steps). Deletes and cancels pick uniformly among
// all live orders of the symbol, so the book populations relax towards the same
// 60/40 split; the result is a persistent quantity imbalance whose sign flips at
// each regime boundary, which is what imbalance and momentum strategies trade.
//
// Population control. Deletes and full removals are the only ways an order
// disappears, so a fixed mix would let the population random-walk away from its
// target. Instead the probability that an Executed/Cancel exhausts its order
// rises and falls (linearly, clamped to [0, 1]) with the symbol's population
// around kTargetPopulation. The nominal mix is unchanged; only the split between
// partial and full removals adapts. If an Add finds its side full or a message
// needs an order on an empty book, it falls back to a Delete or an Add; this is
// unreachable in normal operation and keeps the model bounds-safe otherwise.
//
// Time. Message gaps are drawn from an integer approximation of an exponential
// distribution with mean `mean_gap_ns` (gap = mean * (k + u) / 1.5 with k the
// leading-zero count of a 64-bit draw, i.e. Geometric(1/2), and u uniform in
// [0, 1)). Timestamps are non-decreasing, start at 09:30:00 and saturate at
// 2^48 - 1, the largest value the wire can carry.
//
// Parameter sanitising: symbols is at least 1; tick is clamped to [1, 1'000'000];
// start_price to [32 ticks, 1'000'000'000]; mean_gap_ns to at most 2^32;
// regime_period to at least 1. Memory: about 6 KiB per symbol.
namespace optitrade::sim {

struct SyntheticConfig {
    std::uint64_t seed{1};
    std::uint16_t symbols{8};
    std::uint64_t messages{100'000};  // flow messages after the preamble
    Price start_price{1'000'000};
    Price tick{100};
    Nanos mean_gap_ns{5'000};
    std::uint32_t regime_period{20'000};
};

class SyntheticMarket {
public:
    // Orders kept per side per symbol in the shadow model.
    static constexpr std::size_t kCap = 128;

    explicit SyntheticMarket(const SyntheticConfig& cfg)
        : rng_(cfg.seed),
          symbols_(std::max<std::uint16_t>(cfg.symbols, 1)),
          flow_total_(cfg.messages),
          tick_(std::clamp<Price>(cfg.tick, 1, 1'000'000)),
          period_(std::max<std::uint32_t>(cfg.regime_period, 1)),
          mean_gap_(std::min<Nanos>(cfg.mean_gap_ns, Nanos{1} << 32)),
          syms_(symbols_),
          book_(std::size_t{symbols_} * 2 * kCap) {
        const Price start = std::clamp<Price>(cfg.start_price, 32 * tick_, 1'000'000'000);
        for (std::size_t i = 0; i < symbols_; ++i) {
            Sym& s = syms_[i];
            s.name = make_symbol(i);
            const Price fair = round_tick(start + static_cast<Price>(i % 32) * 4 * tick_);
            s.fair = fair;
            s.lo = round_tick(fair / 2);
            s.hi = round_tick(fair + fair / 2);
        }
    }

    // Writes the next unframed ITCH message into `out` (at least
    // itch::kMaxMessageLength bytes). Returns false, changing nothing, when the
    // stream is finished or `out` is too small.
    bool next(std::span<std::byte> out, std::size_t& len, Nanos& ts) noexcept {
        len = 0;
        if (out.size() < itch::kMaxMessageLength) return false;
        if (phase_ == Phase::flow && flow_index_ >= flow_total_) phase_ = Phase::done;
        if (phase_ == Phase::done) return false;
        advance_time();

        std::size_t n = 0;
        switch (phase_) {
            case Phase::open: {
                itch::SystemEvent m{header(0), 'O'};
                n = itch::encode(m, out);
                phase_ = Phase::directory;
                break;
            }
            case Phase::directory:
                n = gen_directory(out);
                if (++cursor_ == symbols_) {
                    cursor_ = 0;
                    phase_ = Phase::preamble;
                }
                break;
            case Phase::preamble:
                n = gen_preamble(out);
                if (++cursor_ == std::size_t{symbols_} * kPreambleOrders) phase_ = Phase::flow;
                break;
            case Phase::flow:
                bull_ = ((flow_index_ / period_) & 1) == 0;
                n = gen_flow(out);
                ++flow_index_;
                break;
            case Phase::done: break;
        }
        len = n;
        ts = ts_;
        ++emitted_;
        return n != 0;
    }

    std::uint64_t emitted() const noexcept { return emitted_; }

    // Messages preceding the first flow message (System Event, directories, depth
    // preamble); the flow proper is stream positions [preamble_length(), total).
    std::uint64_t preamble_length() const noexcept {
        return 1 + symbols_ + std::uint64_t{symbols_} * kPreambleOrders;
    }

private:
    enum class Phase : std::uint8_t { open, directory, preamble, flow, done };

    struct Live {
        OrderRef ref;
        Price price;
        Qty qty;
    };
    struct Sym {
        Symbol name;
        Price fair{};
        Price lo{};
        Price hi{};
        std::uint32_t n[2]{};
    };

    static constexpr std::size_t kPreambleLevels = 6;
    static constexpr std::size_t kPreambleOrders = kPreambleLevels * 4;  // per symbol: 6 levels x 2 sides x 2 orders
    static constexpr unsigned kTargetPopulation = 48;
    static constexpr Nanos kMaxTimestamp = (Nanos{1} << 48) - 1;
    static constexpr Nanos kSessionStart = 34'200'000'000'000ULL;  // 09:30:00

    // ---- helpers -----------------------------------------------------------

    static Symbol make_symbol(std::size_t index) noexcept {
        char buf[Symbol::kSize] = {'S', 'Y', 'M', '0', '0', ' ', ' ', ' '};
        char digits[5];
        std::size_t nd = 0;
        for (std::size_t v = index; v != 0 || nd == 0; v /= 10) digits[nd++] = static_cast<char>('0' + v % 10);
        const std::size_t pos = nd < 2 ? 3 + (2 - nd) : 3;  // at least two digits, zero padded
        for (std::size_t i = 0; i < nd; ++i) buf[pos + i] = digits[nd - 1 - i];
        return Symbol::from_wire(buf);
    }

    Price round_tick(Price p) const noexcept { return (p / tick_) * tick_; }

    itch::Header header(Locate locate) const noexcept { return {locate, 0, ts_}; }
    Locate locate_of(std::size_t s) const noexcept { return static_cast<Locate>(s + 1); }

    void advance_time() noexcept {
        Nanos gap = 0;
        if (mean_gap_ != 0) {
            const std::uint64_t x = rng_.next();
            const auto k = static_cast<std::uint64_t>(std::countl_zero(x | 1));
            const std::uint64_t frac = rng_.bounded(1024);
            gap = mean_gap_ * (k * 1024 + frac) / 1536;
        }
        ts_ = gap > kMaxTimestamp - ts_ ? kMaxTimestamp : ts_ + gap;
    }

    Live* orders(std::size_t s, Side side) noexcept { return &book_[(s * 2 + index(side)) * kCap]; }
    const Live* orders(std::size_t s, Side side) const noexcept { return &book_[(s * 2 + index(side)) * kCap]; }
    unsigned population(std::size_t s) const noexcept { return syms_[s].n[0] + syms_[s].n[1]; }

    void remove_at(std::size_t s, Side side, std::size_t i) noexcept {
        Live* a = orders(s, side);
        std::uint32_t& n = syms_[s].n[index(side)];
        a[i] = a[n - 1];
        --n;
    }

    // Index of the best-priced order on `side` (highest bid / lowest ask, oldest
    // reference first at equal price), or -1 when the side is empty.
    int best_of(std::size_t s, Side side) const noexcept {
        const Live* a = orders(s, side);
        const std::uint32_t n = syms_[s].n[index(side)];
        int best = -1;
        for (std::uint32_t i = 0; i < n; ++i) {
            if (best < 0) {
                best = static_cast<int>(i);
                continue;
            }
            const Live& b = a[best];
            const bool better = side == Side::buy ? a[i].price > b.price : a[i].price < b.price;
            if (better || (a[i].price == b.price && a[i].ref < b.ref)) best = static_cast<int>(i);
        }
        return best;
    }

    static bool crosses(Side side, Price px, Price opposite_best) noexcept {
        return side == Side::buy ? px >= opposite_best : px <= opposite_best;
    }

    Side pick_side() noexcept {
        const bool buy = rng_.chance(bull_ ? 60 : 40, 100);
        return buy ? Side::buy : Side::sell;
    }

    Qty random_qty() noexcept { return static_cast<Qty>(100 * (1 + rng_.bounded(10))); }

    // Shares removed by an execution or cancel: everything with a probability that
    // steers the population back to kTargetPopulation, otherwise a strict part.
    Qty removal_shares(Qty qty, unsigned pop) noexcept {
        if (qty < 2) return qty;
        const int q = std::clamp(750 + (static_cast<int>(pop) - static_cast<int>(kTargetPopulation)) * 15, 0, 1000);
        if (rng_.bounded(1000) < static_cast<std::uint64_t>(q)) return qty;
        return static_cast<Qty>(1 + rng_.bounded(qty - 1));
    }

    void reduce(std::size_t s, Side side, std::size_t i, Qty shares) noexcept {
        Live& o = orders(s, side)[i];
        if (shares >= o.qty) {
            remove_at(s, side, i);
        } else {
            o.qty -= shares;
        }
    }

    void push(std::size_t s, Side side, OrderRef ref, Price px, Qty qty) noexcept {
        std::uint32_t& n = syms_[s].n[index(side)];
        orders(s, side)[n++] = Live{ref, px, qty};
    }

    // Uniform pick over both sides of a symbol; the symbol must hold an order.
    void pick_any(std::size_t s, Side& side, std::size_t& i) noexcept {
        const std::uint64_t nb = syms_[s].n[0];
        const std::uint64_t k = rng_.bounded(nb + syms_[s].n[1]);
        side = k < nb ? Side::buy : Side::sell;
        i = static_cast<std::size_t>(k < nb ? k : k - nb);
    }

    // Price (1 + g) ticks from fair value on `side`; may still cross the book.
    Price passive_price(std::size_t s, Side side) noexcept {
        const auto g = static_cast<Price>(rng_.geometric(1, 3, 15));
        const Price dist = (1 + g) * tick_;
        const Price fair = syms_[s].fair;
        return side == Side::buy ? std::max(tick_, fair - dist) : fair + dist;
    }

    void step_fair(std::size_t s) noexcept {
        if (rng_.bounded(32) != 0) return;
        const bool up = rng_.chance(bull_ ? 11 : 9, 20);
        Sym& y = syms_[s];
        y.fair = std::clamp(y.fair + (up ? tick_ : -tick_), y.lo, y.hi);
    }

    // ---- generators (each writes one message and updates the shadow model) --

    std::size_t gen_directory(std::span<std::byte> out) noexcept {
        itch::StockDirectory m;
        m.h = header(locate_of(cursor_));
        m.symbol = syms_[cursor_].name;
        m.market_category = 'Q';
        m.financial_status = 'N';
        m.round_lot_size = 100;
        m.round_lots_only = 'N';
        m.issue_classification = 'C';
        m.issue_subtype[0] = 'Z';
        m.issue_subtype[1] = ' ';
        m.authenticity = 'P';
        m.short_sale_threshold = 'N';
        m.ipo_flag = ' ';
        m.luld_tier = '1';
        m.etp_flag = 'N';
        m.etp_leverage = 0;
        m.inverse = 'N';
        return itch::encode(m, out);
    }

    // Order i of a symbol's preamble: level 1..6, buy then sell, two orders each.
    std::size_t gen_preamble(std::span<std::byte> out) noexcept {
        const std::size_t s = cursor_ / kPreambleOrders;
        const std::size_t i = cursor_ % kPreambleOrders;
        const Side side = ((i / 2) & 1) == 0 ? Side::buy : Side::sell;
        const Price dist = static_cast<Price>(i / 4 + 1) * tick_;
        const Price px = side == Side::buy ? syms_[s].fair - dist : syms_[s].fair + dist;
        const Qty qty = random_qty();
        return emit_add(s, side, px, qty, false, out);
    }

    std::size_t emit_add(std::size_t s, Side side, Price px, Qty qty, bool attributed,
                         std::span<std::byte> out) noexcept {
        itch::AddOrder m;
        m.h = header(locate_of(s));
        m.ref = next_ref_++;
        m.side = side;
        m.shares = qty;
        m.symbol = syms_[s].name;
        m.price = px;
        m.has_attribution = attributed;
        if (attributed) std::copy_n("SYNT", 4, m.mpid);
        push(s, side, m.ref, px, qty);
        return itch::encode(m, out);
    }

    std::size_t gen_add(std::size_t s, std::span<std::byte> out) noexcept {
        const Side side = pick_side();
        const Price px = passive_price(s, side);
        const Qty qty = random_qty();
        const bool attributed = rng_.chance(1, 10);
        const int opp = best_of(s, opposite(side));
        if (opp >= 0 && crosses(side, px, orders(s, opposite(side))[opp].price)) {
            return gen_execute(s, opposite(side), qty, false, out);  // aggressive intent trades instead
        }
        if (syms_[s].n[index(side)] >= kCap) return gen_delete(s, out);
        return emit_add(s, side, px, qty, attributed, out);
    }

    std::size_t gen_delete(std::size_t s, std::span<std::byte> out) noexcept {
        if (population(s) == 0) return gen_add(s, out);
        Side side{};
        std::size_t i = 0;
        pick_any(s, side, i);
        itch::OrderDelete m{header(locate_of(s)), orders(s, side)[i].ref};
        remove_at(s, side, i);
        return itch::encode(m, out);
    }

    std::size_t gen_cancel(std::size_t s, std::span<std::byte> out) noexcept {
        if (population(s) == 0) return gen_add(s, out);
        Side side{};
        std::size_t i = 0;
        pick_any(s, side, i);
        const Live& o = orders(s, side)[i];
        itch::OrderCancel m{header(locate_of(s)), o.ref, removal_shares(o.qty, population(s))};
        reduce(s, side, i, m.shares);
        return itch::encode(m, out);
    }

    // Executes against the best order of `target`. `want` == 0 lets the population
    // policy choose the size; otherwise it is capped by what the order holds.
    std::size_t gen_execute(std::size_t s, Side target, Qty want, bool with_price,
                            std::span<std::byte> out) noexcept {
        const int best = best_of(s, target);
        if (best < 0) return gen_add(s, out);
        const auto i = static_cast<std::size_t>(best);
        const Live o = orders(s, target)[i];
        const Qty shares = want != 0 ? std::min(want, o.qty) : removal_shares(o.qty, population(s));
        const std::uint64_t match = ++match_;
        std::size_t n = 0;
        if (with_price) {
            // One execution in four prints a tick inside the displayed price, so
            // last_trade() and the resting price are not always the same number.
            Price px = o.price;
            if (rng_.chance(1, 4)) {
                const Price improved = target == Side::buy ? px - tick_ : px + tick_;
                if (improved >= tick_) px = improved;
            }
            itch::OrderExecutedPrice m{header(locate_of(s)), o.ref, shares, match, true, px};
            n = itch::encode(m, out);
        } else {
            itch::OrderExecuted m{header(locate_of(s)), o.ref, shares, match};
            n = itch::encode(m, out);
        }
        reduce(s, target, i, shares);
        return n;
    }

    std::size_t gen_exec_flow(std::size_t s, bool with_price, std::span<std::byte> out) noexcept {
        const Side aggressor = pick_side();
        Side target = opposite(aggressor);
        if (syms_[s].n[index(target)] == 0) target = aggressor;
        if (syms_[s].n[index(target)] == 0) return gen_add(s, out);
        return gen_execute(s, target, 0, with_price, out);
    }

    std::size_t gen_replace(std::size_t s, std::span<std::byte> out) noexcept {
        if (population(s) == 0) return gen_add(s, out);
        Side side{};
        std::size_t i = 0;
        pick_any(s, side, i);
        Live& o = orders(s, side)[i];
        Price px = passive_price(s, side);
        const Qty qty = random_qty();
        const int opp = best_of(s, opposite(side));
        if (opp >= 0 && crosses(side, px, orders(s, opposite(side))[opp].price)) px = o.price;
        itch::OrderReplace m{header(locate_of(s)), o.ref, next_ref_++, qty, px};
        o = Live{m.new_ref, px, qty};
        return itch::encode(m, out);
    }

    std::size_t gen_trade(std::size_t s, std::span<std::byte> out) noexcept {
        const Side side = pick_side();
        const Qty shares = static_cast<Qty>(100 * (1 + rng_.bounded(5)));
        Price px = syms_[s].fair;
        const int bb = best_of(s, Side::buy);
        const int ba = best_of(s, Side::sell);
        if (bb >= 0 && ba >= 0) {
            px = round_tick((orders(s, Side::buy)[bb].price + orders(s, Side::sell)[ba].price) / 2);
        }
        itch::Trade m;
        m.h = header(locate_of(s));
        m.ref = 0;  // non-displayed liquidity carries no order reference
        m.side = side;
        m.shares = shares;
        m.symbol = syms_[s].name;
        m.price = px;
        m.match = ++match_;
        return itch::encode(m, out);
    }

    std::size_t gen_flow(std::span<std::byte> out) noexcept {
        const auto s = static_cast<std::size_t>(rng_.bounded(symbols_));
        step_fair(s);
        const std::uint64_t r = rng_.bounded(1000);
        if (r < 410) return gen_add(s, out);
        if (r < 636) return gen_delete(s, out);
        if (r < 718) return gen_cancel(s, out);
        if (r < 862) return gen_exec_flow(s, false, out);
        if (r < 944) return gen_replace(s, out);
        if (r < 965) return gen_exec_flow(s, true, out);
        if (r < 996) return gen_trade(s, out);
        itch::SystemEvent m{header(0), (event_toggle_++ & 1) == 0 ? 'Q' : 'M'};
        return itch::encode(m, out);
    }

    Rng rng_;
    std::uint16_t symbols_;
    std::uint64_t flow_total_;
    Price tick_;
    std::uint32_t period_;
    Nanos mean_gap_;
    std::vector<Sym> syms_;
    std::vector<Live> book_;  // [symbol][side][kCap], allocated once

    Phase phase_{Phase::open};
    std::size_t cursor_{0};
    std::uint64_t flow_index_{0};
    std::uint64_t emitted_{0};
    OrderRef next_ref_{1};
    std::uint64_t match_{0};
    std::uint32_t event_toggle_{0};
    Nanos ts_{kSessionStart};
    bool bull_{true};
};

}  // namespace optitrade::sim

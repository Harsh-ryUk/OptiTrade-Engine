#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "optitrade/strategy/strategy.hpp"

namespace optitrade::strategy {

struct EmaCrossConfig {
    std::size_t max_locates{1024};   // locates >= this are ignored
    std::uint32_t fast_shift{3};     // fast EMA weight 2^-fast_shift, 1..30
    std::uint32_t slow_shift{6};     // slow EMA weight 2^-slow_shift, fast_shift+1..31
    std::uint32_t warmup{64};        // book updates seen before any cross can arm a trade
    std::uint32_t confirm_ticks{3};  // consecutive updates on the new side before entering, >= 1
    Qty order_qty{100};              // size of one order; also the size of the target position
    std::int64_t max_position{100};  // target position is min(order_qty, max_position)
    Nanos cooldown_ns{1'000'000};    // minimum time between two orders on one instrument
};

// Exponential moving average crossover on the mid price.
//
// Averages. The sample is bid + ask (twice the mid, so no rounding is lost) and both
// averages are kept in fixed point with kFrac fractional bits:
//     ema += ((sample << kFrac) - ema) >> shift
// The right shift of a negative difference is arithmetic (guaranteed since C++20), i.e.
// it rounds toward minus infinity, giving a bias of at most one unit of 2^-kFrac per
// step, far below one price unit. The first sample seeds both averages. Every update of
// a tradable book (see detail::touch_of) is one sample.
//
// Cross. sign = sgn(fast - slow); an exact tie has no sign and cancels a pending
// confirmation. A change to the opposite sign, or the first non-zero sign, is a cross;
// it arms a trade only when it happens after the warm-up, so the seeding transient and
// any trend already under way when trading starts cannot trigger an entry. `run` counts
// consecutive updates with the same sign, starting at 1 on the cross itself.
//
// Position target, re-evaluated on every update (after warm-up) so that an IOC that did
// not fill is simply retried:
//   armed and run >= confirm_ticks  -> target = sign * min(order_qty, max_position)
//   else holding a position against the current sign  -> target = 0 (exit on the
//                                        opposite cross, without waiting for confirmation)
//   otherwise                       -> no action
// Each order moves toward the target by at most order_qty, IOC at the touch (buy at the
// ask, sell at the bid).
//
// Pacing: one outstanding order per instrument and a cooldown between attempts (see
// detail::OrderGate).
class EmaCross {
public:
    using Config = EmaCrossConfig;
    static constexpr const char* kName = "ema_cross";
    static constexpr unsigned kFrac = 8;  // fractional bits of the fixed-point averages

    explicit EmaCross(const Config& cfg = {}) : cfg_(sanitize(cfg)), slots_(cfg_.max_locates) {}

    const Config& config() const noexcept { return cfg_; }

    // Fixed-point (kFrac bits) averages of bid + ask, for tests and diagnostics.
    std::int64_t fast(Locate l) const noexcept { return l < slots_.size() ? slots_[l].fast : 0; }
    std::int64_t slow(Locate l) const noexcept { return l < slots_.size() ? slots_[l].slow : 0; }
    std::uint32_t samples(Locate l) const noexcept { return l < slots_.size() ? slots_[l].samples : 0; }

    void on_book_update(Locate l, const Context& ctx) noexcept {
        if (l >= slots_.size()) return;
        const auto touch = detail::touch_of(ctx.books.book(l));
        if (!touch) return;
        Slot& s = slots_[l];

        const std::int64_t sample = (touch->bid + touch->ask) * (std::int64_t{1} << kFrac);
        if (s.samples == 0) {
            s.fast = s.slow = sample;
        } else {
            s.fast += (sample - s.fast) >> cfg_.fast_shift;
            s.slow += (sample - s.slow) >> cfg_.slow_shift;
        }
        if (s.samples != UINT32_MAX) ++s.samples;
        const bool warm = s.samples >= cfg_.warmup;

        const int sign = s.fast > s.slow ? 1 : s.fast < s.slow ? -1 : 0;
        if (sign == 0) {
            s.run = 0;
            s.armed = false;
        } else if (sign == s.sign) {
            if (s.run != UINT32_MAX) ++s.run;
        } else {
            s.sign = sign;
            s.run = 1;
            s.armed = warm;
        }
        if (!warm || s.sign == 0) return;

        const std::int64_t pos = detail::position_of(ctx.risk, l);
        std::int64_t target = pos;
        if (s.armed && s.run >= cfg_.confirm_ticks) {
            target = s.sign * std::min<std::int64_t>(cfg_.order_qty, cfg_.max_position);
        } else if (pos != 0 && (pos > 0) != (s.sign > 0)) {
            target = 0;
        }
        if (target == pos) return;

        const bool buy = target > pos;
        const Price price = buy ? touch->ask : touch->bid;
        const auto qty = static_cast<Qty>(std::min<std::int64_t>(buy ? target - pos : pos - target,
                                                                 cfg_.order_qty));
        if (!s.gate.ready(ctx.now, cfg_.cooldown_ns, ctx.orders)) return;
        s.gate.sent(ctx.now, ctx.orders.submit(oms::OrderRequest{l, buy ? Side::buy : Side::sell, price, qty,
                                                                 oms::Tif::ioc},
                                               ctx.now));
    }

    void on_fill(const oms::Fill&, const Context&) noexcept {}

    void on_order_update(const oms::OrderInfo& o, const Context&) noexcept {
        if (o.req.locate < slots_.size()) slots_[o.req.locate].gate.on_update(o);
    }

private:
    struct Slot {
        std::int64_t fast{};
        std::int64_t slow{};
        std::uint32_t samples{};
        std::uint32_t run{};
        int sign{};  // sign of fast - slow at the last non-tie update, 0 before the first
        bool armed{};
        detail::OrderGate gate;
    };

    static Config sanitize(Config c) noexcept {
        c.fast_shift = std::clamp<std::uint32_t>(c.fast_shift, 1, 30);
        c.slow_shift = std::clamp<std::uint32_t>(c.slow_shift, c.fast_shift + 1, 31);
        c.confirm_ticks = std::max<std::uint32_t>(c.confirm_ticks, 1);
        c.order_qty = std::clamp<Qty>(c.order_qty, 1, kMaxOrderQty);
        c.max_position = std::clamp<std::int64_t>(c.max_position, 0, detail::kPositionBound);
        c.max_locates = std::min(c.max_locates, risk::RiskEngine::kMaxLocates);
        return c;
    }

    Config cfg_;
    std::vector<Slot> slots_;
};

static_assert(Strategy<EmaCross>);

}  // namespace optitrade::strategy

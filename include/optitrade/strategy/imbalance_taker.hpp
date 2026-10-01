#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "optitrade/strategy/strategy.hpp"

namespace optitrade::strategy {

struct ImbalanceTakerConfig {
    std::size_t max_locates{1024};   // locates >= this are ignored
    std::size_t depth{5};            // price levels per side that enter the imbalance
    std::int64_t enter_bps{3000};    // |imbalance| needed to open or add, 1..10000
    std::int64_t exit_bps{500};      // flatten when the signal decays to this, 0..enter_bps-1
    Price max_spread{500};           // no entries when ask - bid is wider (price units, 1e-4)
    Qty order_qty{100};              // size of one order, entries and exits alike
    std::int64_t max_position{500};  // absolute share limit per instrument (risk enforces it too)
    Nanos cooldown_ns{1'000'000};    // minimum time between two orders on one instrument
};

// Order book imbalance taker.
//
// Signal. With B and A the shares resting on the best `depth` bid and ask levels,
//     imbalance_bps = (B - A) * 10000 / (B + A)
// in integer arithmetic (C++ division truncates toward zero, so the signal is exactly
// antisymmetric: swapping the sides negates it). Levels are summed unweighted: the
// touch already dominates because it is the part of the queue that trades first, and a
// plain sum keeps the value easy to audit against the book. Range -10000..+10000.
//
// Rules, evaluated per book update for the instrument, first match wins:
//   1. Long and the signal has fallen to exit_bps or below: sell IOC at the bid.
//   2. Short and the signal has risen to -exit_bps or above: buy IOC at the ask.
//   3. Spread <= max_spread and signal >= enter_bps: buy IOC at the ask.
//      Spread <= max_spread and signal <= -enter_bps: sell IOC at the bid.
// Entries are clipped so the position never passes max_position. Because exit_bps is
// below enter_bps the strategy holds through the band between them (hysteresis) and
// cannot flip on noise around a single threshold. Exits are not spread-gated: getting
// out is never worse for a wide spread than staying in. An opposite signal first
// flattens (rule 1 or 2); it opens the new side only on a later update, after the
// cooldown, once the position is actually flat.
//
// Pacing: one outstanding order per instrument and a cooldown between attempts (see
// detail::OrderGate). The position is read from the risk engine, i.e. from fills only.
class ImbalanceTaker {
public:
    using Config = ImbalanceTakerConfig;
    static constexpr const char* kName = "imbalance_taker";

    explicit ImbalanceTaker(const Config& cfg = {})
        : cfg_(sanitize(cfg)), gates_(cfg_.max_locates) {}

    // Exposed so the arithmetic can be checked against hand-computed values.
    static constexpr std::int64_t imbalance_bps(std::int64_t bid_qty, std::int64_t ask_qty) noexcept {
        const std::int64_t total = bid_qty + ask_qty;
        return total == 0 ? 0 : (bid_qty - ask_qty) * 10'000 / total;
    }

    const Config& config() const noexcept { return cfg_; }

    void on_book_update(Locate l, const Context& ctx) noexcept {
        if (l >= gates_.size()) return;
        const book::OrderBook* b = ctx.books.book(l);
        const auto touch = detail::touch_of(b);
        if (!touch) return;

        const std::int64_t imb = imbalance_bps(b->total_qty(Side::buy, cfg_.depth),
                                               b->total_qty(Side::sell, cfg_.depth));
        const std::int64_t pos = detail::position_of(ctx.risk, l);

        Side side = Side::buy;
        Price price = 0;
        std::int64_t qty = 0;
        if (pos > 0 && imb <= cfg_.exit_bps) {
            side = Side::sell, price = touch->bid, qty = pos;
        } else if (pos < 0 && imb >= -cfg_.exit_bps) {
            side = Side::buy, price = touch->ask, qty = -pos;
        } else if (touch->ask - touch->bid <= cfg_.max_spread) {
            if (imb >= cfg_.enter_bps && pos < cfg_.max_position) {
                side = Side::buy, price = touch->ask, qty = cfg_.max_position - pos;
            } else if (imb <= -cfg_.enter_bps && -pos < cfg_.max_position) {
                side = Side::sell, price = touch->bid, qty = cfg_.max_position + pos;
            }
        }
        if (qty <= 0 || price <= 0) return;

        detail::OrderGate& gate = gates_[l];
        if (!gate.ready(ctx.now, cfg_.cooldown_ns, ctx.orders)) return;
        const auto q = static_cast<Qty>(std::min<std::int64_t>(qty, cfg_.order_qty));
        gate.sent(ctx.now, ctx.orders.submit(oms::OrderRequest{l, side, price, q, oms::Tif::ioc}, ctx.now));
    }

    void on_fill(const oms::Fill&, const Context&) noexcept {}

    void on_order_update(const oms::OrderInfo& o, const Context&) noexcept {
        if (o.req.locate < gates_.size()) gates_[o.req.locate].on_update(o);
    }

private:
    // Out-of-range settings are clamped rather than rejected: a strategy that cannot
    // be constructed is worse than one that trades a slightly different size.
    static Config sanitize(Config c) noexcept {
        c.depth = std::max<std::size_t>(c.depth, 1);
        c.enter_bps = std::clamp<std::int64_t>(c.enter_bps, 1, 10'000);
        c.exit_bps = std::clamp<std::int64_t>(c.exit_bps, 0, c.enter_bps - 1);
        c.order_qty = std::clamp<Qty>(c.order_qty, 1, kMaxOrderQty);
        c.max_position = std::clamp<std::int64_t>(c.max_position, 0, detail::kPositionBound);
        c.max_locates = std::min(c.max_locates, risk::RiskEngine::kMaxLocates);
        return c;
    }

    Config cfg_;
    std::vector<detail::OrderGate> gates_;
};

static_assert(Strategy<ImbalanceTaker>);

}  // namespace optitrade::strategy

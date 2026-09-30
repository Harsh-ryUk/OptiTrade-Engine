#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "optitrade/strategy/strategy.hpp"

namespace optitrade::strategy {

struct MicropriceMakerConfig {
    std::size_t max_locates{1024};      // locates >= this are ignored
    Price half_spread{200};             // quote distance from fair value, price units (1e-4), >= 1
    Price tick{100};                    // quote price grid, price units, >= 1
    Price skew_per_share{1};            // quotes move by this much per share of inventory, 0..1e6
    Qty quote_qty{100};                 // size of each quote
    std::int64_t max_inventory{500};    // no more buying at +max, no more selling at -max
    std::uint32_t requote_ticks{2};     // replace only when the target moved by >= this many ticks
    Price max_spread{2000};             // pull quotes when ask - bid is wider (price units)
};

// Two-sided passive market maker around the size-weighted microprice.
//
// Fair value. With best bid b, ask a and their sizes qb, qa
//     fair = (b*qa + a*qb) / (qb + qa)
// which leans toward the ask when the bid queue is the bigger one, the usual "the next
// tick is more likely up" reading. It is computed as b + (a - b)*qb/(qb + qa). The numerator is identical to the textbook
// form up to a multiple of (qb + qa), so the truncated result is bit-for-bit the same, but
// the products stay below 2^63 (the spread is capped at 2^31 and qb below 2^32).
//
// Quotes. inventory is the risk engine's signed position, clamped to +/-max_inventory;
//     bid = floor_tick(fair - half_spread - skew_per_share * inventory)
//     ask =  ceil_tick(fair + half_spread - skew_per_share * inventory)
// so a long position moves both quotes down by exactly skew_per_share per share, making
// the ask easier to hit and the bid harder. Rounding widens the quote (down for bids, up
// for asks). A quote is never priced through the opposite touch: the bid is kept at least
// one tick below the best ask and the ask at least one tick above the best bid, so a
// resting order cannot turn into a taker. Both clamps only widen the quote pair.
//
// Re-quoting. A resting quote is replaced only when its price differs from the target by
// at least requote_ticks * tick (hysteresis, to save messages and queue position); the
// replacement is sized as executed + quote size so a partly filled quote keeps quoting
// a full clip. A side with a pending action (unacknowledged, being replaced or cancelled)
// is left alone until the exchange answers.
//
// Pulling. On a missing, one-sided, crossed or wider-than-max_spread book both quotes are
// cancelled. So is the quote on a side where inventory has reached its limit. The size
// of a new quote is cut to the remaining room, which (with one order per side) keeps
// inventory within max_inventory.
//
// The strategy remembers only its own two OrderIds per instrument. Ids are cleared when
// an order update reports a terminal state (and defensively when find() no longer returns
// a live order).
class MicropriceMaker {
public:
    using Config = MicropriceMakerConfig;
    static constexpr const char* kName = "microprice_maker";

    explicit MicropriceMaker(const Config& cfg = {})
        : cfg_(sanitize(cfg)), quotes_(cfg_.max_locates) {}

    // Exposed so the arithmetic can be checked against hand-computed values.
    static constexpr Price microprice(Price bid, Price ask, Qty bid_qty, Qty ask_qty) noexcept {
        const auto total = static_cast<std::int64_t>(bid_qty) + ask_qty;
        return bid + (ask - bid) * static_cast<std::int64_t>(bid_qty) / total;
    }

    const Config& config() const noexcept { return cfg_; }

    // Working quote ids for tests and diagnostics; 0 = none.
    oms::OrderId bid_id(Locate l) const noexcept { return l < quotes_.size() ? quotes_[l].id[0] : 0; }
    oms::OrderId ask_id(Locate l) const noexcept { return l < quotes_.size() ? quotes_[l].id[1] : 0; }

    void on_book_update(Locate l, const Context& ctx) noexcept {
        if (l >= quotes_.size()) return;
        Quotes& q = quotes_[l];
        const auto touch = detail::touch_of(ctx.books.book(l));
        if (!touch || touch->ask - touch->bid > cfg_.max_spread) {
            pull(q, Side::buy, ctx);
            pull(q, Side::sell, ctx);
            return;
        }

        const Price fair = microprice(touch->bid, touch->ask, touch->bid_qty, touch->ask_qty);
        const std::int64_t inv = std::clamp(detail::position_of(ctx.risk, l), -cfg_.max_inventory,
                                            cfg_.max_inventory);
        const Price skew = cfg_.skew_per_share * inv;

        const Price bid_raw = std::min(fair - cfg_.half_spread - skew, touch->ask - cfg_.tick);
        const Price ask_raw = std::max(fair + cfg_.half_spread - skew, touch->bid + cfg_.tick);
        const auto bid_room = static_cast<Qty>(std::min<std::int64_t>(cfg_.quote_qty, cfg_.max_inventory - inv));
        const auto ask_room = static_cast<Qty>(std::min<std::int64_t>(cfg_.quote_qty, cfg_.max_inventory + inv));

        // A non-positive price cannot be quoted; skip that side instead of clamping to a tick.
        manage(l, q, Side::buy, bid_raw > 0 ? bid_raw - bid_raw % cfg_.tick : 0, bid_room, ctx);
        manage(l, q, Side::sell, ask_raw > 0 ? (ask_raw + cfg_.tick - 1) / cfg_.tick * cfg_.tick : 0,
               ask_room, ctx);
    }

    void on_fill(const oms::Fill&, const Context&) noexcept {}

    void on_order_update(const oms::OrderInfo& o, const Context&) noexcept {
        if (o.req.locate >= quotes_.size() || !oms::is_terminal(o.status)) return;
        for (oms::OrderId& id : quotes_[o.req.locate].id) {
            if (id == o.id) id = 0;
        }
    }

private:
    struct Quotes {
        oms::OrderId id[2]{};  // indexed by index(Side)
    };

    static Config sanitize(Config c) noexcept {
        c.half_spread = std::clamp<Price>(c.half_spread, 1, Price{1} << 40);
        c.tick = std::clamp<Price>(c.tick, 1, Price{1} << 40);
        c.skew_per_share = std::clamp<Price>(c.skew_per_share, 0, 1'000'000);
        c.quote_qty = std::clamp<Qty>(c.quote_qty, 1, kMaxOrderQty);
        c.max_inventory = std::clamp<std::int64_t>(c.max_inventory, 0, 1'000'000'000);
        c.requote_ticks = std::max<std::uint32_t>(c.requote_ticks, 1);
        c.max_spread = std::clamp<Price>(c.max_spread, 0, Price{1} << 31);  // keeps spread * qty < 2^63
        c.max_locates = std::min(c.max_locates, risk::RiskEngine::kMaxLocates);
        return c;
    }

    // The live order behind a tracked id, or nullptr (and the id is forgotten).
    static const oms::OrderInfo* tracked(oms::OrderId& id, const Context& ctx) noexcept {
        if (id == 0) return nullptr;
        const oms::OrderInfo* o = ctx.orders.find(id);
        if (o == nullptr || oms::is_terminal(o->status)) {
            id = 0;
            return nullptr;
        }
        return o;
    }

    // Ask for a cancel unless one is already on its way. The id stays tracked until the
    // terminal update arrives, so a refused cancel is simply retried on the next update.
    static void pull(Quotes& q, Side s, const Context& ctx) noexcept {
        oms::OrderId& id = q.id[index(s)];
        const oms::OrderInfo* o = tracked(id, ctx);
        if (o != nullptr && o->status != oms::OrderStatus::pending_cancel) ctx.orders.cancel(id, ctx.now);
    }

    // `target` <= 0 or `room` == 0 means "do not quote this side".
    void manage(Locate l, Quotes& q, Side s, Price target, Qty room, const Context& ctx) noexcept {
        oms::OrderId& id = q.id[index(s)];
        if (target <= 0 || room == 0) return pull(q, s, ctx);

        const oms::OrderInfo* o = tracked(id, ctx);
        if (o == nullptr) {
            const oms::SubmitResult r =
                ctx.orders.submit(oms::OrderRequest{l, s, target, room, oms::Tif::day}, ctx.now);
            if (r.status == oms::SubmitStatus::ok) id = r.id;
            return;
        }
        if (o->status != oms::OrderStatus::live) return;  // answer pending
        const Price moved = target > o->req.price ? target - o->req.price : o->req.price - target;
        if (moved < static_cast<Price>(cfg_.requote_ticks) * cfg_.tick) return;

        const std::uint64_t total = std::uint64_t{o->cum_qty} + room;
        if (total > kMaxOrderQty) return pull(q, s, ctx);  // cannot express it as a replace
        ctx.orders.replace(id, target, static_cast<Qty>(total), ctx.now);
    }

    Config cfg_;
    std::vector<Quotes> quotes_;
};

static_assert(Strategy<MicropriceMaker>);

}  // namespace optitrade::strategy

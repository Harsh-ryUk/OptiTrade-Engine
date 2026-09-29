#pragma once

// Pre-trade risk checks, exposure bookkeeping and average-cost PnL accounting.
//
// Pre-trade check order. The first failing step is the one reported, so a caller that sees
// `position` knows the earlier, cheaper checks all passed:
//
//   1. kill_switch     manual or automatic halt (overrides everything, even malformed input)
//   2. invalid_order   locate outside the table, side not buy/sell, qty == 0, price <= 0
//   3. order_qty       qty > max_order_qty
//   4. order_notional  price * qty > max_order_notional                (0 disables)
//   5. position        |position +/- same-side open qty +/- qty| > max_position
//   6. gross_position  worst-case gross exposure after the order > max_gross_position (0 disables)
//   7. price_band      |price - reference| > price_band_bps of reference (0 disables)
//      no_reference    band enabled, require_reference set, reference <= 0 (same step as 7)
//   8. rate_limit      max_orders_per_second orders already admitted in the last second
//   9. open_orders     working orders >= max_open_orders                (0 disables)
//
// check() is free of side effects except for one thing: an order that passes every step is
// recorded in the rate window, and only then. A rejected order, whatever the reason, never
// consumes rate budget. The rate step is evaluated at its position in the list but committed
// after step 9 for exactly that reason.
//
// Exposure and PnL are integer only. Products are formed in 128 bits and clamped back to
// +/-INT64_MAX on storage, so no input the API can express reaches signed overflow.
//
// Protocol expected from the order manager:
//   * on_order_open once per order sent, with its full quantity.
//   * on_fill for every execution; it changes the position only.
//   * on_order_closed to release working quantity whenever it stops being open: for the
//     filled part of every execution, for cancelled quantity and for rejected orders.
//     Releasing more than is open is clamped, so a duplicate report can never drive the
//     bookkeeping negative.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "optitrade/core/types.hpp"
#include "optitrade/risk/limits.hpp"
#include "optitrade/risk/reject.hpp"

namespace optitrade::risk {

// Signed quantity, average entry price and realized PnL of one instrument.
// realized is in price units (price * qty, i.e. 1e-4 currency).
struct Position {
    std::int64_t qty{};  // > 0 long, < 0 short
    Price avg_price{};   // 0 while flat
    std::int64_t realized{};
};

namespace detail {

__extension__ typedef __int128 Int128;

inline constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();

// Saturating narrowing. The lower bound is -INT64_MAX rather than INT64_MIN so that negation
// and magnitude of any stored value are always defined.
constexpr std::int64_t clamp64(Int128 v) noexcept {
    if (v > kInt64Max) return kInt64Max;
    if (v < -Int128{kInt64Max}) return -kInt64Max;
    return static_cast<std::int64_t>(v);
}

constexpr Int128 magnitude(Int128 v) noexcept { return v < 0 ? -v : v; }

constexpr bool valid(Side s) noexcept { return s == Side::buy || s == Side::sell; }

struct Slot {
    Position pos{};
    Price mark{};                   // last valid mark, 0 = never marked
    std::int64_t unrealized{};      // cached (mark - avg) * qty at `mark`
    std::int64_t exposure{};        // cached worst-case |position| including open orders
    std::int64_t open[2]{};         // working quantity per side, indexed by index(Side)
    std::uint32_t open_count[2]{};  // working orders per side
};

}  // namespace detail

class RiskEngine {
public:
    // Locate is a 16-bit field, so a larger table could never be addressed.
    static constexpr std::size_t kMaxLocates = std::size_t{1} << 16;
    static constexpr Nanos kRateWindowNs = 1'000'000'000;

    // Allocates the per-instrument table and the rate ring; nothing allocates afterwards.
    RiskEngine(const Limits& limits, std::size_t max_locates)
        : limits_(limits),
          slots_(std::min(max_locates, kMaxLocates)),
          rate_ring_(limits.max_orders_per_second) {}

    // Pre-trade check, see the order at the top of this file. `reference` <= 0 means unknown.
    // `now` should be non-decreasing; a step backwards counts as no time having passed.
    Reject check(Locate locate, Side side, Price price, Qty qty, Price reference,
                 Nanos now) noexcept {
        using detail::Int128;
        using detail::magnitude;

        if (kill_switch_) return Reject::kill_switch;
        if (locate >= slots_.size() || !detail::valid(side) || qty == 0 || price <= 0) {
            return Reject::invalid_order;
        }
        if (qty > limits_.max_order_qty) return Reject::order_qty;
        if (limits_.max_order_notional > 0 &&
            Int128{price} * qty > limits_.max_order_notional) {
            return Reject::order_notional;
        }

        const detail::Slot& s = slots_[locate];
        const Int128 same_leg = magnitude(leg(s, side, qty));
        if (same_leg > limits_.max_position) return Reject::position;

        if (limits_.max_gross_position > 0) {
            const Int128 other_leg = magnitude(leg(s, opposite(side), 0));
            const Int128 gross_after = gross_ - s.exposure + std::max(same_leg, other_leg);
            if (gross_after > limits_.max_gross_position) return Reject::gross_position;
        }

        if (limits_.price_band_bps != 0) {
            if (reference <= 0) {
                if (limits_.require_reference) return Reject::no_reference;
            } else if (magnitude(Int128{price} - reference) * kBasisPoints >
                       Int128{reference} * limits_.price_band_bps) {
                return Reject::price_band;
            }
        }

        if (!rate_has_room(now)) return Reject::rate_limit;
        if (limits_.max_open_orders != 0 && open_orders_ >= limits_.max_open_orders) {
            return Reject::open_orders;
        }

        rate_record(now);
        return Reject::none;
    }

    // One order of `qty` shares now works on the exchange. Ignored for qty 0 (a zero-size
    // order carries no exposure and would break the count invariant below).
    void on_order_open(Locate locate, Side side, Qty qty) noexcept {
        detail::Slot* s = slot_at(locate);
        if (s == nullptr || !detail::valid(side) || qty == 0) return;
        const std::size_t i = index(side);
        s->open[i] = detail::clamp64(detail::Int128{s->open[i]} + qty);
        if (s->open_count[i] != std::numeric_limits<std::uint32_t>::max()) {
            ++s->open_count[i];
            ++open_orders_;
        }
        refresh(*s);
    }

    // Releases `remaining` shares of working quantity, clamped to what is open, so a
    // duplicate or oversized release can never make the bookkeeping negative.
    //
    // Orders are anonymous here, so the count is derived from the quantity: it can never
    // exceed the open share count (every working order holds at least one share) and it
    // drops to zero the moment a side has nothing working. With one working order per
    // (instrument, side), which is what a quoting strategy produces, the count is exact.
    // With several, it may stay above the true count until the side is flat, which errs
    // toward rejecting rather than toward exceeding max_open_orders.
    void on_order_closed(Locate locate, Side side, Qty remaining) noexcept {
        detail::Slot* s = slot_at(locate);
        if (s == nullptr || !detail::valid(side)) return;
        const std::size_t i = index(side);
        s->open[i] -= std::min<std::int64_t>(remaining, s->open[i]);
        const std::uint32_t cap = s->open[i] >= std::numeric_limits<std::uint32_t>::max()
                                      ? std::numeric_limits<std::uint32_t>::max()
                                      : static_cast<std::uint32_t>(s->open[i]);
        const std::uint32_t kept = std::min(s->open_count[i], cap);
        open_orders_ -= s->open_count[i] - kept;
        s->open_count[i] = kept;
        refresh(*s);
    }

    // Average-cost accounting. Extending a position (or opening one) replaces the average with
    // the volume weighted average, integer division truncated toward zero. Reducing realizes
    // (fill - avg) * closed for a long and (avg - fill) * closed for a short and leaves the
    // average alone. A fill through zero realizes the closed part and opens the rest at the
    // fill price. Invalid input (bad locate or side, zero qty, price <= 0) is ignored.
    void on_fill(Locate locate, Side side, Qty qty, Price price) noexcept {
        using detail::Int128;
        detail::Slot* s = slot_at(locate);
        if (s == nullptr || !detail::valid(side) || qty == 0 || price <= 0) return;

        Position& p = s->pos;
        const bool buy = side == Side::buy;
        const Int128 held = detail::magnitude(p.qty);
        const Int128 fill = qty;
        const Int128 after = Int128{p.qty} + (buy ? fill : -fill);
        Int128 gain = 0;
        Price avg = p.avg_price;

        if (p.qty == 0 || (p.qty > 0) == buy) {
            // held == 0 reduces this to avg = price, so opening needs no special case.
            avg = static_cast<Price>((Int128{avg} * held + Int128{price} * fill) / (held + fill));
        } else {
            const Int128 closed = std::min(held, fill);
            gain = (p.qty > 0 ? Int128{price} - avg : Int128{avg} - price) * closed;
            if (after == 0) {
                avg = 0;
            } else if ((after > 0) != (p.qty > 0)) {
                avg = price;  // crossed through zero: the remainder opens at the fill price
            }
        }

        const std::int64_t realized_before = p.realized;
        p.qty = detail::clamp64(after);
        p.avg_price = avg;
        p.realized = detail::clamp64(Int128{p.realized} + gain);
        realized_total_ += Int128{p.realized} - realized_before;
        refresh(*s);
        settle();
    }

    // Records the last price used to value the open position. Non-positive marks are ignored.
    void mark(Locate locate, Price mark_price) noexcept {
        detail::Slot* s = slot_at(locate);
        if (s == nullptr || mark_price <= 0) return;
        s->mark = mark_price;
        refresh(*s);
        settle();
    }

    // Out-of-range locates read as a flat, never-traded instrument.
    const Position& position(Locate locate) const noexcept {
        const detail::Slot* s = slot_at(locate);
        return s != nullptr ? s->pos : kFlat;
    }
    std::int64_t open_qty(Locate locate, Side side) const noexcept {
        const detail::Slot* s = slot_at(locate);
        return s != nullptr && detail::valid(side) ? s->open[index(side)] : 0;
    }

    std::int64_t realized_pnl() const noexcept { return detail::clamp64(realized_total_); }
    std::int64_t unrealized_pnl() const noexcept { return detail::clamp64(unrealized_total_); }
    std::int64_t total_pnl() const noexcept {
        return detail::clamp64(realized_total_ + unrealized_total_);
    }
    // Largest peak-to-trough fall of total PnL seen at fill/mark calls. The running peak
    // starts at 0, the PnL of an untouched book, so an opening loss already counts.
    std::int64_t max_drawdown() const noexcept { return detail::clamp64(max_drawdown_); }

    // Sum over instruments of max(|position + open buys|, |position - open sells|): the
    // largest absolute position each instrument could reach if every working order filled.
    std::int64_t gross_exposure() const noexcept { return detail::clamp64(gross_); }

    void set_kill_switch(bool on) noexcept { kill_switch_ = on; }
    bool kill_switch() const noexcept { return kill_switch_; }

    std::uint32_t open_orders() const noexcept {
        return static_cast<std::uint32_t>(
            std::min<std::uint64_t>(open_orders_, std::numeric_limits<std::uint32_t>::max()));
    }

private:
    static constexpr detail::Int128 kBasisPoints = 10'000;
    static constexpr Position kFlat{};

    detail::Slot* slot_at(Locate l) noexcept { return l < slots_.size() ? &slots_[l] : nullptr; }
    const detail::Slot* slot_at(Locate l) const noexcept {
        return l < slots_.size() ? &slots_[l] : nullptr;
    }

    // Signed position the instrument would reach if all working orders on `side` and an
    // extra `extra` shares on the same side filled. Opposite-side orders are deliberately
    // not netted: they may be cancelled while the same-side ones fill.
    static detail::Int128 leg(const detail::Slot& s, Side side, detail::Int128 extra) noexcept {
        using detail::Int128;
        const std::size_t i = index(side);
        return side == Side::buy ? Int128{s.pos.qty} + s.open[i] + extra
                                 : Int128{s.pos.qty} - s.open[i] - extra;
    }

    // Recomputes the cached unrealized PnL and exposure of one instrument and moves the
    // running totals by the difference, so the totals stay exact without a scan.
    void refresh(detail::Slot& s) noexcept {
        using detail::Int128;
        const std::int64_t u =
            s.mark > 0 && s.pos.qty != 0
                ? detail::clamp64((Int128{s.mark} - s.pos.avg_price) * s.pos.qty)
                : 0;
        unrealized_total_ += Int128{u} - s.unrealized;
        s.unrealized = u;

        const std::int64_t e = detail::clamp64(std::max(detail::magnitude(leg(s, Side::buy, 0)),
                                                        detail::magnitude(leg(s, Side::sell, 0))));
        gross_ += Int128{e} - s.exposure;
        s.exposure = e;
    }

    // Samples total PnL for the drawdown and trips the kill switch on the loss limit. The
    // switch stays tripped until set_kill_switch(false); if PnL is still below the limit
    // the next fill or mark trips it again.
    void settle() noexcept {
        const detail::Int128 total = realized_total_ + unrealized_total_;
        peak_pnl_ = std::max(peak_pnl_, total);
        max_drawdown_ = std::max(max_drawdown_, peak_pnl_ - total);
        if (limits_.max_loss > 0 && total <= -detail::Int128{limits_.max_loss}) {
            kill_switch_ = true;
        }
    }

    // The ring holds the timestamps of the last max_orders_per_second admitted orders in
    // non-decreasing order, so when it is full its oldest entry is the N-th most recent
    // admission. The window is the half-open interval (now - 1 s, now]: an admission stops
    // counting at exactly t + 1 s. There is room iff fewer than N admissions fall inside
    // it, i.e. iff the ring is not full or its oldest entry has expired. O(1) and pure.
    bool rate_has_room(Nanos now) const noexcept {
        if (rate_ring_.empty() || rate_filled_ < rate_ring_.size()) return true;
        const Nanos oldest = rate_ring_[rate_next_];
        return now >= oldest && now - oldest >= kRateWindowNs;
    }

    void rate_record(Nanos now) noexcept {
        if (rate_ring_.empty()) return;
        rate_latest_ = std::max(rate_latest_, now);  // keeps the ring ordered if time steps back
        rate_ring_[rate_next_] = rate_latest_;
        if (++rate_next_ == rate_ring_.size()) rate_next_ = 0;
        if (rate_filled_ < rate_ring_.size()) ++rate_filled_;
    }

    Limits limits_;
    std::vector<detail::Slot> slots_;
    std::vector<Nanos> rate_ring_;
    std::size_t rate_next_{0};    // slot the next admission overwrites (the oldest once full)
    std::size_t rate_filled_{0};  // valid entries, so timestamp 0 is a real timestamp
    Nanos rate_latest_{0};

    detail::Int128 realized_total_{0};
    detail::Int128 unrealized_total_{0};
    detail::Int128 gross_{0};
    detail::Int128 peak_pnl_{0};
    detail::Int128 max_drawdown_{0};
    std::uint64_t open_orders_{0};
    bool kill_switch_{false};
};

}  // namespace optitrade::risk

#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <optional>

#include "optitrade/book/market_books.hpp"
#include "optitrade/core/types.hpp"
#include "optitrade/oms/interfaces.hpp"
#include "optitrade/oms/types.hpp"
#include "optitrade/risk/risk_engine.hpp"

// Strategy interface and the small pieces every strategy shares.
//
// A strategy is a plain class driven by three callbacks. It never owns the market
// or the order state: books and risk are read through the Context, and every order
// action goes through Context::orders. All decision arithmetic is 64-bit integer,
// all per-instrument state lives in tables sized once in the constructor, and time
// is only ever the `now` handed in, so a replay of the same feed yields the same
// orders on every platform.
namespace optitrade::strategy {

struct Context {
    Nanos now;
    const book::MarketBooks& books;
    oms::OrderApi& orders;
    const risk::RiskEngine& risk;
};

template <class S>
concept Strategy = requires(S& s, Locate l, const Context& c, const oms::Fill& f,
                            const oms::OrderInfo& o) {
    { s.on_book_update(l, c) } -> std::same_as<void>;  // after every book-changing message for `l`
    { s.on_fill(f, c) } -> std::same_as<void>;
    { s.on_order_update(o, c) } -> std::same_as<void>;
};

namespace detail {

// Positions beyond this are clamped before any arithmetic. No real book holds more
// shares, and the clamp keeps `target - position` and friends clear of int64 overflow
// even if the risk engine reports a saturated value.
inline constexpr std::int64_t kPositionBound = std::int64_t{1} << 40;

inline std::int64_t position_of(const risk::RiskEngine& risk, Locate l) noexcept {
    return std::clamp(risk.position(l).qty, -kPositionBound, kPositionBound);
}

// Best prices and sizes of a tradable book.
struct Touch {
    Price bid{};
    Price ask{};
    Qty bid_qty{};
    Qty ask_qty{};
};

// A book is tradable when both sides exist, prices are positive and the touch is not
// crossed or locked. Anything else means the feed is mid-update or damaged, and
// strategies treat it as "no opinion" (takers) or "pull quotes" (makers).
inline std::optional<Touch> touch_of(const book::OrderBook* b) noexcept {
    if (b == nullptr) return std::nullopt;
    const auto bid = b->best(Side::buy);
    const auto ask = b->best(Side::sell);
    if (!bid || !ask) return std::nullopt;
    if (bid->price <= 0 || ask->price <= 0 || bid->price >= ask->price) return std::nullopt;
    if (bid->qty == 0 || ask->qty == 0) return std::nullopt;
    return Touch{bid->price, ask->price, bid->qty, ask->qty};
}

// Pacing shared by the two strategies that cross the spread with IOC orders.
//
// At most one order per instrument is outstanding: until the previous one reaches a
// terminal state the gate stays shut, so a slow exchange cannot make the strategy
// stack up duplicate orders. Independently, `cooldown` nanoseconds must pass between
// attempts. The clock starts at every attempt, accepted or not, so a persistent
// risk reject cannot turn into one reject per book update.
class OrderGate {
public:
    bool ready(Nanos now, Nanos cooldown, const oms::OrderApi& api) noexcept {
        if (inflight_ != 0) {
            const oms::OrderInfo* o = api.find(inflight_);
            if (o != nullptr && !oms::is_terminal(o->status)) return false;
            inflight_ = 0;  // terminal, or the update was missed: either way it is over
        }
        return !has_last_ || (now >= last_ && now - last_ >= cooldown);
    }

    void sent(Nanos now, const oms::SubmitResult& r) noexcept {
        last_ = now;
        has_last_ = true;
        if (r.status == oms::SubmitStatus::ok) inflight_ = r.id;
    }

    void on_update(const oms::OrderInfo& o) noexcept {
        if (inflight_ != 0 && o.id == inflight_ && oms::is_terminal(o.status)) inflight_ = 0;
    }

    oms::OrderId inflight() const noexcept { return inflight_; }

private:
    oms::OrderId inflight_{0};
    Nanos last_{0};
    bool has_last_{false};
};

}  // namespace detail
}  // namespace optitrade::strategy

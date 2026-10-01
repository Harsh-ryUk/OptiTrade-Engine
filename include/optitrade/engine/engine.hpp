#pragma once

// Trading engine: the glue between the market data feed, one strategy, pre-trade risk
// and the order manager. It owns no policy of its own; it wires the modules together and
// keeps the counters an operator needs.
//
// Data flow
//
//   on_itch(msg, now)                                     on_ouch(msg, now)
//        |                                                      |
//   itch::decode ---> MarketBooks::on(msg)                ouch::decode_outbound
//        |                 |                                    |
//        |          Applied::ok on an order-flow                v
//        |          message (A F E C X D U)?          OrderManager::on_accepted / _replaced /
//        |                 |                           _canceled / _executed / _rejected
//        |                 v                                    |
//        |        risk.mark(locate, mid)                        | RiskEngine::on_fill / on_order_closed
//        |                 |                                    v
//        |                 v                           OrderListener (this engine)
//        |   trading enabled? --> strategy.on_book_update       |--> strategy.on_fill
//        |                              |                       '--> strategy.on_order_update
//        |                              v
//        |                     Context::orders (counting facade)
//        |                              |
//        |                              v
//        |          OrderManager::submit/cancel/replace --> RiskEngine::check --> OrderGateway::send
//        v
//   DecodeStatus (skipped and malformed messages have no other effect than a counter)
//
// Threading: single threaded, one call at a time. The engine is neither copyable nor
// movable because the order manager holds references to it (it is the manager's listener,
// reference-price source and symbol source).
//
// Allocation: the constructor sizes everything (books, risk tables, order table, the
// strategy's own tables). The message path calls no allocating code of its own. The one
// exception belongs to MarketBooks, which builds a locate's book on first use; feeds
// announce instruments with Stock Directory messages before trading starts, so that
// happens at start-up. Virtual calls on the path are exactly those a strategy makes
// (OrderApi) plus the OrderManager's callbacks to the listener and the gateway.
//
// Semantics that the specification leaves open, and how they were resolved
//   * Which locate a message belongs to. Executions, cancels, deletes and replaces name only
//     an order reference; the books act on the stored order and ignore the header locate.
//     The engine does the same: the strategy is told about the instrument whose book actually
//     changed, so a corrupt header locate cannot make it trade the wrong instrument.
//   * "Book-changing" means an order-flow message (A, F, E, C, X, D, U) for which the books
//     returned Applied::ok. Stock Directory, System Event and Trade never wake the strategy.
//     A message the books refuse (unknown order, duplicate, invalid, capacity) is counted in
//     book_errors and does not wake it either.
//   * Reference price for risk: the mid of the best bid and ask while both exist and the book
//     is not locked or crossed (a damaged book has no meaningful mid); otherwise the last
//     execution price of the instrument; otherwise 0 (unknown). The same mid marks the
//     position in the risk engine after every applied order-flow message.
//   * Gap handling. on_feed_gap() cancels every open order, halts the strategy's book callbacks
//     and turns the strategy's submit/replace into a refusal (SubmitStatus::bad_state). Cancels
//     stay possible. Order reports and fills keep flowing to the strategy, because its view of
//     its own orders must stay correct while it is halted; the books keep applying messages
//     so the caller can rebuild them by replaying a snapshot, then call resume_trading().
//     A gateway may refuse the cancels (its queue is full). While halted with open orders the
//     engine therefore repeats the cancel sweep from on_itch() and on_ouch(), at most once per
//     kGapRetryNs of message time; orders whose cancel is already in flight are skipped by the
//     sweep. What the halt guarantees: after on_feed_gap() the engine sends no new order and no
//     replace, and every order it had open gets a cancel as soon as the gateway takes one. What
//     it cannot guarantee: an order that is live at the exchange can still fill until its
//     cancel arrives there, and those fills reach the strategy. Call resume_trading() only once
//     open_orders() is 0 or the cancels are known to be in flight; an order whose cancel was
//     never accepted stays working after a resume.
//   * Stats. itch_messages / ouch_reports count messages that decoded and were delivered.
//     itch_skipped counts messages that did not decode (unsupported type or malformed), like
//     StreamResult::skipped; ouch_errors is the same for OUCH. book_errors counts decoded
//     messages the books refused. orders_sent counts new orders the gateway accepted;
//     risk_rejects counts submits and replaces refused by pre-trade risk. Both count what the
//     strategy does through its OrderApi; calls made directly on orders() are not counted.
//     capacity_rejects counts strategy submits, replaces and cancels the order manager or the
//     gateway refused for lack of room (SubmitStatus::capacity, SubmitStatus::gateway_busy); a
//     non-zero value means the run was cut short by table sizes, not by the strategy.
//     fills counts executions applied to an order.
//   * Locates at or above Config::max_locates still feed the books but never reach the
//     strategy (risk and the strategy size their tables by max_locates).

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

#include "optitrade/book/market_books.hpp"
#include "optitrade/core/decode_status.hpp"
#include "optitrade/core/types.hpp"
#include "optitrade/itch/decoder.hpp"
#include "optitrade/oms/interfaces.hpp"
#include "optitrade/oms/order_manager.hpp"
#include "optitrade/ouch/codec.hpp"
#include "optitrade/risk/limits.hpp"
#include "optitrade/risk/risk_engine.hpp"
#include "optitrade/strategy/strategy.hpp"

namespace optitrade::engine {

struct Config {
    book::MarketBooks::Config books{};
    risk::Limits limits{};
    oms::OrderManager::Config oms{};
    std::size_t max_locates{1024};  // instruments the risk engine and strategy can trade
};

struct Stats {
    std::uint64_t itch_messages{};  // ITCH messages decoded and applied to the books
    std::uint64_t itch_skipped{};   // ITCH messages that failed to decode (unsupported or malformed)
    std::uint64_t book_updates{};   // order-flow messages the books applied (Applied::ok)
    std::uint64_t book_errors{};    // decoded messages the books refused
    std::uint64_t ouch_reports{};   // OUCH reports decoded and passed to the order manager
    std::uint64_t ouch_errors{};    // OUCH messages that failed to decode
    std::uint64_t feed_gaps{};      // on_feed_gap() calls
    std::uint64_t orders_sent{};    // new orders accepted by the gateway
    std::uint64_t risk_rejects{};   // strategy submits/replaces refused by pre-trade risk
    std::uint64_t fills{};          // executions applied to orders
    std::uint64_t capacity_rejects{};  // strategy requests refused: order table or gateway full
};

template <strategy::Strategy S>
class Engine final : private oms::OrderListener,
                     private oms::ReferenceSource,
                     private oms::SymbolSource {
    static_assert(strategy::Strategy<S>);

public:
    Engine(const Config& config, oms::OrderGateway& gateway, S strategy = S{})
        : max_locates_(config.max_locates),
          books_(config.books),
          risk_(config.limits, config.max_locates),
          orders_(config.oms, gateway, risk_, static_cast<const oms::ReferenceSource&>(*this),
                  static_cast<const oms::SymbolSource&>(*this), static_cast<oms::OrderListener*>(this)),
          api_(*this),
          strategy_(std::move(strategy)) {}

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // One unframed ITCH message. Unsupported or malformed input changes nothing but a counter.
    DecodeStatus on_itch(std::span<const std::byte> msg, Nanos now) noexcept {
        ItchSink sink{*this, now};
        const DecodeStatus st = itch::decode(msg, sink);
        if (st != DecodeStatus::ok) ++stats_.itch_skipped;
        retry_gap_cancels(now);
        return st;
    }

    // One unframed OUCH outbound message (exchange -> us).
    DecodeStatus on_ouch(std::span<const std::byte> msg, Nanos now) noexcept {
        OuchSink sink{*this, now};
        const DecodeStatus st = ouch::decode_outbound(msg, sink);
        if (st != DecodeStatus::ok) ++stats_.ouch_errors;
        retry_gap_cancels(now);
        return st;
    }

    // Market data was lost. Working orders were priced off a book that is no longer
    // trustworthy, so pull them all and stop the strategy until the caller has rebuilt the
    // books and calls resume_trading().
    void on_feed_gap(Nanos now) noexcept {
        ++stats_.feed_gaps;
        trading_ = false;
        next_gap_retry_ = saturating_add(now, kGapRetryNs);
        orders_.cancel_all(now);
    }

    void resume_trading() noexcept { trading_ = true; }
    bool trading_enabled() const noexcept { return trading_; }

    const Stats& stats() const noexcept { return stats_; }
    S& strategy() noexcept { return strategy_; }
    const book::MarketBooks& books() const noexcept { return books_; }
    risk::RiskEngine& risk() noexcept { return risk_; }
    oms::OrderManager& orders() noexcept { return orders_; }

private:
    // Message time between cancel sweeps while halted. A sweep walks the working orders, so the
    // interval keeps a refusing gateway from turning every message into a walk.
    static constexpr Nanos kGapRetryNs = 100'000;

    static Nanos saturating_add(Nanos a, Nanos b) noexcept { return a > ~Nanos{0} - b ? ~Nanos{0} : a + b; }

    // One predictable branch when trading. Halted, the sweep is skipped unless orders are open
    // and the interval has passed; the order manager skips orders whose cancel is in flight.
    void retry_gap_cancels(Nanos now) noexcept {
        if (trading_ || now < next_gap_retry_ || orders_.open_orders() == 0) return;
        next_gap_retry_ = saturating_add(now, kGapRetryNs);
        orders_.cancel_all(now);
    }

    // What the strategy sees as its OrderApi. It forwards to the order manager (a final class,
    // so the forwarding calls are direct), counts outcomes, and refuses new exposure while the
    // engine is halted.
    class Api final : public oms::OrderApi {
    public:
        explicit Api(Engine& e) noexcept : e_(e) {}

        oms::SubmitResult submit(const oms::OrderRequest& req, Nanos now) override {
            if (!e_.trading_) return {oms::SubmitStatus::bad_state, 0, risk::Reject::none};
            const oms::SubmitResult r = e_.orders_.submit(req, now);
            if (r.status == oms::SubmitStatus::ok) ++e_.stats_.orders_sent;
            if (r.status == oms::SubmitStatus::rejected_by_risk) ++e_.stats_.risk_rejects;
            e_.count_refusal(r.status);
            return r;
        }
        oms::SubmitStatus cancel(oms::OrderId id, Nanos now) override {
            const oms::SubmitStatus s = e_.orders_.cancel(id, now);
            e_.count_refusal(s);
            return s;
        }
        oms::SubmitStatus replace(oms::OrderId id, Price px, Qty qty, Nanos now) override {
            if (!e_.trading_) return oms::SubmitStatus::bad_state;
            const oms::SubmitStatus s = e_.orders_.replace(id, px, qty, now);
            if (s == oms::SubmitStatus::rejected_by_risk) ++e_.stats_.risk_rejects;
            e_.count_refusal(s);
            return s;
        }
        const oms::OrderInfo* find(oms::OrderId id) const override { return e_.orders_.find(id); }
        std::size_t open_orders() const override { return e_.orders_.open_orders(); }

    private:
        Engine& e_;
    };

    // Decoder handlers. `decode` calls on() only for a fully validated message.
    struct ItchSink {
        Engine& e;
        Nanos now;
        template <class M>
        void on(const M& m) noexcept { e.apply(m, now); }
    };
    struct OuchSink {
        Engine& e;
        Nanos now;
        template <class M>
        void on(const M& m) noexcept { e.report(m, now); }
    };

    template <class M>
    static constexpr bool kOrderFlow =
        std::is_same_v<M, itch::AddOrder> || std::is_same_v<M, itch::OrderExecuted> ||
        std::is_same_v<M, itch::OrderExecutedPrice> || std::is_same_v<M, itch::OrderCancel> ||
        std::is_same_v<M, itch::OrderDelete> || std::is_same_v<M, itch::OrderReplace>;

    // The instrument a message will change, read before it is applied (an execution that
    // empties an order removes the record the locate comes from).
    Locate target_locate(const itch::AddOrder& m) const noexcept { return m.h.locate; }
    Locate target_locate(const itch::OrderReplace& m) const noexcept { return stored_locate(m.old_ref, m.h.locate); }
    template <class M>
    Locate target_locate(const M& m) const noexcept { return stored_locate(m.ref, m.h.locate); }

    Locate stored_locate(OrderRef ref, Locate header) const noexcept {
        const book::MarketBooks::OrderInfo* o = books_.order(ref);
        return o != nullptr ? o->locate : header;  // unknown ref: the books refuse it anyway
    }

    template <class M>
    void apply(const M& m, Nanos now) noexcept {
        ++stats_.itch_messages;
        if constexpr (kOrderFlow<M>) {
            const Locate loc = target_locate(m);
            const book::Applied r = books_.on(m);
            if (r == book::Applied::ok) {
                ++stats_.book_updates;
                book_changed(loc, now);
            } else if (r != book::Applied::ignored) {
                ++stats_.book_errors;
            }
        } else {
            const book::Applied r = books_.on(m);
            if (r != book::Applied::ok && r != book::Applied::ignored) ++stats_.book_errors;
        }
    }

    void book_changed(Locate loc, Nanos now) noexcept {
        if (const Price mid = mid_price(loc); mid > 0) risk_.mark(loc, mid);
        if (trading_ && loc < max_locates_) strategy_.on_book_update(loc, context(now));
    }

    // Mid of a sane touch, else 0. The half-spread form cannot overflow.
    Price mid_price(Locate loc) const noexcept {
        const book::OrderBook* b = books_.book(loc);
        if (b == nullptr) return 0;
        const auto bid = b->best(Side::buy);
        const auto ask = b->best(Side::sell);
        if (!bid || !ask || bid->price <= 0 || bid->price >= ask->price) return 0;
        return bid->price + (ask->price - bid->price) / 2;
    }

    void report(const ouch::Accepted& m, Nanos now) noexcept { ++stats_.ouch_reports; orders_.on_accepted(m, now); }
    void report(const ouch::Replaced& m, Nanos now) noexcept { ++stats_.ouch_reports; orders_.on_replaced(m, now); }
    void report(const ouch::Canceled& m, Nanos now) noexcept { ++stats_.ouch_reports; orders_.on_canceled(m, now); }
    void report(const ouch::Executed& m, Nanos now) noexcept { ++stats_.ouch_reports; orders_.on_executed(m, now); }
    void report(const ouch::Rejected& m, Nanos now) noexcept { ++stats_.ouch_reports; orders_.on_rejected(m, now); }

    void count_refusal(oms::SubmitStatus s) noexcept {
        if (s == oms::SubmitStatus::capacity || s == oms::SubmitStatus::gateway_busy) ++stats_.capacity_rejects;
    }

    strategy::Context context(Nanos now) noexcept { return strategy::Context{now, books_, api_, risk_}; }

    // OrderListener. The order manager stamps every event with the `now` of the call that
    // caused it, so the strategy's clock is that same value.
    void on_fill(const oms::Fill& f) override {
        ++stats_.fills;
        strategy_.on_fill(f, context(f.ts));
    }
    void on_order_update(const oms::OrderInfo& o) override { strategy_.on_order_update(o, context(o.last_update)); }

    // ReferenceSource / SymbolSource
    Price reference_price(Locate loc) const override {
        if (const Price mid = mid_price(loc); mid > 0) return mid;
        return books_.last_trade(loc);
    }
    Symbol symbol(Locate loc) const override { return books_.symbol(loc); }

    std::size_t max_locates_;
    book::MarketBooks books_;
    risk::RiskEngine risk_;
    oms::OrderManager orders_;
    Api api_;
    S strategy_;
    bool trading_{true};
    Nanos next_gap_retry_{0};
    Stats stats_{};
};

}  // namespace optitrade::engine

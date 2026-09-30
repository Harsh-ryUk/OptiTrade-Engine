#pragma once

// Order manager: turns strategy intent into OUCH order-entry messages and turns the
// exchange's reports back into a consistent order book, position and exposure ledger.
//
// Lifecycle
//
//   submit ------> pending_new --Accepted--> live --Executed (leaves > 0)--> live
//                     |  |                   |  |
//                     |  +--Rejected-> rejected (terminal)
//                     |                      +--cancel()--> pending_cancel --Canceled--> canceled
//                     |                      +--replace()-> pending_replace --Replaced--> live
//                     |                                          +--Rejected (of the replace)--> live
//                     +-- Executed/Canceled/Accepted('D') can also finish an order: filled or canceled
//
// Terminal states (filled, canceled, rejected) are final: a report for a terminal order is
// counted as late and changes nothing, not even `last_update`.
//
// Identifiers and tokens
//   * OrderIds are 1, 2, 3 ... in submit order. They are never reused; the table holds every
//     order of the session, so `Config::max_orders` bounds the orders per session and `find()`
//     keeps answering for finished orders. (OUCH requires tokens to be unique for the day, so
//     a session-long table costs nothing that the protocol does not already impose.)
//   * Tokens are minted from a single counter starting at 1 (`Token::from_id`). A token is
//     consumed only by a message the gateway accepted: a refused send leaves no trace.
//   * A report is matched through token -> OrderId. The token must be exactly the canonical
//     14-digit form we minted, so "42" cannot alias "00000000000042". While a replace is in
//     flight the order is reachable under both its active and its replacement token. A
//     superseded token is forgotten at once; the last token of a finished order is kept so
//     a late report can be told from a stranger.
//
// Exposure ledger (risk.open_qty)
//   For every non-terminal order the risk engine holds `leaves + extra` shares, where `extra`
//   is quantity reserved for a size-increasing replace that the exchange has not yet answered.
//   Reserving at send time closes the window in which a second order could pass the pre-trade
//   check against exposure that the replace is about to add. Exposure only ever grows at
//   submit()/replace() (after risk.check) and only shrinks on reports, so an exchange report
//   can never push the ledger past a limit. A shrinking replace keeps the old exposure until
//   the exchange confirms, because the order can still fill at its old size until then.
//   Every share is released exactly once: fills and cancels release what they consume, the
//   terminal transition releases the rest, and a terminal order holds nothing.
//
// Tolerance for hostile or confused reports. The exchange is not trusted:
//   * Executed: applied to any non-terminal order that carries the token (a fill is a fact, so
//     one that arrives before Accepted is booked and counts as an implicit acknowledgment).
//     Quantity above the open quantity is clamped to it; zero quantity or a price outside
//     (0, 2^32) is dropped.
//   * Canceled: same rules; zero decrement is dropped.
//   * Accepted: at most once per order. Rejected: only for an order the exchange has not yet
//     been seen to hold, or for a replacement token.
//   * Reports on a replacement token other than Replaced/Rejected are out of order and dropped.
//   Dropped reports increment a counter in `stats()` and never touch state. The manager cannot
//   tell a repeated Executed from a second genuine one (match numbers are not required to be
//   unique in this model); repeats are bounded by the clamp to the open quantity.
//
// Threading and re-entrancy: single threaded. Listener callbacks run after all state has been
// updated, so a listener may call submit/cancel/replace/cancel_all from inside a callback.
// OrderGateway::send must not call back into the manager; deliver its reports later. Gateway
// and listener must not throw.
//
// Not modelled: short-sale marking (orders leave as plain buys and sells), price re-checks
// for a replace that changes only the price or shrinks the order (the pre-trade check runs
// for growth only, so shrinking stays possible while the kill switch is on), and time-outs
// for reports that never arrive. A growing replace goes through RiskEngine::check like a new
// order, so it also spends rate budget and is refused while max_open_orders is reached.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "optitrade/core/flat_hash_map.hpp"
#include "optitrade/core/types.hpp"
#include "optitrade/oms/interfaces.hpp"
#include "optitrade/oms/types.hpp"
#include "optitrade/ouch/messages.hpp"
#include "optitrade/risk/risk_engine.hpp"

namespace optitrade::oms {

class OrderManager final : public OrderApi {
public:
    struct Config {
        std::size_t max_orders{1u << 16};  // orders per session, terminal ones included
    };

    // Counters for reports that were not applied.
    struct Stats {
        std::uint64_t unknown_tokens{};   // token maps to no order
        std::uint64_t late_reports{};     // order already terminal
        std::uint64_t invalid_reports{};  // malformed, duplicate or illegal in the order's state
        std::uint64_t clamped_reports{};  // applied, but quantity cut to the open quantity
    };

    // Everything is allocated here; no member function allocates afterwards. `listener` may be null.
    OrderManager(const Config& config, OrderGateway& gateway, risk::RiskEngine& risk,
                 const ReferenceSource& reference, const SymbolSource& symbols,
                 OrderListener* listener)
        : gateway_(gateway),
          risk_(risk),
          reference_(reference),
          symbols_(symbols),
          listener_(listener),
          orders_(config.max_orders),
          tokens_(2 * config.max_orders) {}

    // ---- OrderApi -----------------------------------------------------------------------

    SubmitResult submit(const OrderRequest& req, Nanos now) noexcept override {
        if (!valid(req)) return {SubmitStatus::invalid_request, 0, risk::Reject::none};
        const Symbol symbol = symbols_.symbol(req.locate);
        if (symbol.empty()) return {SubmitStatus::invalid_request, 0, risk::Reject::none};
        // Capacity is tested before the risk check: check() spends rate budget when it passes.
        if (count_ >= orders_.size() || !token_available()) {
            return {SubmitStatus::capacity, 0, risk::Reject::none};
        }
        const risk::Reject verdict = risk_.check(req.locate, req.side, req.price, req.qty,
                                                 reference_.reference_price(req.locate), now);
        if (verdict != risk::Reject::none) return {SubmitStatus::rejected_by_risk, 0, verdict};

        ouch::EnterOrder msg;
        msg.token = ouch::Token::from_id(next_token_);
        msg.side = req.side;
        msg.shares = req.qty;
        msg.stock = symbol;
        msg.price = req.price;
        msg.time_in_force = wire_tif(req.tif);

        const OrderId id = count_ + 1;
        if (!tokens_.insert(next_token_, id).second) {
            return {SubmitStatus::capacity, 0, risk::Reject::none};
        }
        if (!gateway_.send(msg, now)) {
            tokens_.erase(next_token_);
            return {SubmitStatus::gateway_busy, 0, risk::Reject::none};
        }

        Entry& e = orders_[count_];
        e = Entry{};
        e.info.id = id;
        e.info.req = req;
        e.info.leaves_qty = req.qty;
        e.info.last_update = now;
        e.token = next_token_;
        ++count_;
        ++next_token_;
        ++open_;
        risk_.on_order_open(req.locate, req.side, req.qty);
        notify(e);
        return {SubmitStatus::ok, id, risk::Reject::none};
    }

    // Valid from every non-terminal state except pending_cancel. While a replace is in flight the
    // cancel is addressed to the replacement token: the exchange handles our messages in order,
    // so by the time it sees the cancel the replacement is the live order.
    SubmitStatus cancel(OrderId id, Nanos now) noexcept override {
        Entry* e = entry(id);
        if (e == nullptr) return SubmitStatus::unknown_order;
        if (is_terminal(e->info.status) || e->info.status == OrderStatus::pending_cancel) {
            return SubmitStatus::bad_state;
        }
        const std::uint64_t target = e->next_token != 0 ? e->next_token : e->token;
        if (!gateway_.send(ouch::CancelOrder{ouch::Token::from_id(target), 0}, now)) {
            return SubmitStatus::gateway_busy;
        }
        e->info.status = OrderStatus::pending_cancel;
        e->info.last_update = now;
        notify(*e);
        return SubmitStatus::ok;
    }

    // `new_qty` is the total intended size including executions, as in OUCH. Only a live order
    // can be replaced. The pre-trade check covers the extra shares when the order grows.
    SubmitStatus replace(OrderId id, Price new_price, Qty new_qty, Nanos now) noexcept override {
        Entry* e = entry(id);
        if (e == nullptr) return SubmitStatus::unknown_order;
        if (e->info.status != OrderStatus::live) return SubmitStatus::bad_state;
        const OrderInfo& info = e->info;
        if (new_price <= 0 || new_price > kMaxWirePrice || new_qty == 0 || new_qty > kMaxOrderQty ||
            new_qty <= info.cum_qty) {
            return SubmitStatus::invalid_request;
        }
        if (!token_available()) return SubmitStatus::capacity;

        const Qty open_after = new_qty - info.cum_qty;
        const Qty growth = open_after > info.leaves_qty ? open_after - info.leaves_qty : 0;
        if (growth != 0) {
            const Locate loc = info.req.locate;
            const risk::Reject verdict = risk_.check(loc, info.req.side, new_price, growth,
                                                     reference_.reference_price(loc), now);
            if (verdict != risk::Reject::none) return SubmitStatus::rejected_by_risk;
        }

        ouch::ReplaceOrder msg;
        msg.existing = ouch::Token::from_id(e->token);
        msg.replacement = ouch::Token::from_id(next_token_);
        msg.shares = new_qty;
        msg.price = new_price;
        msg.time_in_force = wire_tif(info.req.tif);
        if (!tokens_.insert(next_token_, id).second) return SubmitStatus::capacity;
        if (!gateway_.send(msg, now)) {
            tokens_.erase(next_token_);
            return SubmitStatus::gateway_busy;
        }

        e->next_token = next_token_++;
        e->replace_price = new_price;
        e->replace_qty = new_qty;
        e->extra = growth;
        if (growth != 0) {
            // Grow the ledger by re-opening the order at its larger size instead of adding a
            // second "order": the risk engine derives its open-order count from quantity, so
            // this keeps the count exact for the usual one order per instrument and side.
            risk_.on_order_closed(info.req.locate, info.req.side, info.leaves_qty);
            risk_.on_order_open(info.req.locate, info.req.side, info.leaves_qty + growth);
        }
        e->info.status = OrderStatus::pending_replace;
        e->info.last_update = now;
        notify(*e);
        return SubmitStatus::ok;
    }

    const OrderInfo* find(OrderId id) const override {
        const Entry* e = entry(id);
        return e != nullptr ? &e->info : nullptr;
    }

    std::size_t open_orders() const override { return open_; }

    // Sends a cancel for every order that is not terminal and has none in flight, in id order.
    // Returns how many the gateway accepted; the rest can be retried by calling again.
    // The scan is linear in the orders submitted so far, which is fine for a kill-switch path.
    std::size_t cancel_all(Nanos now) noexcept {
        const OrderId last = count_;  // orders a listener submits from a callback are not swept
        std::size_t sent = 0;
        for (OrderId id = 1; id <= last; ++id) {
            const OrderStatus s = orders_[id - 1].info.status;
            if (is_terminal(s) || s == OrderStatus::pending_cancel) continue;
            if (cancel(id, now) == SubmitStatus::ok) ++sent;
        }
        return sent;
    }

    // ---- exchange reports ---------------------------------------------------------------

    void on_accepted(const ouch::Accepted& a, Nanos now) noexcept {
        Entry* e = active_order(a.token);
        if (e == nullptr) return;
        if (e->accepted || (a.order_state != 'L' && a.order_state != 'D')) return invalid();
        e->accepted = true;
        e->acked = true;
        e->info.exchange_ref = a.ref;
        e->info.last_update = now;
        if (a.order_state == 'D') {
            end_of_life(*e);  // accepted and dead on arrival: nothing will follow
        } else if (e->info.status == OrderStatus::pending_new) {
            e->info.status = OrderStatus::live;
        }
        notify(*e);
    }

    void on_executed(const ouch::Executed& x, Nanos now) noexcept {
        Entry* e = active_order(x.token);
        if (e == nullptr) return;
        if (x.shares == 0 || x.price <= 0 || x.price > kMaxWirePrice) return invalid();
        OrderInfo& info = e->info;
        const Qty qty = std::min(x.shares, info.leaves_qty);
        if (qty != x.shares) ++stats_.clamped_reports;

        e->acked = true;
        e->notional += static_cast<std::uint64_t>(x.price) * qty;
        info.cum_qty += qty;
        info.leaves_qty -= qty;
        info.avg_price = static_cast<Price>(e->notional / info.cum_qty);  // exact, not a running mean
        info.last_update = now;
        risk_.on_fill(info.req.locate, info.req.side, qty, x.price);
        risk_.on_order_closed(info.req.locate, info.req.side, qty);
        if (info.leaves_qty == 0) {
            end_of_life(*e, OrderStatus::filled);  // even after a partial cancel: nothing is left to cancel
        } else if (info.status == OrderStatus::pending_new) {
            info.status = OrderStatus::live;
        }
        if (listener_ != nullptr) {
            listener_->on_fill(Fill{info.id, info.req.locate, info.req.side, qty, x.price, now, x.match});
        }
        notify(*e);
    }

    void on_canceled(const ouch::Canceled& c, Nanos now) noexcept {
        Entry* e = active_order(c.token);
        if (e == nullptr) return;
        if (c.decrement == 0) return invalid();
        OrderInfo& info = e->info;
        const Qty qty = std::min(c.decrement, info.leaves_qty);
        if (qty != c.decrement) ++stats_.clamped_reports;

        e->acked = true;
        info.leaves_qty -= qty;
        info.reason = c.reason;
        info.last_update = now;
        risk_.on_order_closed(info.req.locate, info.req.side, qty);
        if (info.leaves_qty == 0) {
            end_of_life(*e);
        } else if (info.status == OrderStatus::pending_new) {
            info.status = OrderStatus::live;  // a partial cancel leaves the order working
        }
        notify(*e);
    }

    void on_rejected(const ouch::Rejected& r, Nanos now) noexcept {
        const Match m = resolve(r.token);
        if (m.entry == nullptr) return;
        Entry& e = *m.entry;
        if (is_terminal(e.info.status)) return late();
        if (m.replacement) {
            // The replace failed and the original order is untouched. A cancel that was sent
            // to the replacement token died with it, so the order is simply live again.
            release(e, e.extra);
            e.extra = 0;
            drop_replacement(e);
            e.info.status = OrderStatus::live;
        } else {
            // An order the exchange is known to hold cannot be rejected afterwards.
            if (e.acked) return invalid();
            end_of_life(e, OrderStatus::rejected);
        }
        e.info.reason = r.reason;
        e.info.last_update = now;
        notify(e);
    }

    void on_replaced(const ouch::Replaced& rp, Nanos now) noexcept {
        const Match m = resolve(rp.a.token);
        if (m.entry == nullptr) return;
        Entry& e = *m.entry;
        if (is_terminal(e.info.status)) return late();
        const char state = rp.a.order_state;
        if (!m.replacement || rp.previous != ouch::Token::from_id(e.token) ||
            (state != 'L' && state != 'D')) {
            return invalid();
        }

        // Swap the active token; the old one is dead from here on.
        OrderInfo& info = e.info;
        const Qty total = e.replace_qty;
        tokens_.erase(e.token);
        e.token = e.next_token;
        e.next_token = 0;
        // Executions that were in flight can exceed a shrunken total; the request then reads as
        // fully executed rather than as less than what traded.
        info.req.qty = std::max(total, info.cum_qty);
        info.req.price = rp.a.price > 0 && rp.a.price <= kMaxWirePrice ? rp.a.price : e.replace_price;
        if (rp.a.ref != 0) info.exchange_ref = rp.a.ref;  // a replaced order gets a fresh reference
        e.replace_qty = 0;
        e.replace_price = 0;

        // The exchange reports what is left open. Never trust it above what we asked for
        // (the new total less what has executed) or above what the ledger holds, so an
        // acknowledgment can only release exposure, never add to it.
        const Qty held = info.leaves_qty + e.extra;
        const Qty asked = total > info.cum_qty ? total - info.cum_qty : 0;
        const Qty leaves = state == 'D' ? 0 : std::min({rp.a.shares, asked, held});
        release(e, held - leaves);
        e.extra = 0;
        info.leaves_qty = leaves;
        info.last_update = now;
        if (leaves == 0) {
            end_of_life(e);
        } else if (info.status == OrderStatus::pending_replace) {
            info.status = OrderStatus::live;  // pending_cancel stays: its cancel is still in flight
        }
        notify(e);
    }

    // ---- inspection ---------------------------------------------------------------------

    // Shares the risk engine currently holds for this order: leaves plus any reservation for a
    // replace in flight. 0 for a terminal or unknown order.
    Qty reserved_qty(OrderId id) const noexcept {
        const Entry* e = entry(id);
        return e != nullptr ? e->info.leaves_qty + e->extra : 0;
    }

    const Stats& stats() const noexcept { return stats_; }
    std::size_t orders_submitted() const noexcept { return count_; }

private:
    static constexpr Price kMaxWirePrice = 0xFFFFFFFFLL;  // OUCH price is a u32

    struct Entry {
        OrderInfo info{};
        std::uint64_t token{};       // id of the token the exchange knows the order by
        std::uint64_t next_token{};  // replacement token while a replace is in flight, else 0
        Price replace_price{};
        Qty replace_qty{};           // total intended size requested by the replace in flight
        Qty extra{};                 // shares reserved beyond leaves for that replace
        std::uint64_t notional{};    // sum(price * qty) over executions
        bool accepted{};             // an Accepted report has been applied
        bool acked{};                // the exchange has shown it holds the order (any applied report)
    };

    struct Match {
        Entry* entry{nullptr};
        bool replacement{false};  // the token was the replacement token, not the active one
    };

    static bool valid(const OrderRequest& r) noexcept {
        return (r.side == Side::buy || r.side == Side::sell) &&
               (r.tif == Tif::day || r.tif == Tif::ioc) && r.qty >= 1 && r.qty <= kMaxOrderQty &&
               r.price > 0 && r.price <= kMaxWirePrice;
    }

    static std::uint32_t wire_tif(Tif t) noexcept {
        return t == Tif::ioc ? ouch::kTifIoc : ouch::kTifSystemHours;
    }

    bool token_available() const noexcept {
        return next_token_ <= ouch::Token::kMaxId && tokens_.size() < tokens_.max_size();
    }

    Entry* entry(OrderId id) noexcept {
        return id >= 1 && id <= count_ ? &orders_[id - 1] : nullptr;
    }
    const Entry* entry(OrderId id) const noexcept {
        return id >= 1 && id <= count_ ? &orders_[id - 1] : nullptr;
    }

    // Report token -> order. Unknown, non-canonical and stale tokens count as unknown.
    Match resolve(const ouch::Token& t) noexcept {
        const std::optional<std::uint64_t> id = t.to_id();
        if (id && ouch::Token::from_id(*id) == t) {
            const OrderId* owner = tokens_.find(*id);
            if (owner != nullptr) {
                if (Entry* e = entry(*owner)) {
                    if (e->token == *id) return {e, false};
                    if (e->next_token == *id) return {e, true};
                }
            }
        }
        ++stats_.unknown_tokens;
        return {};
    }

    // The order a report addresses by its active token; null (and counted) when the token is
    // unknown, the order is finished, or the token is a replacement that is not yet confirmed.
    Entry* active_order(const ouch::Token& t) noexcept {
        const Match m = resolve(t);
        if (m.entry == nullptr) return nullptr;
        if (is_terminal(m.entry->info.status)) {
            late();
            return nullptr;
        }
        if (m.replacement) {
            invalid();
            return nullptr;
        }
        return m.entry;
    }

    void late() noexcept { ++stats_.late_reports; }
    void invalid() noexcept { ++stats_.invalid_reports; }

    void release(const Entry& e, Qty qty) noexcept {
        if (qty != 0) risk_.on_order_closed(e.info.req.locate, e.info.req.side, qty);
    }

    void drop_replacement(Entry& e) noexcept {
        if (e.next_token != 0) tokens_.erase(e.next_token);
        e.next_token = 0;
        e.replace_price = 0;
        e.replace_qty = 0;
    }

    // Terminal transition. Whatever the ledger still holds is released here, exactly once.
    // `outcome` defaults to filled/canceled depending on whether the whole request executed.
    void end_of_life(Entry& e, std::optional<OrderStatus> outcome = std::nullopt) noexcept {
        OrderInfo& info = e.info;
        release(e, info.leaves_qty + e.extra);
        info.leaves_qty = 0;
        e.extra = 0;
        drop_replacement(e);
        info.status = outcome ? *outcome
                              : (info.cum_qty >= info.req.qty ? OrderStatus::filled : OrderStatus::canceled);
        --open_;
    }

    // The listener sees a copy taken now, so a callback that changes the order cannot make an
    // earlier notification describe a later state.
    void notify(const Entry& e) noexcept {
        if (listener_ == nullptr) return;
        const OrderInfo snapshot = e.info;
        listener_->on_order_update(snapshot);
    }

    OrderGateway& gateway_;
    risk::RiskEngine& risk_;
    const ReferenceSource& reference_;
    const SymbolSource& symbols_;
    OrderListener* listener_;
    std::vector<Entry> orders_;
    // Holds each order's active token, plus the replacement token while a replace is in flight.
    FlatHashMap<std::uint64_t, OrderId> tokens_;
    std::size_t count_{0};       // orders submitted; orders_[0 .. count_) are in use
    std::size_t open_{0};        // non-terminal orders
    std::uint64_t next_token_{1};
    Stats stats_{};
};

}  // namespace optitrade::oms

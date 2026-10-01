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
// Identifiers, tokens and the order table
//   * OrderIds are 1, 2, 3 ... in submit order and are never reused, not even when a finished
//     order's table slot is. The table holds `Config::max_orders` slots; a slot is reclaimed
//     from the oldest finished order only when a new order needs one. So the limit is on
//     orders working at the same moment, not on orders per session, and `submit` answers
//     `capacity` only when every slot holds a non-terminal order.
//   * Retention: a finished order stays queryable through find() until its slot is reused,
//     which is first-finished-first-reused (at least the last `max_orders - working` finished
//     orders are kept). After that find() returns nullptr, and so do cancel()/replace()
//     (unknown_order). Its tokens are forgotten with it, so a late report for it is counted
//     in `unknown_tokens` and changes nothing; a token is never reissued, so it cannot alias
//     the order that took the slot.
//   * Tokens are minted from a single counter starting at `Config::first_token` (default 1;
//     set it to continue a session's numbering, OUCH needs tokens unique for the day) via
//     `Token::from_id`. A token is consumed only by a message the gateway accepted: a refused
//     send leaves no trace.
//   * A report is matched through token -> table slot. The token must be exactly the canonical
//     14-digit form we minted, so "42" cannot alias "00000000000042". While a replace is in
//     flight the order is reachable under both its active and its replacement token. A
//     superseded token is forgotten at once; the last token of a finished order is kept until
//     its slot is reused so a late report can be told from a stranger.
//
// Exposure ledger (risk.open_qty)
//   For every non-terminal order the risk engine holds exactly `reserved_qty(id)` shares, which is
//   its `leaves`, or while a replace is in flight the larger of `leaves` and the open quantity
//   the replace asks for (total - cum). That is the most the order can have open at the
//   exchange whichever way the race between the replace and a partial cancel or a fill goes:
//   reserving at send time also closes the window in which another order could pass the
//   pre-trade check against exposure the replace is about to add. A terminal order holds nothing.
//   The ledger grows at submit()/replace() (after the risk check) and when the exchange reports
//   more open quantity than was booked (a fact we cannot refuse), and shrinks on reports.
//   Every change goes through one function, so the ledger is the sum of the reserved
//   quantities by construction. The open-order COUNT is exact too: +1 at submit, -1 at the one
//   terminal transition, never touched by a replace.
//
// Tolerance for hostile or confused reports. The exchange is not trusted, but what it says
// happened, happened:
//   * Executed: a fact. The full executed quantity goes to the position (risk.on_fill), to
//     cum_qty and to the average price, even when it is more than the open quantity we
//     believed (a replace raced a partial cancel). The order's `req.qty` is then raised to
//     cum_qty, so cum_qty <= req.qty always holds, and only the exposure release is clamped to
//     what the ledger holds. Applied to any non-terminal order that carries the token (one
//     that arrives before Accepted is booked and counts as an implicit acknowledgment). A
//     zero quantity, a price outside (0, 2^32), or a total beyond kMaxOrderQty is dropped.
//   * A repeated Executed (same match number on the same order, among the last kMatchMemory
//     of that order) is dropped and counted in `invalid_reports`. Older repeats are beyond
//     the window and are not recognised: known limit, there is no per-order unbounded set.
//   * Canceled: decrement is clamped to the open quantity (releasing more than is open
//     cannot make sense); zero decrement is dropped.
//   * Replaced: the open quantity it reports is the source of truth (an exchange may reopen
//     more than we held after a partial cancel), capped at what we asked for (total - cum).
//   * Accepted: at most once per order. Rejected: only for an order the exchange has not yet
//     been seen to hold, or for a replacement token.
//   * Reports on a replacement token other than Replaced/Rejected are out of order and dropped.
//   Dropped reports increment a counter in `stats()` and never touch state.
//
// Risk wiring. submit() and replace() ask the risk engine for a verdict (pure), then send, and
// only a send the gateway accepted spends rate budget (RiskEngine::admit). A replace is
// judged on the NEW open quantity and NEW price by the size, notional and band limits, and
// on the extra exposure only by the position and gross limits (see RiskEngine).
//
// Threading and re-entrancy: single threaded. Listener callbacks run after all state has been
// updated, so a listener may call submit/cancel/replace/cancel_all from inside a callback.
// OrderGateway::send must not call back into the manager; deliver its reports later. Gateway
// and listener must not throw.
//
// Not modelled: short-sale marking (orders leave as plain buys and sells) and time-outs for
// reports that never arrive.

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
        std::size_t max_orders{1u << 16};   // table slots: orders working at once, plus retained finished ones
        std::uint64_t first_token{1};       // first token to mint; 0 is treated as 1
    };

    // How many recent execution match numbers of one order are remembered to spot repeats.
    static constexpr std::size_t kMatchMemory = 8;

    // Counters for reports that were not applied.
    struct Stats {
        std::uint64_t unknown_tokens{};   // token maps to no order (including recycled ones)
        std::uint64_t late_reports{};     // order already terminal
        std::uint64_t invalid_reports{};  // malformed, duplicate or illegal in the order's state
        std::uint64_t clamped_reports{};  // applied, but the report disagreed with the open quantity
                                          // (Canceled/Replaced: cut to it; Executed: booked in full)
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
          orders_(slot_count(config.max_orders)),
          by_id_(orders_.size()),
          tokens_(2 * orders_.size()),
          finished_(orders_.size()),
          next_token_(config.first_token != 0 ? config.first_token : 1) {}

    // ---- OrderApi -----------------------------------------------------------------------

    SubmitResult submit(const OrderRequest& req, Nanos now) noexcept override {
        if (!valid(req)) return {SubmitStatus::invalid_request, 0, risk::Reject::none};
        const Symbol symbol = symbols_.symbol(req.locate);
        if (symbol.empty()) return {SubmitStatus::invalid_request, 0, risk::Reject::none};
        if (!slot_available() || !token_available()) {
            return {SubmitStatus::capacity, 0, risk::Reject::none};
        }
        // The verdict is pure: rate budget is spent below, only once the gateway took the order.
        const risk::Reject verdict = risk_.evaluate(req.locate, req.side, req.price, req.qty,
                                                    reference_.reference_price(req.locate), now);
        if (verdict != risk::Reject::none) return {SubmitStatus::rejected_by_risk, 0, verdict};

        ouch::EnterOrder msg;
        msg.token = ouch::Token::from_id(next_token_);
        msg.side = req.side;
        msg.shares = req.qty;
        msg.stock = symbol;
        msg.price = req.price;
        msg.time_in_force = wire_tif(req.tif);

        const std::uint32_t slot = next_slot();
        if (!tokens_.insert(next_token_, slot).second) {
            return {SubmitStatus::capacity, 0, risk::Reject::none};
        }
        if (!gateway_.send(msg, now)) {
            tokens_.erase(next_token_);
            return {SubmitStatus::gateway_busy, 0, risk::Reject::none};
        }
        risk_.admit(now);

        take_slot(slot);
        const OrderId id = next_id_++;
        Entry& e = orders_[slot];
        e = Entry{};
        e.info.id = id;
        e.info.req = req;
        e.info.leaves_qty = req.qty;
        e.info.last_update = now;
        e.token = next_token_;
        e.reserved = req.qty;
        by_id_.insert(id, slot);  // cannot fail: the table is sized for one entry per slot
        link(slot);
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
        return e != nullptr ? cancel_entry(*e, now) : SubmitStatus::unknown_order;
    }

    // `new_qty` is the total intended size including executions, as in OUCH. Only a live order
    // can be replaced. The risk engine judges the NEW open quantity at the NEW price (so a
    // replace cannot be used to dodge the size, notional and band limits, growing or not) and
    // the extra exposure, if any, against the position limits.
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
        const Locate loc = info.req.locate;
        const risk::Reject verdict = risk_.evaluate_replace(loc, info.req.side, new_price, open_after,
                                                            growth, reference_.reference_price(loc), now);
        if (verdict != risk::Reject::none) return SubmitStatus::rejected_by_risk;

        ouch::ReplaceOrder msg;
        msg.existing = ouch::Token::from_id(e->token);
        msg.replacement = ouch::Token::from_id(next_token_);
        msg.shares = new_qty;
        msg.price = new_price;
        msg.time_in_force = wire_tif(info.req.tif);
        if (!tokens_.insert(next_token_, slot_of(*e)).second) return SubmitStatus::capacity;
        if (!gateway_.send(msg, now)) {
            tokens_.erase(next_token_);
            return SubmitStatus::gateway_busy;
        }
        if (growth != 0) risk_.admit(now);  // a replace that adds nothing spends no budget

        e->next_token = next_token_++;
        e->replace_price = new_price;
        e->replace_qty = new_qty;
        sync_ledger(*e);
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
    // Walks the working orders only, so its cost does not depend on how many orders finished.
    std::size_t cancel_all(Nanos now) noexcept {
        const OrderId last = next_id_ - 1;  // orders a listener submits from a callback are not swept
        std::size_t sent = 0;
        for (std::uint32_t s = head_; s != kNone;) {
            Entry& e = orders_[s];
            if (e.info.id > last) break;
            s = e.next;  // read first: the cancel calls the listener, which may append orders
            if (e.info.status != OrderStatus::pending_cancel && cancel_entry(e, now) == SubmitStatus::ok) {
                ++sent;
            }
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
        OrderInfo& info = e->info;
        // No order can execute past the largest order size in total, so a report that claims
        // it is garbage rather than a fact.
        if (x.shares == 0 || x.price <= 0 || x.price > kMaxWirePrice ||
            x.shares > kMaxOrderQty - info.cum_qty) {
            return invalid();
        }
        if (seen_match(*e, x.match)) return invalid();  // retransmit: the first copy was booked
        remember_match(*e, x.match);

        // The execution happened, so all of it is booked even when it exceeds what we believed
        // was open; only the part the ledger actually holds is released below.
        if (x.shares > info.leaves_qty) ++stats_.clamped_reports;
        e->acked = true;
        e->notional += static_cast<std::uint64_t>(x.price) * x.shares;
        info.cum_qty += x.shares;
        info.leaves_qty -= std::min(x.shares, info.leaves_qty);
        info.req.qty = std::max(info.req.qty, info.cum_qty);  // keep cum_qty <= req.qty
        info.avg_price = static_cast<Price>(e->notional / info.cum_qty);  // exact, not a running mean
        info.last_update = now;
        risk_.on_fill(info.req.locate, info.req.side, x.shares, x.price);
        if (info.leaves_qty == 0) {
            end_of_life(*e, OrderStatus::filled);  // even after a partial cancel: nothing is left to cancel
        } else {
            if (info.status == OrderStatus::pending_new) info.status = OrderStatus::live;
            sync_ledger(*e);
        }
        if (listener_ != nullptr) {
            // on_fill may act on the order, so the update that follows shows its freshest state.
            // The order may even be gone: if it just finished, a submit from the callback can
            // have been given its slot, so look it up again instead of trusting `e`.
            const Fill fill{info.id, info.req.locate, info.req.side, x.shares, x.price, now, x.match};
            const OrderInfo before = info;
            listener_->on_fill(fill);
            if (const Entry* now_e = entry(before.id)) {
                notify(*now_e);
            } else {
                listener_->on_order_update(before);
            }
        }
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
        if (info.leaves_qty == 0) {
            end_of_life(*e);
        } else {
            if (info.status == OrderStatus::pending_new) info.status = OrderStatus::live;  // a partial cancel leaves the order working
            sync_ledger(*e);
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
            drop_replacement(e);
            e.info.status = OrderStatus::live;
            sync_ledger(e);
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

        // The exchange reports what is open after the replace and that is the truth, also when
        // it is more than we held: a replace carries the TOTAL size, so after a partial cancel
        // the exchange reopens (total - executed). Never above what we asked for, though.
        const Qty asked = total > info.cum_qty ? total - info.cum_qty : 0;
        if (state != 'D' && rp.a.shares > asked) ++stats_.clamped_reports;
        info.leaves_qty = state == 'D' ? 0 : std::min(rp.a.shares, asked);
        info.last_update = now;
        if (info.leaves_qty == 0) {
            end_of_life(e);
        } else {
            if (info.status == OrderStatus::pending_replace) {
                info.status = OrderStatus::live;  // pending_cancel stays: its cancel is still in flight
            }
            sync_ledger(e);
        }
        notify(e);
    }

    // ---- inspection ---------------------------------------------------------------------

    // Shares the risk engine currently holds for this order (see "Exposure ledger"). 0 for a
    // terminal or unknown order.
    Qty reserved_qty(OrderId id) const noexcept {
        const Entry* e = entry(id);
        return e != nullptr ? e->reserved : 0;
    }

    const Stats& stats() const noexcept { return stats_; }
    std::size_t orders_submitted() const noexcept { return next_id_ - 1; }  // ids issued so far

private:
    static constexpr Price kMaxWirePrice = 0xFFFFFFFFLL;  // OUCH price is a u32
    static constexpr std::uint32_t kNone = 0xFFFFFFFFu;

    struct Entry {
        OrderInfo info{};
        std::uint64_t token{};       // id of the token the exchange knows the order by
        std::uint64_t next_token{};  // replacement token while a replace is in flight, else 0
        Price replace_price{};
        Qty replace_qty{};           // total intended size requested by the replace in flight
        Qty reserved{};              // shares the risk engine holds for this order
        std::uint64_t notional{};    // sum(price * qty) over executions
        std::uint64_t matches[kMatchMemory]{};  // ring of recent execution match numbers
        std::uint32_t match_count{};            // executions booked; the ring is indexed modulo kMatchMemory
        std::uint32_t prev{kNone};   // working orders, oldest first
        std::uint32_t next{kNone};
        bool accepted{};             // an Accepted report has been applied
        bool acked{};                // the exchange has shown it holds the order (any applied report)
    };

    struct Match {
        Entry* entry{nullptr};
        bool replacement{false};  // the token was the replacement token, not the active one
    };

    static std::size_t slot_count(std::size_t n) noexcept {
        return std::min<std::size_t>(n, kNone - 1);
    }

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

    // ---- table slots --------------------------------------------------------------------

    // A slot is free if it was never used or holds a finished order (the oldest one goes first).
    bool slot_available() const noexcept { return fresh_ < orders_.size() || finished_count_ != 0; }

    // The slot take_slot() will hand out; no side effects, so a send can still fail in between.
    std::uint32_t next_slot() const noexcept {
        return fresh_ < orders_.size() ? static_cast<std::uint32_t>(fresh_) : finished_[finished_head_];
    }

    // Claims the slot next_slot() named. A finished order in it is forgotten: its id and its
    // tokens stop resolving (ids and tokens are never reissued, so nothing can alias it).
    void take_slot(std::uint32_t slot) noexcept {
        if (fresh_ < orders_.size()) {
            ++fresh_;
            return;
        }
        finished_head_ = (finished_head_ + 1) % finished_.size();
        --finished_count_;
        const Entry& old = orders_[slot];
        by_id_.erase(old.info.id);
        tokens_.erase(old.token);  // a finished order holds no replacement token (see drop_replacement)
    }

    std::uint32_t slot_of(const Entry& e) const noexcept {
        return static_cast<std::uint32_t>(&e - orders_.data());
    }

    Entry* entry(OrderId id) noexcept {
        const std::uint32_t* s = by_id_.find(id);
        return s != nullptr ? &orders_[*s] : nullptr;
    }
    const Entry* entry(OrderId id) const noexcept {
        const std::uint32_t* s = by_id_.find(id);
        return s != nullptr ? &orders_[*s] : nullptr;
    }

    // Working-order list, appended in id order and unlinked at the terminal transition.
    void link(std::uint32_t slot) noexcept {
        Entry& e = orders_[slot];
        e.prev = tail_;
        e.next = kNone;
        (tail_ != kNone ? orders_[tail_].next : head_) = slot;
        tail_ = slot;
    }
    void unlink(Entry& e) noexcept {
        (e.prev != kNone ? orders_[e.prev].next : head_) = e.next;
        (e.next != kNone ? orders_[e.next].prev : tail_) = e.prev;
        e.prev = e.next = kNone;
    }

    // ---- reports ------------------------------------------------------------------------

    // Report token -> order. Unknown, non-canonical, stale and recycled tokens count as unknown.
    Match resolve(const ouch::Token& t) noexcept {
        const std::optional<std::uint64_t> id = t.to_id();
        if (id && ouch::Token::from_id(*id) == t) {
            const std::uint32_t* slot = tokens_.find(*id);
            if (slot != nullptr) {
                Entry& e = orders_[*slot];
                if (e.token == *id) return {&e, false};
                if (e.next_token == *id) return {&e, true};
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

    static bool seen_match(const Entry& e, std::uint64_t match) noexcept {
        const std::size_t n = std::min<std::size_t>(e.match_count, kMatchMemory);
        for (std::size_t i = 0; i < n; ++i) {
            if (e.matches[i] == match) return true;
        }
        return false;
    }
    static void remember_match(Entry& e, std::uint64_t match) noexcept {
        e.matches[e.match_count % kMatchMemory] = match;
        ++e.match_count;
    }

    SubmitStatus cancel_entry(Entry& e, Nanos now) noexcept {
        if (is_terminal(e.info.status) || e.info.status == OrderStatus::pending_cancel) {
            return SubmitStatus::bad_state;
        }
        const std::uint64_t target = e.next_token != 0 ? e.next_token : e.token;
        if (!gateway_.send(ouch::CancelOrder{ouch::Token::from_id(target), 0}, now)) {
            return SubmitStatus::gateway_busy;
        }
        e.info.status = OrderStatus::pending_cancel;
        e.info.last_update = now;
        notify(e);
        return SubmitStatus::ok;
    }

    // ---- ledger -------------------------------------------------------------------------

    // Brings the risk engine's quantity for this (non-terminal) order to what it should hold
    // now: leaves, or while a replace is in flight at least the open quantity it asks for.
    // The only place that moves a working order's quantity, so each share is booked once.
    void sync_ledger(Entry& e) noexcept {
        Qty want = e.info.leaves_qty;
        if (e.next_token != 0 && e.replace_qty > e.info.cum_qty) {
            want = std::max(want, e.replace_qty - e.info.cum_qty);
        }
        const Locate loc = e.info.req.locate;
        const Side side = e.info.req.side;
        if (want > e.reserved) {
            risk_.on_order_grow(loc, side, want - e.reserved);
        } else if (want < e.reserved) {
            risk_.on_order_reduce(loc, side, e.reserved - want);
        }
        e.reserved = want;
    }

    void drop_replacement(Entry& e) noexcept {
        if (e.next_token != 0) tokens_.erase(e.next_token);
        e.next_token = 0;
        e.replace_price = 0;
        e.replace_qty = 0;
    }

    // Terminal transition, reached exactly once per order: releases whatever the ledger still
    // holds, takes the order off the count and queues its slot for reuse.
    // `outcome` defaults to filled/canceled depending on whether the whole request executed.
    void end_of_life(Entry& e, std::optional<OrderStatus> outcome = std::nullopt) noexcept {
        OrderInfo& info = e.info;
        info.leaves_qty = 0;
        drop_replacement(e);
        info.status = outcome ? *outcome
                              : (info.cum_qty >= info.req.qty ? OrderStatus::filled : OrderStatus::canceled);
        risk_.on_order_end(info.req.locate, info.req.side, e.reserved);
        e.reserved = 0;
        unlink(e);
        finished_[(finished_head_ + finished_count_) % finished_.size()] = slot_of(e);
        ++finished_count_;
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
    FlatHashMap<OrderId, std::uint32_t> by_id_;  // id -> slot, for orders still in the table
    // Holds each tabled order's active token, plus the replacement token while a replace is in
    // flight. Value: the order's slot.
    FlatHashMap<std::uint64_t, std::uint32_t> tokens_;
    std::vector<std::uint32_t> finished_;  // ring of finished orders' slots, oldest first
    std::size_t finished_head_{0};
    std::size_t finished_count_{0};
    std::size_t fresh_{0};       // slots never used: orders_[fresh_ ..) are untouched
    std::uint32_t head_{kNone};  // working orders, oldest first
    std::uint32_t tail_{kNone};
    OrderId next_id_{1};
    std::size_t open_{0};        // non-terminal orders
    std::uint64_t next_token_{1};
    Stats stats_{};
};

}  // namespace optitrade::oms

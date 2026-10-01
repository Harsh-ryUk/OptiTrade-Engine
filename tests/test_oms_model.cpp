// Model-based test of the order manager and the risk ledger it drives.
//
// A naive exchange (a few dozen lines, written here and sharing no code with the module under
// test) receives what the manager sends through the gateway and answers with reports. The
// manager is then driven by a random strategy (submit, cancel, replace, cancel_all, kill
// switch, gateway outages, marks) while the exchange keeps working on its own (fills, partial
// cancels), so the reports the manager sees are always a little behind the exchange: that lag
// is what produces the races (a replace crossing a partial cancel, a fill crossing a cancel,
// a cancel crossing the fill that ends the order).
//
//   legal mode    reports arrive exactly once, in order. Nothing the manager is shown may be
//                 counted as malformed, and once everything has been delivered the manager's
//                 view has to agree with the exchange's own (orders, executed quantities,
//                 open quantities, the risk position, the risk exposure).
//   illegal mode  reports are dropped, repeated, reordered, replayed late, mangled, or made up.
//                 The manager may end up disagreeing with the exchange, but its own books
//                 must stay consistent.
//
// After every step, in both modes:
//   * risk open quantity per (instrument, side) == sum of the manager's reserved quantity over
//     its non-terminal orders, and never negative
//   * risk position == signed sum of the fills the listener was told about
//   * risk open-order count == number of non-terminal orders (exact, not an estimate)
//   * cum_qty <= req.qty <= kMaxOrderQty; leaves + cum <= req.qty; a live order has leaves > 0
//   * terminal orders hold nothing and never change again
//   * per order: listener fills add up to cum_qty, average price is the exact quotient, and no
//     execution (match number) is booked twice
//   * ids are consecutive, tokens increase, amends respect the size/notional/band limits
//
// The table is tiny (a handful of slots) so slots are recycled constantly, and a listener that
// acts from inside its callbacks is part of the mix. Seeds are fixed; the run takes a couple
// of seconds under ASan+UBSan.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "check.hpp"
#include "optitrade/core/rng.hpp"
#include "optitrade/core/types.hpp"
#include "optitrade/oms/order_manager.hpp"
#include "optitrade/ouch/messages.hpp"
#include "optitrade/risk/risk_engine.hpp"

using namespace optitrade;

namespace {

constexpr Price kPx = 1'000'000;
constexpr Locate kLocates = 4;  // 1..3 are traded; 0 and 3 have no reference price

// ---- the wire ---------------------------------------------------------------------------------

struct Msg {
    enum Kind { enter, cancel, replace } kind{enter};
    ouch::EnterOrder e{};
    ouch::CancelOrder c{};
    ouch::ReplaceOrder r{};
};

// Keeps messages in the order they were sent, across kinds.
struct Wire final : oms::OrderGateway {
    std::deque<Msg> inbox;
    bool open{true};
    std::vector<std::uint64_t> enter_tokens;

    bool send(const ouch::EnterOrder& m, Nanos) override {
        if (!open) return false;
        Msg x; x.kind = Msg::enter; x.e = m; inbox.push_back(x);
        enter_tokens.push_back(m.token.to_id().value_or(0));
        return true;
    }
    bool send(const ouch::CancelOrder& m, Nanos) override {
        if (!open) return false;
        Msg x; x.kind = Msg::cancel; x.c = m; inbox.push_back(x);
        return true;
    }
    bool send(const ouch::ReplaceOrder& m, Nanos) override {
        if (!open) return false;
        Msg x; x.kind = Msg::replace; x.r = m; inbox.push_back(x);
        return true;
    }
};

struct Reference final : oms::ReferenceSource {
    Price reference_price(Locate l) const override { return l == 1 || l == 2 ? kPx : 0; }
};
struct Symbols final : oms::SymbolSource {
    Symbol symbol(Locate l) const override {
        switch (l) {
            case 1: return Symbol("AAA");
            case 2: return Symbol("BBB");
            case 3: return Symbol("CCC");
            default: return Symbol{};
        }
    }
};

// Sees every callback; optionally acts from inside them.
struct Listener final : oms::OrderListener {
    std::vector<oms::Fill> fills;
    std::function<void()> hook;
    int depth{0};
    void on_fill(const oms::Fill& f) override { fills.push_back(f); run(); }
    void on_order_update(const oms::OrderInfo&) override { run(); }
    void run() {
        if (hook && depth < 2) { ++depth; hook(); --depth; }
    }
};

ouch::Token tok(std::uint64_t id) { return ouch::Token::from_id(id); }

// ---- the naive exchange --------------------------------------------------------------------------

struct Rep {
    enum Kind { acc, exe, can, rej, rpl } kind{acc};
    ouch::Accepted a{};
    ouch::Executed x{};
    ouch::Canceled c{};
    ouch::Rejected j{};
    ouch::Replaced r{};

    std::uint64_t token() const {
        switch (kind) {
            case acc: return a.token.to_id().value_or(0);
            case exe: return x.token.to_id().value_or(0);
            case can: return c.token.to_id().value_or(0);
            case rej: return j.token.to_id().value_or(0);
            case rpl: return r.a.token.to_id().value_or(0);
        }
        return 0;
    }
};

struct XOrd {
    Locate loc{};
    Side side{Side::buy};
    Price px{};
    Qty cum{}, leaves{};
    bool alive{false};
    bool ioc{false};
    std::uint64_t active{};  // the token it currently answers to
};

class Exchange {
public:
    explicit Exchange(Rng& rng) : rng_(rng) {}

    std::vector<XOrd> orders;                         // in arrival order
    std::unordered_map<std::uint64_t, std::size_t> by_token;
    std::deque<Rep> outbox;
    std::uint64_t match{0};

    static Locate locate_of(const Symbol& s) {
        return s == Symbol("AAA") ? 1 : s == Symbol("BBB") ? 2 : 3;
    }

    XOrd* at(std::uint64_t token) {
        const auto it = by_token.find(token);
        return it == by_token.end() ? nullptr : &orders[it->second];
    }

    void execute(XOrd& x, Qty q, Price px) {
        Rep r; r.kind = Rep::exe;
        r.x.token = tok(x.active); r.x.shares = q; r.x.price = px; r.x.match = ++match;
        outbox.push_back(r);
        x.leaves -= q; x.cum += q;
        if (x.leaves == 0) x.alive = false;
    }
    void cancel_shares(XOrd& x, Qty q, char reason) {
        Rep r; r.kind = Rep::can;
        r.c.token = tok(x.active); r.c.decrement = q; r.c.reason = reason;
        outbox.push_back(r);
        x.leaves -= q;
        if (x.leaves == 0) x.alive = false;
    }

    // Handles one message the manager sent.
    void process(const Msg& m) {
        switch (m.kind) {
            case Msg::enter: {
                const std::uint64_t t = m.e.token.to_id().value_or(0);
                XOrd x; x.loc = locate_of(m.e.stock); x.side = m.e.side; x.px = m.e.price; x.leaves = m.e.shares;
                x.ioc = m.e.time_in_force == ouch::kTifIoc; x.active = t;
                by_token[t] = orders.size();
                Rep r;
                if (rng_.chance(5, 100)) {
                    x.leaves = 0;
                    r.kind = Rep::rej; r.j.token = m.e.token; r.j.reason = 'X';
                    outbox.push_back(r);
                    orders.push_back(x);
                    return;
                }
                r.kind = Rep::acc; r.a.token = m.e.token; r.a.shares = m.e.shares; r.a.price = m.e.price;
                r.a.ref = 1000 + t; r.a.order_state = 'L';
                x.alive = true;
                if (rng_.chance(3, 100)) { r.a.order_state = 'D'; x.alive = false; x.leaves = 0; }
                outbox.push_back(r);
                orders.push_back(x);
                XOrd& y = orders.back();
                if (y.alive && y.ioc) {  // immediate-or-cancel: trade some, cancel the rest, all at once
                    if (rng_.chance(1, 2)) execute(y, 1 + static_cast<Qty>(rng_.bounded(y.leaves)), y.px);
                    if (y.alive) cancel_shares(y, y.leaves, 'I');
                }
                return;
            }
            case Msg::cancel: {
                XOrd* x = at(m.c.token.to_id().value_or(0));
                if (x != nullptr && x->alive && x->active == m.c.token.to_id().value_or(0)) {
                    cancel_shares(*x, x->leaves, 'U');  // shares == 0 means all of it
                }
                return;  // a cancel for a dead or unknown order is ignored
            }
            case Msg::replace: {
                const std::uint64_t existing = m.r.existing.to_id().value_or(0);
                const std::uint64_t replacement = m.r.replacement.to_id().value_or(0);
                XOrd* x = at(existing);
                Rep r;
                // The size in a replace is the TOTAL: what is open afterwards is that less what
                // has executed, even if a partial cancel made the order smaller in between.
                if (x != nullptr && x->alive && x->active == existing && m.r.shares > x->cum &&
                    !rng_.chance(5, 100)) {
                    x->leaves = m.r.shares - x->cum;
                    x->active = replacement;
                    x->px = m.r.price;
                    by_token[replacement] = static_cast<std::size_t>(x - orders.data());
                    r.kind = Rep::rpl; r.r.a.token = m.r.replacement; r.r.a.shares = x->leaves;
                    r.r.a.price = m.r.price; r.r.a.ref = 9000 + replacement; r.r.a.order_state = 'L';
                    r.r.previous = m.r.existing;
                } else {
                    r.kind = Rep::rej; r.j.token = m.r.replacement; r.j.reason = 'R';
                }
                outbox.push_back(r);
                return;
            }
        }
    }

    // An event of the exchange's own: a fill or a partial cancel of some working order.
    void spontaneous() {
        if (orders.empty()) return;
        XOrd& x = orders[rng_.bounded(orders.size())];
        if (!x.alive) return;
        const Qty q = 1 + static_cast<Qty>(rng_.bounded(x.leaves));
        if (rng_.chance(70, 100)) {
            execute(x, q, std::max<Price>(1, x.px + rng_.range(-1000, 1000)));
        } else {
            cancel_shares(x, q, 'S');
        }
    }

private:
    Rng& rng_;
};

// ---- one episode -----------------------------------------------------------------------------------

// Fails the test once per episode and stops the run, so a broken invariant does not flood the log.
#define INV(cond, ...)                                                                        \
    do {                                                                                      \
        if (!broken_ && !(cond)) {                                                            \
            char ot_buf[256];                                                                 \
            std::snprintf(ot_buf, sizeof ot_buf, __VA_ARGS__);                                \
            ::ot_test::fail(__FILE__, __LINE__,                                               \
                            std::string(legal_ ? "legal" : "illegal") + " seed " +            \
                                std::to_string(seed_) + " step " + std::to_string(step_) +    \
                                ": " + #cond + " :: " + ot_buf);                              \
            broken_ = true;                                                                   \
        }                                                                                     \
    } while (0)


struct Totals {
    std::uint64_t steps{}, submits{}, amends{}, growth_amends{}, recycled{}, reopened_after_cancel{},
        dup_dropped{}, capacity{}, reentrant_calls{}, fills{}, replays{}, unknown_reports{};
};

class Episode {
public:
    Episode(std::uint64_t seed, bool legal, bool reentrant, Totals& totals)
        : seed_(seed), legal_(legal), rng_(seed), ex_(rng_), totals_(totals),
          lim_(limits(rng_)), cfg_(config(rng_)), risk_(lim_, kLocates),
          om_(cfg_, wire_, risk_, ref_, syms_, &lis_) {
        if (reentrant) {
            lis_.hook = [this] {
                if (rng_.chance(30, 100)) { ++totals_.reentrant_calls; act(); }
            };
        }
    }

    void run(int steps) {
        for (step_ = 0; step_ < steps && !broken_; ++step_) {
            ++totals_.steps;
            now_ += 1 + rng_.bounded(200'000'000);
            const std::uint64_t r = rng_.bounded(100);
            if (r < 30) {
                act();
            } else if (r < 55) {
                exchange_step();
            } else if (r < 68) {
                ex_.spontaneous();
            } else {
                deliver_one();
            }
            check();
        }
        if (legal_ && !broken_) settle();
    }

private:
    // ---- setup -------------------------------------------------------------------------

    static risk::Limits limits(Rng& r) {
        risk::Limits l;
        l.max_order_qty = 200;
        l.max_position = 150 + static_cast<std::int64_t>(r.bounded(300));
        l.max_gross_position = r.chance(1, 2) ? 0 : 200 + static_cast<std::int64_t>(r.bounded(300));
        l.max_open_orders = r.chance(1, 2) ? 0 : 3 + static_cast<std::uint32_t>(r.bounded(10));
        l.max_orders_per_second = r.chance(1, 2) ? 0 : 3 + static_cast<std::uint32_t>(r.bounded(20));
        l.price_band_bps = r.chance(4, 10) ? 500 : 0;
        l.max_order_notional = r.chance(3, 10) ? static_cast<std::int64_t>(kPx) * 120 : 0;
        l.max_loss = r.chance(1, 5) ? 20'000'000 : 0;
        return l;
    }
    static oms::OrderManager::Config config(Rng& r) {
        oms::OrderManager::Config c;
        c.max_orders = 4 + static_cast<std::size_t>(r.bounded(12));
        c.first_token = r.chance(1, 2) ? 1 : 1 + r.bounded(1'000'000);
        return c;
    }

    // ---- the strategy --------------------------------------------------------------------

    oms::OrderId pick_id() {
        const std::uint64_t n = om_.orders_submitted();
        if (n == 0 || rng_.chance(1, 20)) return n + 1 + rng_.bounded(3);  // never issued
        if (!watch_.empty() && rng_.chance(85, 100)) return watch_[rng_.bounded(watch_.size())];
        return 1 + rng_.bounded(n);                                        // possibly recycled
    }

    void act() {
        const std::uint64_t a = rng_.bounded(100);
        if (a < 35) return do_submit();
        if (a < 55) {
            const oms::OrderId id = pick_id();
            const bool tabled = om_.find(id) != nullptr;
            const oms::SubmitStatus s = om_.cancel(id, now_);
            if (!tabled) INV(s == oms::SubmitStatus::unknown_order, "cancel of untabled id %llu -> %d", (unsigned long long)id, (int)s);
            return;
        }
        if (a < 80) return do_replace();
        if (a < 83) { om_.cancel_all(now_); return; }
        if (a < 86) { risk_.set_kill_switch(rng_.chance(1, 2)); return; }
        if (a < 91) { risk_.mark(static_cast<Locate>(1 + rng_.bounded(2)), kPx + rng_.range(-30'000, 30'000)); return; }
        wire_.open = rng_.chance(85, 100);
    }

    void do_submit() {
        oms::OrderRequest q;
        q.locate = static_cast<Locate>(1 + rng_.bounded(3));
        q.side = rng_.chance(1, 2) ? Side::buy : Side::sell;
        q.price = kPx + rng_.range(-40'000, 40'000);
        q.qty = static_cast<Qty>(1 + rng_.bounded(150));
        q.tif = rng_.chance(1, 10) ? oms::Tif::ioc : oms::Tif::day;
        if (rng_.chance(1, 40)) q.qty = 0;
        const std::size_t before = om_.orders_submitted();
        const bool full = om_.open_orders() == slots();
        const oms::SubmitResult res = om_.submit(q, now_);
        if (res.status == oms::SubmitStatus::ok) {
            ++totals_.submits;
            INV(!full, "submitted into a full table");
            INV(res.id == before + 1, "id %llu after %zu orders", (unsigned long long)res.id, before);
            INV(wire_.enter_tokens.size() >= res.id, "ok submit sent nothing");
            if (wire_.enter_tokens.size() >= res.id) owner_[wire_.enter_tokens[res.id - 1]] = res.id;
            watch_.push_back(res.id);
        } else {
            INV(res.id == 0, "refused submit returned id %llu", (unsigned long long)res.id);
            INV(om_.orders_submitted() == before, "refused submit consumed an id");
            if (res.status == oms::SubmitStatus::capacity) {
                ++totals_.capacity;
                INV(full, "capacity with a free slot (%zu working of %zu)", om_.open_orders(), slots());
            } else if (full && q.qty != 0) {
                INV(false, "table full but submit answered %d", (int)res.status);
            }
        }
    }

    void do_replace() {
        const oms::OrderId id = pick_id();
        const oms::OrderInfo* i = om_.find(id);
        const oms::OrderInfo pre = i != nullptr ? *i : oms::OrderInfo{};
        const Qty cum = pre.cum_qty;
        Qty new_qty = rng_.chance(1, 2) ? cum + 1 + static_cast<Qty>(rng_.bounded(200))
                                        : static_cast<Qty>(rng_.bounded(260));
        const Price px = kPx + rng_.range(-40'000, 40'000);
        const std::size_t sent_before = wire_.inbox.size();
        const oms::SubmitStatus s = om_.replace(id, px, new_qty, now_);
        if (i == nullptr) {
            INV(s == oms::SubmitStatus::unknown_order, "replace of untabled id -> %d", (int)s);
            return;
        }
        if (s != oms::SubmitStatus::ok) {
            INV(wire_.inbox.size() == sent_before, "refused replace sent a message");
            return;
        }
        ++totals_.amends;
        // The limits that used to be skipped: size, notional and band on the NEW open quantity
        // at the NEW price, whatever the growth.
        const Qty open_after = new_qty - cum;
        INV(new_qty > cum && open_after <= lim_.max_order_qty, "amend to open %u over max_order_qty", open_after);
        INV(lim_.max_order_notional == 0 || static_cast<std::int64_t>(px) * open_after <= lim_.max_order_notional,
            "amend over max_order_notional");
        if (lim_.price_band_bps != 0 && ref_.reference_price(pre.req.locate) > 0) {
            const std::int64_t d = px > kPx ? px - kPx : kPx - px;
            INV(d * 10'000 <= static_cast<std::int64_t>(kPx) * lim_.price_band_bps, "amend outside the band");
        }
        if (open_after > pre.leaves_qty) ++totals_.growth_amends;
    }

    // ---- the exchange and the delivery of its reports --------------------------------------

    void exchange_step() {
        if (wire_.inbox.empty()) return;
        const Msg m = wire_.inbox.front();
        wire_.inbox.pop_front();
        ex_.process(m);
    }

    void deliver(const Rep& r) {
        recent_.push_back(r);
        if (recent_.size() > 6) recent_.pop_front();  // replays stay within the repeat window
        const std::uint64_t unknown = om_.stats().unknown_tokens;
        const std::uint64_t invalid = om_.stats().invalid_reports;
        switch (r.kind) {
            case Rep::acc: om_.on_accepted(r.a, now_); break;
            case Rep::exe: om_.on_executed(r.x, now_); break;
            case Rep::can: om_.on_canceled(r.c, now_); break;
            case Rep::rej: om_.on_rejected(r.j, now_); break;
            case Rep::rpl: om_.on_replaced(r.r, now_); break;
        }
        totals_.unknown_reports += om_.stats().unknown_tokens - unknown;
        if (r.kind == Rep::exe) totals_.dup_dropped += om_.stats().invalid_reports - invalid;
    }

    // Legal delivery, plus the bookkeeping that finds which order a Replaced belongs to.
    void deliver_legal(const Rep& r) {
        if (r.kind == Rep::rpl) {
            const auto it = owner_.find(r.r.previous.to_id().value_or(0));
            if (it != owner_.end()) {
                owner_[r.token()] = it->second;
                // The race under test: a partial cancel made the manager's order smaller than
                // what the exchange now reopens.
                const oms::OrderInfo* i = om_.find(it->second);
                if (i != nullptr && !oms::is_terminal(i->status) && r.r.a.shares > i->leaves_qty) {
                    ++totals_.reopened_after_cancel;
                }
            }
        }
        deliver(r);
    }

    void deliver_one() {
        if (ex_.outbox.empty()) return;
        Rep r = ex_.outbox.front();
        ex_.outbox.pop_front();
        if (legal_) return deliver_legal(r);
        const std::uint64_t m = rng_.bounded(100);
        if (m < 8) return;                                                   // lost
        if (m < 16) { deliver(r); deliver(r); return; }                      // repeated
        if (m < 22 && !ex_.outbox.empty()) {                                 // swapped with the next
            const Rep r2 = ex_.outbox.front();
            ex_.outbox.pop_front();
            deliver(r2);
            deliver(r);
            return;
        }
        if (m < 28 && !recent_.empty()) {                                    // an old one again
            ++totals_.replays;
            const Rep old = recent_[rng_.bounded(recent_.size())];  // copy: deliver() edits recent_
            deliver(old);
            deliver(r);
            return;
        }
        if (m < 38) { mangle(r); deliver(r); return; }
        if (m < 44) { deliver(garbage()); deliver(r); return; }
        deliver(r);
    }

    void mangle(Rep& r) {
        switch (r.kind) {
            case Rep::exe:
                switch (rng_.bounded(5)) {
                    case 0: r.x.shares *= 3; break;
                    case 1: r.x.price = 0; break;
                    case 2: r.x.shares = 0; break;
                    case 3: r.x.price = 0x1'0000'0005LL; break;
                    default: r.x.shares = 2'000'000; break;
                }
                break;
            case Rep::can: r.c.decrement = rng_.chance(1, 2) ? r.c.decrement * 4 : 0; break;
            case Rep::rpl:
                r.r.a.shares = rng_.chance(1, 2) ? 100'000 : 0;
                if (rng_.chance(1, 3)) r.r.a.order_state = 'D';
                if (rng_.chance(1, 3)) r.r.a.order_state = '?';
                break;
            case Rep::acc: r.a.order_state = rng_.chance(1, 2) ? 'D' : 'Z'; break;
            case Rep::rej: r.j.token = tok(1 + rng_.bounded(60)); break;
        }
    }

    Rep garbage() {
        Rep g;
        const std::uint64_t t = wire_.enter_tokens.empty() ? 1 : wire_.enter_tokens[rng_.bounded(wire_.enter_tokens.size())] + rng_.bounded(2);
        switch (rng_.bounded(5)) {
            case 0: g.kind = Rep::acc; g.a.token = tok(t); g.a.order_state = 'L'; break;
            case 1: g.kind = Rep::exe; g.x.token = tok(t); g.x.shares = 1 + static_cast<Qty>(rng_.bounded(50)); g.x.price = kPx; g.x.match = ++ex_.match; break;
            case 2: g.kind = Rep::can; g.c.token = tok(t); g.c.decrement = 1 + static_cast<Qty>(rng_.bounded(50)); break;
            case 3: g.kind = Rep::rej; g.j.token = tok(t); break;
            default: g.kind = Rep::rpl; g.r.a.token = tok(t + 1); g.r.previous = tok(t); g.r.a.shares = 1 + static_cast<Qty>(rng_.bounded(50)); g.r.a.price = kPx; g.r.a.order_state = 'L'; break;
        }
        return g;
    }

    // ---- invariants ------------------------------------------------------------------------

    std::size_t slots() const { return cfg_.max_orders; }

    void check() {
        // Fold new fills into the independent tallies.
        for (; fills_seen_ < lis_.fills.size(); ++fills_seen_) {
            const oms::Fill& f = lis_.fills[fills_seen_];
            ++totals_.fills;
            INV(f.qty > 0 && f.price > 0, "bad fill");
            INV(booked_.insert({f.id, f.match}).second, "execution %llu of order %llu booked twice", (unsigned long long)f.match, (unsigned long long)f.id);
            Tally& t = tally_[f.id];
            t.qty += f.qty;
            t.notional += static_cast<std::uint64_t>(f.price) * f.qty;
            position_[f.locate] += f.side == Side::buy ? static_cast<std::int64_t>(f.qty) : -static_cast<std::int64_t>(f.qty);
        }

        std::int64_t reserved[kLocates][2] = {};
        std::size_t working = 0;
        for (std::size_t k = 0; k < watch_.size();) {
            const oms::OrderId id = watch_[k];
            const oms::OrderInfo* ip = om_.find(id);
            if (ip == nullptr) {  // its slot was reused; it is gone for good
                ++totals_.recycled;
                INV(om_.reserved_qty(id) == 0, "recycled order holds exposure");
                frozen_.erase(id);
                watch_[k] = watch_.back();
                watch_.pop_back();
                continue;
            }
            ++k;
            const oms::OrderInfo& i = *ip;
            INV(i.id == id, "find(%llu) returned order %llu", (unsigned long long)id, (unsigned long long)i.id);
            INV(i.cum_qty <= i.req.qty && i.req.qty <= kMaxOrderQty && i.req.qty >= 1, "sizes cum %u qty %u", i.cum_qty, i.req.qty);
            INV(static_cast<std::uint64_t>(i.leaves_qty) + i.cum_qty <= i.req.qty, "leaves %u + cum %u > qty %u", i.leaves_qty, i.cum_qty, i.req.qty);
            const Tally t = tally_.count(id) ? tally_[id] : Tally{};
            INV(i.cum_qty == t.qty, "order %llu cum %u vs fills %llu", (unsigned long long)id, i.cum_qty, (unsigned long long)t.qty);
            INV(i.avg_price == (t.qty > 0 ? static_cast<Price>(t.notional / t.qty) : Price{0}), "avg price");
            const Qty res = om_.reserved_qty(id);
            if (oms::is_terminal(i.status)) {
                INV(i.leaves_qty == 0 && res == 0, "terminal order %llu holds leaves %u reserved %u", (unsigned long long)id, i.leaves_qty, res);
                auto it = frozen_.find(id);
                if (it == frozen_.end()) {
                    frozen_[id] = i;
                } else {
                    INV(same(it->second, i), "terminal order %llu changed", (unsigned long long)id);
                }
                continue;
            }
            ++working;
            INV(i.leaves_qty >= 1, "working order %llu with no leaves", (unsigned long long)id);
            INV(res >= i.leaves_qty, "reserved %u below leaves %u", res, i.leaves_qty);
            if (i.status == oms::OrderStatus::pending_new || i.status == oms::OrderStatus::live) {
                INV(res == i.leaves_qty, "nothing in flight yet reserved %u != leaves %u", res, i.leaves_qty);
            }
            INV(i.req.locate >= 1 && i.req.locate <= 3, "locate %u", i.req.locate);
            reserved[i.req.locate][index(i.req.side)] += res;
            if (legal_ && id - 1 < ex_.orders.size() && ex_.orders[id - 1].alive) {
                // Exposure is never understated: whatever the exchange really has open (a
                // replace it has already applied, a partial cancel not yet reported) is covered
                // by what the ledger holds for the order. The exchange is ahead of the manager,
                // so its open quantity can only exceed the manager's leaves through a replace,
                // and that case is what the reservation is for.
                INV(res >= ex_.orders[id - 1].leaves, "order %llu reserved %u below the exchange's open %u", (unsigned long long)id, res, ex_.orders[id - 1].leaves);
            }
        }
        INV(om_.open_orders() == working, "open_orders %zu vs %zu working", om_.open_orders(), working);
        INV(risk_.open_orders() == working, "risk open_orders %u vs %zu working", risk_.open_orders(), working);
        for (Locate l = 1; l <= 3; ++l) {
            for (int s = 0; s < 2; ++s) {
                const std::int64_t got = risk_.open_qty(l, s == 0 ? Side::buy : Side::sell);
                INV(got >= 0, "negative open quantity %lld", (long long)got);
                INV(got == reserved[l][s], "loc %u side %d risk open %lld vs reserved %lld", l, s, (long long)got, (long long)reserved[l][s]);
            }
            INV(risk_.position(l).qty == position_[l], "loc %u position %lld vs fills %lld", l, (long long)risk_.position(l).qty, (long long)position_[l]);
        }
    }

    static bool same(const oms::OrderInfo& a, const oms::OrderInfo& b) {
        return a.id == b.id && a.req.locate == b.req.locate && a.req.side == b.req.side &&
               a.req.price == b.req.price && a.req.qty == b.req.qty && a.status == b.status &&
               a.cum_qty == b.cum_qty && a.leaves_qty == b.leaves_qty && a.avg_price == b.avg_price &&
               a.exchange_ref == b.exchange_ref && a.last_update == b.last_update && a.reason == b.reason;
    }

    // Legal mode: deliver everything, then compare with the exchange's own books.
    void settle() {
        lis_.hook = nullptr;
        for (int guard = 0; guard < 100'000 && (!wire_.inbox.empty() || !ex_.outbox.empty()); ++guard) {
            while (!wire_.inbox.empty()) exchange_step();
            while (!ex_.outbox.empty()) {
                const Rep r = ex_.outbox.front();
                ex_.outbox.pop_front();
                deliver_legal(r);
            }
        }
        check();
        INV(wire_.inbox.empty() && ex_.outbox.empty(), "did not drain");
        const oms::OrderManager::Stats& st = om_.stats();
        INV(st.invalid_reports == 0, "%llu legal reports were called malformed", (unsigned long long)st.invalid_reports);
        INV(st.clamped_reports == 0, "%llu legal reports needed clamping", (unsigned long long)st.clamped_reports);

        std::int64_t position[kLocates] = {};
        std::int64_t open[kLocates][2] = {};
        std::size_t alive = 0;
        for (std::size_t k = 0; k < ex_.orders.size(); ++k) {
            const XOrd& x = ex_.orders[k];
            // The exchange saw the enters in the order they were sent, and ids are consecutive.
            const oms::OrderId id = k + 1;
            const oms::OrderInfo* i = om_.find(id);
            const Locate loc = x.loc;
            const std::int64_t signed_cum = x.side == Side::buy ? x.cum : -static_cast<std::int64_t>(x.cum);
            position[loc] += signed_cum;
            if (x.alive) {
                ++alive;
                open[loc][index(x.side)] += x.leaves;
            }
            if (i == nullptr) continue;  // recycled: covered by the totals
            INV(i->cum_qty == x.cum, "order %llu cum %u vs exchange %u", (unsigned long long)id, i->cum_qty, x.cum);
            if (x.alive) {
                INV(!oms::is_terminal(i->status), "order %llu alive at the exchange but %d here", (unsigned long long)id, (int)i->status);
                INV(i->leaves_qty == x.leaves, "order %llu leaves %u vs exchange %u", (unsigned long long)id, i->leaves_qty, x.leaves);
                INV(i->status != oms::OrderStatus::pending_cancel && i->status != oms::OrderStatus::pending_replace &&
                        i->status != oms::OrderStatus::pending_new,
                    "order %llu still %d after everything was delivered", (unsigned long long)id, (int)i->status);
            } else {
                INV(oms::is_terminal(i->status), "order %llu dead at the exchange but %d here", (unsigned long long)id, (int)i->status);
            }
        }
        INV(om_.open_orders() == alive, "working orders %zu vs exchange %zu", om_.open_orders(), alive);
        for (Locate l = 1; l <= 3; ++l) {
            INV(risk_.position(l).qty == position[l], "loc %u position %lld vs exchange %lld", l, (long long)risk_.position(l).qty, (long long)position[l]);
            INV(risk_.open_qty(l, Side::buy) == open[l][0], "loc %u open buys %lld vs exchange %lld", l, (long long)risk_.open_qty(l, Side::buy), (long long)open[l][0]);
            INV(risk_.open_qty(l, Side::sell) == open[l][1], "loc %u open sells %lld vs exchange %lld", l, (long long)risk_.open_qty(l, Side::sell), (long long)open[l][1]);
        }
    }

    struct Tally {
        std::uint64_t qty{};
        std::uint64_t notional{};
    };

    std::uint64_t seed_;
    bool legal_;
    Rng rng_;
    Exchange ex_;
    Totals& totals_;
    risk::Limits lim_;
    oms::OrderManager::Config cfg_;
    Wire wire_;
    Reference ref_;
    Symbols syms_;
    Listener lis_;
    risk::RiskEngine risk_;
    oms::OrderManager om_;

    Nanos now_{1'000'000};
    int step_{0};
    bool broken_{false};
    std::unordered_map<std::uint64_t, oms::OrderId> owner_;  // token -> order (legal mode bookkeeping)
    std::size_t fills_seen_{0};
    std::vector<oms::OrderId> watch_;  // ids that may still be in the table
    std::unordered_map<oms::OrderId, oms::OrderInfo> frozen_;
    std::unordered_map<oms::OrderId, Tally> tally_;
    std::set<std::pair<oms::OrderId, std::uint64_t>> booked_;
    std::int64_t position_[kLocates] = {};
    std::deque<Rep> recent_;
};

}  // namespace

namespace {

// Seeds are fixed. Each episode draws its own limits and table size from its seed.
Totals run_mode(bool legal, bool reentrant, std::uint64_t first_seed, int episodes, int steps) {
    Totals t;
    for (int k = 0; k < episodes; ++k) Episode(first_seed + static_cast<std::uint64_t>(k), legal, reentrant, t).run(steps);
    return t;
}

}  // namespace

OT_TEST(legal_reports_keep_the_books_and_converge_with_the_exchange) {
    const Totals t = run_mode(true, false, 1000, 60, 2500);
    if (std::getenv("OT_MODEL_VERBOSE")) std::printf("legal: steps %llu submits %llu amends %llu growth %llu recycled %llu capacity %llu fills %llu reopened %llu\n", (unsigned long long)t.steps, (unsigned long long)t.submits, (unsigned long long)t.amends, (unsigned long long)t.growth_amends, (unsigned long long)t.recycled, (unsigned long long)t.capacity, (unsigned long long)t.fills, (unsigned long long)t.reopened_after_cancel);
    OT_CHECK(t.submits > 2000 && t.fills > 1000);
    OT_CHECK(t.recycled > 500);               // slots were reused over and over
    OT_CHECK(t.capacity > 0);                 // and the table did fill up at times
    OT_CHECK(t.amends > 500 && t.growth_amends > 100);
    OT_CHECK(t.reopened_after_cancel > 0);    // a replace crossed a partial cancel
}

OT_TEST(legal_reports_with_a_listener_that_acts_from_its_callbacks) {
    const Totals t = run_mode(true, true, 2000, 40, 2500);
    OT_CHECK(t.reentrant_calls > 500);
    OT_CHECK(t.submits > 1000 && t.recycled > 300);
}

OT_TEST(lost_repeated_reordered_replayed_and_forged_reports_keep_the_books) {
    const Totals t = run_mode(false, false, 3000, 60, 2500);
    OT_CHECK(t.submits > 2000 && t.fills > 500);
    OT_CHECK(t.recycled > 500);
    OT_CHECK(t.dup_dropped > 20);             // repeated executions were recognised
    OT_CHECK(t.replays > 100);
    OT_CHECK(t.unknown_reports > 100);        // including reports for forgotten orders
}

OT_TEST(illegal_reports_with_a_listener_that_acts_from_its_callbacks) {
    const Totals t = run_mode(false, true, 4000, 40, 2500);
    OT_CHECK(t.reentrant_calls > 500);
    OT_CHECK(t.submits > 1000);
}

OT_TEST_MAIN()

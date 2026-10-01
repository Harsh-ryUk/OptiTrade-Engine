#pragma once

// Shared fixtures for the strategy tests: a scripted OrderApi that records every call,
// a helper that drives real MarketBooks through ITCH structs, and a RiskEngine.
//
// The fake never invents exchange behaviour on its own. Orders are created pending_new;
// the test moves them through their life cycle explicitly (set_status, ack_replace, ...)
// and tells the strategy about each change with the same on_order_update call the engine
// would make. `Responder` (bottom) is the one exception: it plays a trivially simple
// exchange for the randomized and determinism tests.

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "optitrade/book/market_books.hpp"
#include "optitrade/itch/messages.hpp"
#include "optitrade/oms/interfaces.hpp"
#include "optitrade/risk/risk_engine.hpp"
#include "optitrade/strategy/strategy.hpp"

namespace ot_strat {

using namespace optitrade;

struct Action {
    enum class Kind : std::uint8_t { submit, cancel, replace };
    Kind kind{};
    Nanos now{};
    oms::OrderId id{};  // the order acted on; for a submit the id it was given, 0 if refused
    Locate locate{};
    Side side{};
    Price price{};
    Qty qty{};
    oms::Tif tif{};

    friend bool operator==(const Action&, const Action&) = default;
};

class FakeOrders final : public oms::OrderApi {
public:
    std::vector<Action> actions;
    oms::SubmitStatus submit_status{oms::SubmitStatus::ok};   // returned by the next submits
    oms::SubmitStatus cancel_status{oms::SubmitStatus::ok};
    oms::SubmitStatus replace_status{oms::SubmitStatus::ok};

    oms::SubmitResult submit(const oms::OrderRequest& req, Nanos now) override {
        actions.push_back({Action::Kind::submit, now, 0, req.locate, req.side, req.price, req.qty, req.tif});
        if (submit_status != oms::SubmitStatus::ok) return {submit_status, 0, risk::Reject::none};
        oms::OrderInfo info;
        info.id = infos_.size() + 1;
        info.req = req;
        info.status = oms::OrderStatus::pending_new;
        info.leaves_qty = req.qty;
        infos_.push_back(info);
        pending_.push_back({});
        actions.back().id = info.id;
        return {oms::SubmitStatus::ok, info.id, risk::Reject::none};
    }

    oms::SubmitStatus cancel(oms::OrderId id, Nanos now) override {
        const oms::OrderInfo* o = find(id);
        actions.push_back({Action::Kind::cancel, now, id, o ? o->req.locate : Locate{0}, o ? o->req.side : Side::buy,
                           0, 0, oms::Tif::day});
        if (o == nullptr) return oms::SubmitStatus::unknown_order;
        if (cancel_status == oms::SubmitStatus::ok) mut(id).status = oms::OrderStatus::pending_cancel;
        return cancel_status;
    }

    oms::SubmitStatus replace(oms::OrderId id, Price price, Qty qty, Nanos now) override {
        const oms::OrderInfo* o = find(id);
        actions.push_back({Action::Kind::replace, now, id, o ? o->req.locate : Locate{0},
                           o ? o->req.side : Side::buy, price, qty, oms::Tif::day});
        if (o == nullptr) return oms::SubmitStatus::unknown_order;
        if (replace_status == oms::SubmitStatus::ok) {
            mut(id).status = oms::OrderStatus::pending_replace;
            pending_[id - 1] = {price, qty};
        }
        return replace_status;
    }

    const oms::OrderInfo* find(oms::OrderId id) const override {
        return id >= 1 && id <= infos_.size() ? &infos_[id - 1] : nullptr;
    }

    std::size_t open_orders() const override {
        std::size_t n = 0;
        for (const auto& i : infos_) n += oms::is_terminal(i.status) ? 0 : 1;
        return n;
    }

    // Test controls.
    oms::OrderInfo& mut(oms::OrderId id) { return infos_.at(id - 1); }
    void set_status(oms::OrderId id, oms::OrderStatus s) {
        mut(id).status = s;
        if (oms::is_terminal(s)) mut(id).leaves_qty = 0;
    }
    // Exchange acknowledges the last replace: new terms, back to live.
    void ack_replace(oms::OrderId id) {
        oms::OrderInfo& o = mut(id);
        o.req.price = pending_[id - 1].price;
        o.req.qty = pending_[id - 1].qty;
        o.leaves_qty = o.req.qty - o.cum_qty;
        o.status = oms::OrderStatus::live;
    }
    std::size_t count(Action::Kind k) const {
        std::size_t n = 0;
        for (const auto& a : actions) n += a.kind == k ? 1 : 0;
        return n;
    }
    oms::OrderId last_id() const { return infos_.size(); }

private:
    struct Pending {
        Price price{};
        Qty qty{};
    };
    std::vector<oms::OrderInfo> infos_;
    std::vector<Pending> pending_;
};

struct Harness {
    book::MarketBooks books;
    risk::RiskEngine risk;
    FakeOrders orders;
    Nanos now{0};
    OrderRef next_ref{1};
    std::vector<OrderRef> live_;

    explicit Harness(const risk::Limits& limits = default_limits())
        : books(book::MarketBooks::Config{4096, 32, 64}), risk(limits, 64) {}

    static risk::Limits default_limits() {
        risk::Limits l;
        l.max_order_qty = 100'000;
        l.max_position = 1'000'000;
        return l;
    }

    strategy::Context ctx() { return strategy::Context{now, books, orders, risk}; }

    OrderRef add(Locate loc, Side side, Price price, Qty qty) {
        itch::AddOrder m;
        m.h.locate = loc;
        m.ref = next_ref++;
        m.side = side;
        m.shares = qty;
        m.price = price;
        books.on(m);
        live_.push_back(m.ref);
        return m.ref;
    }
    void del(OrderRef ref) {
        itch::OrderDelete m;
        m.ref = ref;
        books.on(m);
        std::erase(live_, ref);
    }
    // Delete every order of one side (or both) of an instrument.
    void clear_side(Locate loc, Side side) {
        std::vector<OrderRef> doomed;
        for (OrderRef r : live_) {
            const auto* o = books.order(r);
            if (o != nullptr && o->locate == loc && o->side == side) doomed.push_back(r);
        }
        for (OrderRef r : doomed) del(r);
    }
    void clear(Locate loc) {
        clear_side(loc, Side::buy);
        clear_side(loc, Side::sell);
    }
    // Replace the whole book of `loc` with one order per (price, qty) pair.
    void set_book(Locate loc, const std::vector<std::pair<Price, Qty>>& bids,
                  const std::vector<std::pair<Price, Qty>>& asks) {
        clear(loc);
        for (auto [p, q] : bids) add(loc, Side::buy, p, q);
        for (auto [p, q] : asks) add(loc, Side::sell, p, q);
    }
    // Position change as if an order had filled.
    void fill(Locate loc, Side side, Qty qty, Price price) { risk.on_fill(loc, side, qty, price); }
};

// Minimal exchange for randomized runs: IOC orders fill completely at their limit, day
// orders go live at once, cancels and replaces succeed at once. Every state change is
// reported to the strategy exactly as the engine would.
template <class S>
class Responder {
public:
    Responder(Harness& h, S& s) : h_(h), s_(s) {}

    // Call after each strategy callback; handles every action recorded since the last call.
    void settle() {
        while (next_ < h_.orders.actions.size()) {
            const Action a = h_.orders.actions[next_++];
            const oms::OrderId id = a.id;
            if (id == 0 || h_.orders.find(id) == nullptr) continue;
            switch (a.kind) {
                case Action::Kind::submit:
                    if (a.tif == oms::Tif::ioc) {
                        full_fill(id, a.locate, a.side, a.qty, a.price);
                    } else {
                        h_.orders.set_status(id, oms::OrderStatus::live);
                        s_.on_order_update(*h_.orders.find(id), h_.ctx());
                    }
                    break;
                case Action::Kind::cancel:
                    h_.orders.set_status(id, oms::OrderStatus::canceled);
                    s_.on_order_update(*h_.orders.find(id), h_.ctx());
                    break;
                case Action::Kind::replace:
                    h_.orders.ack_replace(id);
                    s_.on_order_update(*h_.orders.find(id), h_.ctx());
                    break;
            }
        }
    }

private:
    void full_fill(oms::OrderId id, Locate loc, Side side, Qty qty, Price price) {
        h_.fill(loc, side, qty, price);
        oms::OrderInfo& o = h_.orders.mut(id);
        o.cum_qty = qty;
        o.avg_price = price;
        h_.orders.set_status(id, oms::OrderStatus::filled);
        oms::Fill f;
        f.id = id;
        f.locate = loc;
        f.side = side;
        f.qty = qty;
        f.price = price;
        f.ts = h_.now;
        s_.on_fill(f, h_.ctx());
        s_.on_order_update(*h_.orders.find(id), h_.ctx());
    }

    Harness& h_;
    S& s_;
    std::size_t next_{0};
};

}  // namespace ot_strat

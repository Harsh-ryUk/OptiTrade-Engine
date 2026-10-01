// Tests for the order manager.
//
// The scripted tests are known-answer scenarios: every expected number is derived by hand in the
// comment next to it, so a reviewer can redo the arithmetic. Reports are hand-built structs fed
// straight to the manager, and the gateway, reference source, symbol source and listener are
// recording fakes, so nothing here shares code with the module under test except the risk engine
// (whose ledger is one of the things being verified).
//
// The last group plays randomized legal and illegal report sequences against a manager and checks
// the ledger invariants after every single step. Its expectations are computed from the public
// interface and from an independent tally of the fills the listener saw.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "check.hpp"
#include "optitrade/core/digest.hpp"
#include "optitrade/core/rng.hpp"
#include "optitrade/core/types.hpp"
#include "optitrade/oms/order_manager.hpp"
#include "optitrade/ouch/codec.hpp"
#include "optitrade/ouch/messages.hpp"
#include "optitrade/risk/risk_engine.hpp"

using namespace optitrade;

namespace {

constexpr Locate kAapl = 1;
constexpr Locate kMsft = 2;
constexpr Locate kGoog = 3;
constexpr Locate kFar = 20;  // has a symbol but lies outside the risk engine's table
constexpr Price kPx = 1'000'000;  // 100.0000

const char* name(oms::OrderStatus s) {
    switch (s) {
        case oms::OrderStatus::pending_new: return "pending_new";
        case oms::OrderStatus::live: return "live";
        case oms::OrderStatus::pending_cancel: return "pending_cancel";
        case oms::OrderStatus::pending_replace: return "pending_replace";
        case oms::OrderStatus::filled: return "filled";
        case oms::OrderStatus::canceled: return "canceled";
        case oms::OrderStatus::rejected: return "rejected";
    }
    return "?";
}

const char* name(oms::SubmitStatus s) {
    switch (s) {
        case oms::SubmitStatus::ok: return "ok";
        case oms::SubmitStatus::rejected_by_risk: return "rejected_by_risk";
        case oms::SubmitStatus::invalid_request: return "invalid_request";
        case oms::SubmitStatus::unknown_order: return "unknown_order";
        case oms::SubmitStatus::bad_state: return "bad_state";
        case oms::SubmitStatus::capacity: return "capacity";
        case oms::SubmitStatus::gateway_busy: return "gateway_busy";
    }
    return "?";
}

// Failures print names instead of enum ordinals.
#define OT_SUBMIT(got, want) OT_CHECK_EQ(std::string_view(name(got)), std::string_view(want))
#define OT_REJECT(got, want) \
    OT_CHECK_EQ(std::string_view(risk::to_string(got)), std::string_view(risk::to_string(want)))
#define OT_STATE(om, id, want)                                                            \
    do {                                                                                  \
        const oms::OrderInfo* ot_i = (om).find(id);                                       \
        OT_CHECK_EQ(std::string_view(ot_i != nullptr ? name(ot_i->status) : "none"),      \
                    std::string_view(want));                                              \
    } while (0)

// ---------------------------------------------------------------------------------------------
// Fakes
// ---------------------------------------------------------------------------------------------

// Records every message it accepts. `set_open(false)` makes it refuse everything, `budget`
// lets exactly N more messages through.
struct RecordingGateway final : oms::OrderGateway {
    std::vector<ouch::EnterOrder> enters;
    std::vector<ouch::CancelOrder> cancels;
    std::vector<ouch::ReplaceOrder> replaces;
    std::vector<Nanos> times;
    std::size_t budget{std::numeric_limits<std::size_t>::max()};
    std::size_t refused{0};

    bool take(Nanos now) {
        if (budget == 0) {
            ++refused;
            return false;
        }
        --budget;
        times.push_back(now);
        return true;
    }
    bool send(const ouch::EnterOrder& m, Nanos now) override {
        if (!take(now)) return false;
        enters.push_back(m);
        return true;
    }
    bool send(const ouch::CancelOrder& m, Nanos now) override {
        if (!take(now)) return false;
        cancels.push_back(m);
        return true;
    }
    bool send(const ouch::ReplaceOrder& m, Nanos now) override {
        if (!take(now)) return false;
        replaces.push_back(m);
        return true;
    }
    std::size_t sent() const { return enters.size() + cancels.size() + replaces.size(); }
    void set_open(bool open) { budget = open ? std::numeric_limits<std::size_t>::max() : 0; }
};

struct FakeReference final : oms::ReferenceSource {
    std::array<Price, 32> px{};
    Price reference_price(Locate l) const override { return l < px.size() ? px[l] : 0; }
};

struct FakeSymbols final : oms::SymbolSource {
    Symbol symbol(Locate l) const override {
        switch (l) {
            case kAapl: return Symbol("AAPL");
            case kMsft: return Symbol("MSFT");
            case kGoog: return Symbol("GOOG");
            case kFar: return Symbol("FAR");
            default: return Symbol{};  // unknown instrument: all spaces
        }
    }
};

struct RecordingListener final : oms::OrderListener {
    std::vector<oms::Fill> fills;
    std::vector<oms::OrderInfo> updates;
    std::vector<char> log;  // 'F' for a fill, 'U' for an order update, in call order
    std::function<void(const oms::Fill&)> on_fill_hook;
    std::function<void(const oms::OrderInfo&)> on_update_hook;

    void on_fill(const oms::Fill& f) override {
        fills.push_back(f);
        log.push_back('F');
        if (on_fill_hook) on_fill_hook(f);
    }
    void on_order_update(const oms::OrderInfo& i) override {
        updates.push_back(i);
        log.push_back('U');
        if (on_update_hook) on_update_hook(i);
    }
};

// Only the two hard caps are open, so a test enables exactly the limit it is about.
risk::Limits wide() {
    risk::Limits l;
    l.max_order_qty = kMaxOrderQty;
    l.max_position = 1'000'000'000;
    return l;
}

struct Rig {
    RecordingGateway gw;
    FakeReference ref;
    FakeSymbols syms;
    RecordingListener lis;
    risk::RiskEngine risk;
    oms::OrderManager om;

    explicit Rig(const risk::Limits& limits = wide(), std::size_t max_orders = 64,
                 bool with_listener = true)
        : risk(limits, 8),
          om(oms::OrderManager::Config{max_orders}, gw, risk, ref, syms,
             with_listener ? &lis : nullptr) {}
};

oms::OrderRequest req(Locate loc, Side side, Price px, Qty qty, oms::Tif tif = oms::Tif::day) {
    oms::OrderRequest r;
    r.locate = loc;
    r.side = side;
    r.price = px;
    r.qty = qty;
    r.tif = tif;
    return r;
}

ouch::Token tok(std::uint64_t id) { return ouch::Token::from_id(id); }

// Report builders. Only the fields the manager may rely on are filled in.
ouch::Accepted acc(std::uint64_t token, OrderRef ref = 5000, char state = 'L') {
    ouch::Accepted a;
    a.token = tok(token);
    a.ref = ref;
    a.order_state = state;
    return a;
}
// Match numbers identify an execution (the manager drops a repeat), so unless a test is about
// repeats each call gets a fresh one.
ouch::Executed exe(std::uint64_t token, Qty shares, Price px, std::uint64_t match = 0) {
    static std::uint64_t fresh = 1'000'000;
    ouch::Executed x;
    x.token = tok(token);
    x.shares = shares;
    x.price = px;
    x.match = match != 0 ? match : ++fresh;
    return x;
}
ouch::Canceled can(std::uint64_t token, Qty dec, char reason = 'U') {
    ouch::Canceled c;
    c.token = tok(token);
    c.decrement = dec;
    c.reason = reason;
    return c;
}
ouch::Rejected rej(std::uint64_t token, char reason = 'O') {
    ouch::Rejected r;
    r.token = tok(token);
    r.reason = reason;
    return r;
}
// `open` is the Replaced.shares field: what is left exposed after the replace.
ouch::Replaced rpl(std::uint64_t prev, std::uint64_t next, Qty open, Price px, OrderRef ref = 6000,
                   char state = 'L') {
    ouch::Replaced r;
    r.a.token = tok(next);
    r.a.shares = open;
    r.a.price = px;
    r.a.ref = ref;
    r.a.order_state = state;
    r.previous = tok(prev);
    return r;
}

std::uint64_t token_id(const ouch::Token& t) { return t.to_id().value_or(0); }

// Submits a buy or sell on AAPL and acknowledges it. Returns the order id; the token is
// `token_id(rig.gw.enters.back().token)`.
oms::OrderId live_order(Rig& r, Side side, Price px, Qty qty, Nanos now = 100) {
    const auto res = r.om.submit(req(kAapl, side, px, qty), now);
    OT_SUBMIT(res.status, "ok");
    r.om.on_accepted(acc(token_id(r.gw.enters.back().token)), now + 1);
    return res.id;
}

bool same_info(const oms::OrderInfo& a, const oms::OrderInfo& b) {
    return a.id == b.id && a.req.locate == b.req.locate && a.req.side == b.req.side &&
           a.req.price == b.req.price && a.req.qty == b.req.qty && a.req.tif == b.req.tif &&
           a.status == b.status && a.cum_qty == b.cum_qty && a.leaves_qty == b.leaves_qty &&
           a.avg_price == b.avg_price && a.exchange_ref == b.exchange_ref &&
           a.last_update == b.last_update && a.reason == b.reason;
}

// Everything observable about the manager, the risk ledger, the listener and the gateway. Two
// equal fingerprints mean a call changed nothing.
std::uint64_t fingerprint(const Rig& r) {
    Digest d;
    const std::size_t n = r.om.orders_submitted();
    d.update(n);
    d.update(r.om.open_orders());
    for (oms::OrderId id = 1; id <= n; ++id) {
        const oms::OrderInfo* i = r.om.find(id);
        if (i == nullptr) continue;  // finished long ago and its slot was reused
        d.update(i->id);
        d.update(i->req.locate);
        d.update(static_cast<std::uint64_t>(i->req.side));
        d.update(static_cast<std::uint64_t>(i->req.price));
        d.update(i->req.qty);
        d.update(static_cast<std::uint64_t>(i->req.tif));
        d.update(static_cast<std::uint64_t>(i->status));
        d.update(i->cum_qty);
        d.update(i->leaves_qty);
        d.update(static_cast<std::uint64_t>(i->avg_price));
        d.update(i->exchange_ref);
        d.update(i->last_update);
        d.update(static_cast<std::uint64_t>(i->reason));
        d.update(r.om.reserved_qty(id));
    }
    for (Locate l = 0; l < 8; ++l) {
        d.update(static_cast<std::uint64_t>(r.risk.open_qty(l, Side::buy)));
        d.update(static_cast<std::uint64_t>(r.risk.open_qty(l, Side::sell)));
        d.update(static_cast<std::uint64_t>(r.risk.position(l).qty));
        d.update(static_cast<std::uint64_t>(r.risk.position(l).realized));
    }
    d.update(r.risk.open_orders());
    d.update(r.lis.fills.size());
    d.update(r.lis.updates.size());
    d.update(r.gw.sent());
    return d.value();
}

std::uint64_t dropped(const oms::OrderManager& om) {
    const auto& s = om.stats();
    return s.unknown_tokens + s.late_reports + s.invalid_reports;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// submit
// ---------------------------------------------------------------------------------------------

OT_TEST(submit_assigns_ids_and_tokens_and_builds_the_enter_order) {
    Rig r;
    const auto a = r.om.submit(req(kAapl, Side::buy, 1'234'500, 300), 1000);
    OT_SUBMIT(a.status, "ok");
    OT_CHECK_EQ(a.id, oms::OrderId{1});
    OT_REJECT(a.risk_reason, risk::Reject::none);

    OT_CHECK_EQ(r.gw.enters.size(), std::size_t{1});
    const ouch::EnterOrder& m = r.gw.enters[0];
    OT_CHECK(m.token == tok(1));
    OT_CHECK(m.side == Side::buy);
    OT_CHECK(!m.short_sell);
    OT_CHECK_EQ(m.shares, Qty{300});
    OT_CHECK(m.stock == Symbol("AAPL"));
    OT_CHECK_EQ(m.price, Price{1'234'500});
    OT_CHECK_EQ(m.time_in_force, ouch::kTifSystemHours);  // day orders live for the system hours
    OT_CHECK_EQ(r.gw.times[0], Nanos{1000});

    const oms::OrderInfo* i = r.om.find(1);
    OT_CHECK(i != nullptr);
    OT_CHECK_EQ(i->id, oms::OrderId{1});
    OT_CHECK_EQ(i->req.locate, kAapl);
    OT_CHECK_EQ(i->req.price, Price{1'234'500});
    OT_CHECK_EQ(i->req.qty, Qty{300});
    OT_STATE(r.om, 1, "pending_new");
    OT_CHECK_EQ(i->cum_qty, Qty{0});
    OT_CHECK_EQ(i->leaves_qty, Qty{300});
    OT_CHECK_EQ(i->avg_price, Price{0});
    OT_CHECK_EQ(i->exchange_ref, OrderRef{0});
    OT_CHECK_EQ(i->last_update, Nanos{1000});
    OT_CHECK_EQ(i->reason, '\0');
    OT_CHECK_EQ(r.om.open_orders(), std::size_t{1});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{300});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::sell), std::int64_t{0});
    OT_CHECK_EQ(r.risk.open_orders(), 1u);
    OT_CHECK_EQ(r.lis.updates.size(), std::size_t{1});
    OT_CHECK(r.lis.fills.empty());

    const auto b = r.om.submit(req(kMsft, Side::sell, 2'000'000, 50, oms::Tif::ioc), 2000);
    OT_SUBMIT(b.status, "ok");
    OT_CHECK_EQ(b.id, oms::OrderId{2});
    const ouch::EnterOrder& m2 = r.gw.enters[1];
    OT_CHECK(m2.token == tok(2));
    OT_CHECK(m2.side == Side::sell);
    OT_CHECK(m2.stock == Symbol("MSFT"));
    OT_CHECK_EQ(m2.time_in_force, ouch::kTifIoc);
    OT_CHECK_EQ(r.risk.open_qty(kMsft, Side::sell), std::int64_t{50});
    OT_CHECK_EQ(r.om.open_orders(), std::size_t{2});

    OT_CHECK(r.om.find(0) == nullptr);
    OT_CHECK(r.om.find(3) == nullptr);
    OT_CHECK_EQ(r.om.orders_submitted(), std::size_t{2});
}

OT_TEST(submit_rejects_malformed_requests) {
    Rig r;
    std::vector<oms::OrderRequest> bad;
    bad.push_back(req(kAapl, Side::buy, kPx, 0));
    bad.push_back(req(kAapl, Side::buy, kPx, kMaxOrderQty + 1));
    bad.push_back(req(kAapl, Side::buy, 0, 100));
    bad.push_back(req(kAapl, Side::buy, -5, 100));
    bad.push_back(req(kAapl, Side::sell, 0x1'0000'0000LL, 100));  // does not fit the u32 wire price
    bad.push_back(req(7, Side::buy, kPx, 100));                   // no symbol for this locate
    bad.push_back(req(60000, Side::buy, kPx, 100));
    oms::OrderRequest bad_side = req(kAapl, Side::buy, kPx, 100);
    bad_side.side = static_cast<Side>(2);
    bad.push_back(bad_side);
    oms::OrderRequest bad_tif = req(kAapl, Side::buy, kPx, 100);
    bad_tif.tif = static_cast<oms::Tif>(9);
    bad.push_back(bad_tif);

    for (const oms::OrderRequest& q : bad) {
        const auto res = r.om.submit(q, 10);
        OT_SUBMIT(res.status, "invalid_request");
        OT_CHECK_EQ(res.id, oms::OrderId{0});
        OT_REJECT(res.risk_reason, risk::Reject::none);
    }
    OT_CHECK_EQ(r.gw.sent(), std::size_t{0});
    OT_CHECK(r.lis.updates.empty());
    OT_CHECK_EQ(r.om.orders_submitted(), std::size_t{0});
    OT_CHECK_EQ(r.om.open_orders(), std::size_t{0});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
    OT_CHECK_EQ(r.risk.open_orders(), 0u);

    // The boundary values themselves are fine, and what leaves is encodable on the wire.
    OT_SUBMIT(r.om.submit(req(kAapl, Side::buy, kPx, 1), 11).status, "ok");
    OT_SUBMIT(r.om.submit(req(kAapl, Side::buy, kPx, kMaxOrderQty), 11).status, "ok");
    OT_SUBMIT(r.om.submit(req(kAapl, Side::buy, 1, 100), 11).status, "ok");
    OT_SUBMIT(r.om.submit(req(kAapl, Side::buy, 0xFFFF'FFFFLL, 100), 11).status, "ok");
    OT_CHECK_EQ(r.gw.enters.size(), std::size_t{4});
    for (const ouch::EnterOrder& m : r.gw.enters) {
        std::array<std::byte, 64> buf{};
        OT_CHECK_EQ(ouch::encode(m, buf), ouch::kEnterOrderLength);
    }
}

OT_TEST(submitted_order_survives_the_wire_round_trip) {
    // Decoding the bytes that would leave the process returns the manager's message, and the
    // decoder is independent code, so a field written to the wrong place shows up here.
    Rig r;
    r.om.submit(req(kMsft, Side::sell, 3'141'500, 777, oms::Tif::ioc), 5);
    std::array<std::byte, 64> buf{};
    const std::size_t n = ouch::encode(r.gw.enters[0], buf);
    OT_CHECK_EQ(n, std::size_t{49});
    struct Grab : ouch::NullHandler {
        using ouch::NullHandler::on;
        ouch::EnterOrder got;
        void on(const ouch::EnterOrder& m) noexcept { got = m; }
    } grab;
    OT_CHECK(ouch::decode_inbound(std::span<const std::byte>(buf.data(), n), grab) ==
             DecodeStatus::ok);
    OT_CHECK(grab.got.token == tok(1));
    OT_CHECK(grab.got.side == Side::sell);
    OT_CHECK_EQ(grab.got.shares, Qty{777});
    OT_CHECK(grab.got.stock == Symbol("MSFT"));
    OT_CHECK_EQ(grab.got.price, Price{3'141'500});
    OT_CHECK_EQ(grab.got.time_in_force, ouch::kTifIoc);
}

// Capacity is the number of orders that can work at the same time. A finished order gives its
// slot back, ids keep counting, and capacity is still answered before the risk check (which
// would otherwise spend rate budget on an order that cannot be recorded).
OT_TEST(capacity_is_about_working_orders_and_checked_before_risk) {
    risk::Limits lim = wide();
    lim.max_orders_per_second = 4;
    Rig r(lim, 3);
    for (int k = 0; k < 3; ++k) OT_SUBMIT(r.om.submit(req(kAapl, Side::buy, kPx, 10), 0).status, "ok");
    const auto full = r.om.submit(req(kAapl, Side::buy, kPx, 10), 0);
    OT_SUBMIT(full.status, "capacity");
    OT_CHECK_EQ(full.id, oms::OrderId{0});
    OT_CHECK_EQ(r.gw.sent(), std::size_t{3});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{30});
    // The refusal did not spend rate budget: three admissions so far, the fourth of four is
    // still available, and only then is the window full.
    OT_REJECT(r.risk.check(kAapl, Side::buy, kPx, 10, 0, 0), risk::Reject::none);
    OT_REJECT(r.risk.check(kAapl, Side::buy, kPx, 10, 0, 0), risk::Reject::rate_limit);

    // A finished order frees its slot; the next id is 4, never 1 again.
    r.om.on_accepted(acc(1), 1);
    r.om.on_executed(exe(1, 10, kPx), 2);
    OT_STATE(r.om, 1, "filled");
    OT_CHECK(r.om.find(1) != nullptr);  // still queryable until its slot is needed
    // Same again without the rate limit, which is used up above.
    lim.max_orders_per_second = 0;
    Rig q(lim, 3);
    for (int k = 0; k < 3; ++k) OT_SUBMIT(q.om.submit(req(kAapl, Side::buy, kPx, 10), 0).status, "ok");
    q.om.on_accepted(acc(1), 1);
    q.om.on_executed(exe(1, 10, kPx), 2);
    const auto again = q.om.submit(req(kAapl, Side::buy, kPx, 10), 3);
    OT_SUBMIT(again.status, "ok");
    OT_CHECK_EQ(again.id, oms::OrderId{4});
    OT_CHECK(q.om.find(1) == nullptr);   // the slot was reused
    OT_CHECK(q.om.find(4) != nullptr);
    OT_CHECK(q.om.find(2) != nullptr);
    OT_CHECK_EQ(q.om.orders_submitted(), std::size_t{4});
    OT_SUBMIT(q.om.submit(req(kAapl, Side::buy, kPx, 10), 4).status, "capacity");  // 2, 3, 4 work

    Rig none(wide(), 0);
    OT_SUBMIT(none.om.submit(req(kAapl, Side::buy, kPx, 10), 0).status, "capacity");
}

OT_TEST(risk_rejection_reports_the_reason_and_leaves_no_trace) {
    struct Case {
        risk::Limits limits;
        oms::OrderRequest q;
        risk::Reject want;
        Price ref;
    };
    std::vector<Case> cases;
    {
        risk::Limits l = wide();
        l.max_order_qty = 100;
        cases.push_back({l, req(kAapl, Side::buy, kPx, 101), risk::Reject::order_qty, 0});
    }
    {
        risk::Limits l = wide();
        l.max_order_notional = 50'000'000;  // 51 * 1'000'000 = 51'000'000 > 50'000'000
        cases.push_back({l, req(kAapl, Side::buy, kPx, 51), risk::Reject::order_notional, 0});
    }
    {
        risk::Limits l = wide();
        l.price_band_bps = 100;  // 1 % of 1'000'000 = 10'000; 1'010'001 is one unit outside
        cases.push_back({l, req(kAapl, Side::buy, 1'010'001, 10), risk::Reject::price_band, kPx});
        cases.push_back({l, req(kAapl, Side::sell, 989'999, 10), risk::Reject::price_band, kPx});
    }
    {
        risk::Limits l = wide();
        l.price_band_bps = 100;
        l.require_reference = true;
        cases.push_back({l, req(kAapl, Side::buy, kPx, 10), risk::Reject::no_reference, 0});
    }
    cases.push_back({wide(), req(kFar, Side::buy, kPx, 10), risk::Reject::invalid_order, 0});

    for (const Case& c : cases) {
        Rig r(c.limits);
        r.ref.px[c.q.locate < r.ref.px.size() ? c.q.locate : 0] = c.ref;
        const std::uint64_t before = fingerprint(r);
        const auto res = r.om.submit(c.q, 5);
        OT_SUBMIT(res.status, "rejected_by_risk");
        OT_REJECT(res.risk_reason, c.want);
        OT_CHECK_EQ(res.id, oms::OrderId{0});
        OT_CHECK_EQ(fingerprint(r), before);
        OT_CHECK(r.lis.updates.empty());
        OT_CHECK(r.om.find(1) == nullptr);
        OT_CHECK_EQ(r.risk.open_orders(), 0u);
    }

    // Edges: exactly on the band, exactly at the notional and quantity caps.
    {
        risk::Limits l = wide();
        l.price_band_bps = 100;
        l.max_order_notional = 50'000'000;
        l.max_order_qty = 100;
        Rig r(l);
        r.ref.px[kAapl] = kPx;
        OT_SUBMIT(r.om.submit(req(kAapl, Side::buy, 1'010'000, 49), 0).status, "ok");
        OT_SUBMIT(r.om.submit(req(kAapl, Side::sell, kPx, 50), 0).status, "ok");
    }
}

OT_TEST(position_rate_open_orders_and_kill_switch_rejections) {
    {  // position: worst case includes same-side working orders
        risk::Limits l = wide();
        l.max_position = 150;
        Rig r(l);
        OT_SUBMIT(r.om.submit(req(kAapl, Side::buy, kPx, 100), 0).status, "ok");
        const auto over = r.om.submit(req(kAapl, Side::buy, kPx, 51), 0);  // 100 + 51 = 151 > 150
        OT_SUBMIT(over.status, "rejected_by_risk");
        OT_REJECT(over.risk_reason, risk::Reject::position);
        OT_SUBMIT(r.om.submit(req(kAapl, Side::buy, kPx, 50), 0).status, "ok");  // exactly 150
        OT_CHECK_EQ(r.om.orders_submitted(), std::size_t{2});
        // Opposite side is not netted: 0 - 51 fits.
        OT_SUBMIT(r.om.submit(req(kAapl, Side::sell, kPx, 51), 0).status, "ok");
    }
    {  // rate limit: 2 per second, the window is (now - 1 s, now]
        risk::Limits l = wide();
        l.max_orders_per_second = 2;
        Rig r(l);
        OT_SUBMIT(r.om.submit(req(kAapl, Side::buy, kPx, 1), 0).status, "ok");
        OT_SUBMIT(r.om.submit(req(kAapl, Side::buy, kPx, 1), 1).status, "ok");
        const auto res = r.om.submit(req(kAapl, Side::buy, kPx, 1), 2);
        OT_SUBMIT(res.status, "rejected_by_risk");
        OT_REJECT(res.risk_reason, risk::Reject::rate_limit);
        OT_CHECK_EQ(r.om.orders_submitted(), std::size_t{2});
        // The oldest admission (t = 0) expires at exactly one second.
        OT_SUBMIT(r.om.submit(req(kAapl, Side::buy, kPx, 1), 999'999'999).status, "rejected_by_risk");
        OT_SUBMIT(r.om.submit(req(kAapl, Side::buy, kPx, 1), 1'000'000'000).status, "ok");
    }
    {  // open orders: freed by a terminal report
        risk::Limits l = wide();
        l.max_open_orders = 2;
        Rig r(l);
        OT_SUBMIT(r.om.submit(req(kAapl, Side::buy, kPx, 10), 0).status, "ok");
        OT_SUBMIT(r.om.submit(req(kMsft, Side::buy, kPx, 10), 0).status, "ok");
        const auto res = r.om.submit(req(kGoog, Side::buy, kPx, 10), 0);
        OT_SUBMIT(res.status, "rejected_by_risk");
        OT_REJECT(res.risk_reason, risk::Reject::open_orders);
        r.om.on_accepted(acc(1), 1);
        OT_SUBMIT(r.om.cancel(1, 2), "ok");
        r.om.on_canceled(can(1, 10), 3);
        OT_SUBMIT(r.om.submit(req(kGoog, Side::buy, kPx, 10), 4).status, "ok");
    }
    {  // kill switch
        Rig r;
        r.risk.set_kill_switch(true);
        const auto res = r.om.submit(req(kAapl, Side::buy, kPx, 1), 0);
        OT_SUBMIT(res.status, "rejected_by_risk");
        OT_REJECT(res.risk_reason, risk::Reject::kill_switch);
        r.risk.set_kill_switch(false);
        const auto ok = r.om.submit(req(kAapl, Side::buy, kPx, 1), 0);
        OT_SUBMIT(ok.status, "ok");
        OT_CHECK_EQ(ok.id, oms::OrderId{1});  // the refusal consumed no id
        OT_CHECK(r.gw.enters[0].token == tok(1));
    }
}

OT_TEST(gateway_busy_leaves_no_trace) {
    Rig r;
    r.gw.set_open(false);
    const std::uint64_t before = fingerprint(r);
    const auto res = r.om.submit(req(kAapl, Side::buy, kPx, 100), 10);
    OT_SUBMIT(res.status, "gateway_busy");
    OT_CHECK_EQ(res.id, oms::OrderId{0});
    OT_CHECK_EQ(r.gw.refused, std::size_t{1});
    OT_CHECK_EQ(fingerprint(r), before);
    OT_CHECK(r.om.find(1) == nullptr);
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
    OT_CHECK_EQ(r.risk.open_orders(), 0u);
    OT_CHECK(r.lis.updates.empty());

    // Neither the id nor the token was consumed.
    r.gw.set_open(true);
    const auto ok = r.om.submit(req(kAapl, Side::buy, kPx, 100), 11);
    OT_SUBMIT(ok.status, "ok");
    OT_CHECK_EQ(ok.id, oms::OrderId{1});
    OT_CHECK(r.gw.enters[0].token == tok(1));
    r.om.on_accepted(acc(1), 12);

    // A refused cancel and a refused replace change nothing either and burn no token.
    r.gw.set_open(false);
    const std::uint64_t live = fingerprint(r);
    OT_SUBMIT(r.om.cancel(1, 13), "gateway_busy");
    OT_SUBMIT(r.om.replace(1, kPx, 150, 13), "gateway_busy");
    OT_CHECK_EQ(fingerprint(r), live);
    OT_STATE(r.om, 1, "live");
    r.gw.set_open(true);
    OT_SUBMIT(r.om.replace(1, kPx, 150, 14), "ok");
    OT_CHECK(r.gw.replaces[0].replacement == tok(2));
}

// ---------------------------------------------------------------------------------------------
// acknowledgment and rejection
// ---------------------------------------------------------------------------------------------

OT_TEST(accepted_moves_pending_new_to_live_exactly_once) {
    Rig r;
    r.om.submit(req(kAapl, Side::buy, kPx, 100), 1000);
    r.om.on_accepted(acc(1, 777), 2000);
    OT_STATE(r.om, 1, "live");
    const oms::OrderInfo* i = r.om.find(1);
    OT_CHECK_EQ(i->exchange_ref, OrderRef{777});
    OT_CHECK_EQ(i->last_update, Nanos{2000});
    OT_CHECK_EQ(i->leaves_qty, Qty{100});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{100});
    OT_CHECK_EQ(r.lis.updates.size(), std::size_t{2});
    OT_STATE(r.om, 1, "live");
    OT_CHECK_EQ(std::string_view(name(r.lis.updates[1].status)), std::string_view("live"));

    // A repeated Accepted is dropped whole: ref, timestamp and listener stay as they were.
    r.om.on_accepted(acc(1, 999), 3000);
    OT_CHECK_EQ(i->exchange_ref, OrderRef{777});
    OT_CHECK_EQ(i->last_update, Nanos{2000});
    OT_CHECK_EQ(r.lis.updates.size(), std::size_t{2});
    OT_CHECK_EQ(r.om.stats().invalid_reports, std::uint64_t{1});

    // An order state other than 'L' or 'D' is malformed.
    r.om.submit(req(kAapl, Side::buy, kPx, 100), 4000);
    r.om.on_accepted(acc(2, 5, 'X'), 4001);
    OT_STATE(r.om, 2, "pending_new");
    OT_CHECK_EQ(r.om.stats().invalid_reports, std::uint64_t{2});
}

OT_TEST(order_dead_on_accept_is_a_terminal_cancel) {
    Rig r;
    r.om.submit(req(kAapl, Side::buy, kPx, 100, oms::Tif::ioc), 1000);
    r.om.on_accepted(acc(1, 42, 'D'), 2000);
    OT_STATE(r.om, 1, "canceled");
    const oms::OrderInfo* i = r.om.find(1);
    OT_CHECK_EQ(i->leaves_qty, Qty{0});
    OT_CHECK_EQ(i->cum_qty, Qty{0});
    OT_CHECK_EQ(i->exchange_ref, OrderRef{42});
    OT_CHECK_EQ(i->last_update, Nanos{2000});
    OT_CHECK_EQ(r.om.open_orders(), std::size_t{0});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
    OT_CHECK_EQ(r.risk.open_orders(), 0u);
    OT_CHECK_EQ(r.lis.updates.size(), std::size_t{2});
    // Nothing more is expected for this order; a straggler is late.
    r.om.on_executed(exe(1, 10, kPx), 3000);
    OT_CHECK_EQ(r.om.stats().late_reports, std::uint64_t{1});
    OT_CHECK_EQ(r.risk.position(kAapl).qty, std::int64_t{0});
}

OT_TEST(rejected_new_order_releases_its_exposure_once) {
    Rig r;
    r.om.submit(req(kAapl, Side::sell, kPx, 80), 1000);
    r.om.submit(req(kAapl, Side::sell, kPx, 20), 1001);
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::sell), std::int64_t{100});
    r.om.on_rejected(rej(1, 'X'), 2000);
    OT_STATE(r.om, 1, "rejected");
    const oms::OrderInfo* i = r.om.find(1);
    OT_CHECK_EQ(i->reason, 'X');
    OT_CHECK_EQ(i->leaves_qty, Qty{0});
    OT_CHECK_EQ(i->last_update, Nanos{2000});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::sell), std::int64_t{20});
    OT_CHECK_EQ(r.om.open_orders(), std::size_t{1});

    // A duplicate reject must not release the other order's shares.
    r.om.on_rejected(rej(1, 'X'), 2001);
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::sell), std::int64_t{20});
    OT_CHECK_EQ(r.om.stats().late_reports, std::uint64_t{1});
    OT_CHECK_EQ(r.lis.updates.size(), std::size_t{3});  // two submits + one reject
}

OT_TEST(an_order_the_exchange_holds_cannot_be_rejected) {
    Rig r;
    live_order(r, Side::buy, kPx, 100);
    const std::uint64_t before = fingerprint(r);
    r.om.on_rejected(rej(1, 'O'), 500);
    OT_CHECK_EQ(fingerprint(r), before);
    OT_STATE(r.om, 1, "live");
    OT_CHECK_EQ(r.om.stats().invalid_reports, std::uint64_t{1});

    // The same holds once a fill has proved the order exists, even without an Accepted.
    r.om.submit(req(kAapl, Side::buy, kPx, 100), 600);
    r.om.on_executed(exe(2, 10, kPx), 601);
    const std::uint64_t mid = fingerprint(r);
    r.om.on_rejected(rej(2, 'O'), 602);
    OT_CHECK_EQ(fingerprint(r), mid);
    OT_CHECK_EQ(r.om.stats().invalid_reports, std::uint64_t{2});
}

// ---------------------------------------------------------------------------------------------
// executions
// ---------------------------------------------------------------------------------------------

OT_TEST(partial_fills_then_full_fill_with_exact_average) {
    Rig r;
    r.om.submit(req(kAapl, Side::buy, 1'002'000, 300), 1000);
    r.om.on_accepted(acc(1), 1001);

    // 100 @ 100.0000
    r.om.on_executed(exe(1, 100, 1'000'000, 11), 2000);
    const oms::OrderInfo* i = r.om.find(1);
    OT_STATE(r.om, 1, "live");
    OT_CHECK_EQ(i->cum_qty, Qty{100});
    OT_CHECK_EQ(i->leaves_qty, Qty{200});
    OT_CHECK_EQ(i->avg_price, Price{1'000'000});
    OT_CHECK_EQ(i->last_update, Nanos{2000});
    OT_CHECK_EQ(r.risk.position(kAapl).qty, std::int64_t{100});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{200});
    OT_CHECK_EQ(r.lis.fills.size(), std::size_t{1});
    const oms::Fill& f = r.lis.fills[0];
    OT_CHECK_EQ(f.id, oms::OrderId{1});
    OT_CHECK_EQ(f.locate, kAapl);
    OT_CHECK(f.side == Side::buy);
    OT_CHECK_EQ(f.qty, Qty{100});
    OT_CHECK_EQ(f.price, Price{1'000'000});
    OT_CHECK_EQ(f.ts, Nanos{2000});
    OT_CHECK_EQ(f.match, std::uint64_t{11});

    // 50 @ 100.1000: notional 100*1'000'000 + 50*1'001'000 = 150'050'000, / 150 = 1'000'333.3
    r.om.on_executed(exe(1, 50, 1'001'000, 12), 2001);
    OT_CHECK_EQ(i->cum_qty, Qty{150});
    OT_CHECK_EQ(i->leaves_qty, Qty{150});
    OT_CHECK_EQ(i->avg_price, Price{1'000'333});
    OT_CHECK_EQ(r.risk.position(kAapl).qty, std::int64_t{150});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{150});
    OT_STATE(r.om, 1, "live");

    // 150 @ 100.2000: notional 150'050'000 + 150*1'002'000 = 300'350'000, / 300 = 1'001'166.67
    r.om.on_executed(exe(1, 150, 1'002'000, 13), 2002);
    OT_STATE(r.om, 1, "filled");
    OT_CHECK_EQ(i->cum_qty, Qty{300});
    OT_CHECK_EQ(i->leaves_qty, Qty{0});
    OT_CHECK_EQ(i->avg_price, Price{1'001'166});
    OT_CHECK_EQ(r.risk.position(kAapl).qty, std::int64_t{300});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
    OT_CHECK_EQ(r.risk.open_orders(), 0u);
    OT_CHECK_EQ(r.om.open_orders(), std::size_t{0});
    OT_CHECK_EQ(r.lis.fills.size(), std::size_t{3});

    // Listener: a fill is announced before the order update it belongs to, and every step of the
    // life cycle produced exactly one update: pending_new, live, three executions.
    OT_CHECK_EQ(std::string(r.lis.log.begin(), r.lis.log.end()), std::string("UUFUFUFU"));
    const char* expect[] = {"pending_new", "live", "live", "live", "filled"};
    OT_CHECK_EQ(r.lis.updates.size(), std::size_t{5});
    for (std::size_t k = 0; k < r.lis.updates.size() && k < 5; ++k) {
        OT_CHECK_EQ(std::string_view(name(r.lis.updates[k].status)), std::string_view(expect[k]));
    }
    OT_CHECK_EQ(r.lis.updates[4].cum_qty, Qty{300});
}

OT_TEST(average_price_is_the_exact_quotient_not_a_running_mean) {
    // 1 @ 1'000'000, 1 @ 1'000'001, 1 @ 1'000'002. Total 3'000'003 / 3 = 1'000'001 exactly.
    // A running mean truncates after every step: (1'000'000+1'000'001)/2 = 1'000'000, then
    // (2*1'000'000+1'000'002)/3 = 1'000'000, which is one unit low.
    Rig r;
    live_order(r, Side::buy, 1'010'000, 3);
    r.om.on_executed(exe(1, 1, 1'000'000), 10);
    OT_CHECK_EQ(r.om.find(1)->avg_price, Price{1'000'000});
    r.om.on_executed(exe(1, 1, 1'000'001), 11);
    OT_CHECK_EQ(r.om.find(1)->avg_price, Price{1'000'000});  // 2'000'001 / 2
    r.om.on_executed(exe(1, 1, 1'000'002), 12);
    OT_CHECK_EQ(r.om.find(1)->avg_price, Price{1'000'001});
}

OT_TEST(fills_feed_position_and_realized_pnl_in_the_risk_engine) {
    Rig r;
    live_order(r, Side::buy, 1'000'000, 100);   // token 1
    live_order(r, Side::sell, 1'010'000, 100);  // token 2
    r.om.on_executed(exe(1, 100, 1'000'000), 10);
    OT_CHECK_EQ(r.risk.position(kAapl).qty, std::int64_t{100});
    OT_CHECK_EQ(r.risk.position(kAapl).avg_price, Price{1'000'000});
    OT_CHECK_EQ(r.risk.realized_pnl(), std::int64_t{0});
    r.om.on_executed(exe(2, 100, 1'010'000), 11);
    OT_CHECK_EQ(r.risk.position(kAapl).qty, std::int64_t{0});
    // (1'010'000 - 1'000'000) * 100 price units
    OT_CHECK_EQ(r.risk.realized_pnl(), std::int64_t{1'000'000});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::sell), std::int64_t{0});

    // A sell into no position goes short: 60 @ 50.0000.
    live_order(r, Side::sell, 500'000, 60);  // token 3
    r.om.on_executed(exe(3, 60, 500'000), 12);
    OT_CHECK_EQ(r.risk.position(kAapl).qty, std::int64_t{-60});
    OT_CHECK(r.lis.fills.back().side == Side::sell);
}

// An Executed report is a fact. When it says more traded than the manager believed was open
// (a replace raced a partial cancel, a confused exchange), the position still gets every share;
// the order's size grows to what executed, and only the ledger release stops at what it holds.
OT_TEST(executions_beyond_the_open_quantity_are_booked_in_full) {
    Rig r;
    live_order(r, Side::buy, kPx, 100);
    r.om.on_executed(exe(1, 60, kPx), 10);
    r.om.on_executed(exe(1, 60, kPx), 11);  // 60 more traded, but only 40 were believed open
    const oms::OrderInfo* i = r.om.find(1);
    OT_STATE(r.om, 1, "filled");
    OT_CHECK_EQ(i->cum_qty, Qty{120});
    OT_CHECK_EQ(i->leaves_qty, Qty{0});
    OT_CHECK_EQ(i->req.qty, Qty{120});  // cum_qty <= req.qty always holds
    OT_CHECK_EQ(r.lis.fills.size(), std::size_t{2});
    OT_CHECK_EQ(r.lis.fills[1].qty, Qty{60});
    OT_CHECK_EQ(r.risk.position(kAapl).qty, std::int64_t{120});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});  // the release never goes negative
    OT_CHECK_EQ(r.risk.open_orders(), 0u);
    OT_CHECK_EQ(r.om.stats().clamped_reports, std::uint64_t{1});

    // A single oversize execution on a fresh order.
    live_order(r, Side::sell, kPx, 100);  // token 2
    r.om.on_executed(exe(2, 5000, kPx), 12);
    OT_STATE(r.om, 2, "filled");
    OT_CHECK_EQ(r.om.find(2)->cum_qty, Qty{5000});
    OT_CHECK_EQ(r.om.find(2)->req.qty, Qty{5000});
    OT_CHECK_EQ(r.risk.position(kAapl).qty, std::int64_t{120 - 5000});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::sell), std::int64_t{0});
    OT_CHECK_EQ(r.om.stats().clamped_reports, std::uint64_t{2});

    // Nothing beyond the largest order size can have traded: that is garbage, not a fact.
    live_order(r, Side::buy, kPx, 100);  // token 3
    const std::uint64_t before = fingerprint(r);
    r.om.on_executed(exe(3, kMaxOrderQty + 1, kPx), 13);
    OT_CHECK_EQ(fingerprint(r), before);
    OT_CHECK_EQ(r.om.stats().invalid_reports, std::uint64_t{1});
}

// Retransmits: the same match number on the same order is one execution, not two.
OT_TEST(a_repeated_execution_is_dropped_and_counted) {
    Rig r;
    live_order(r, Side::buy, kPx, 1000);
    r.om.on_executed(exe(1, 300, kPx, 77), 10);
    const std::uint64_t before = fingerprint(r);
    r.om.on_executed(exe(1, 300, kPx, 77), 11);
    OT_CHECK_EQ(fingerprint(r), before);
    OT_CHECK_EQ(r.om.stats().invalid_reports, std::uint64_t{1});
    OT_CHECK_EQ(r.om.find(1)->cum_qty, Qty{300});
    OT_CHECK_EQ(r.risk.position(kAapl).qty, std::int64_t{300});
    OT_CHECK_EQ(r.lis.fills.size(), std::size_t{1});

    // The exchange then cancels the true remainder and the order ends with the right numbers.
    OT_SUBMIT(r.om.cancel(1, 12), "ok");
    r.om.on_canceled(can(1, 700), 13);
    OT_STATE(r.om, 1, "canceled");
    OT_CHECK_EQ(r.risk.position(kAapl).qty, std::int64_t{300});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});

    // A different match number with identical content is a second execution.
    live_order(r, Side::buy, kPx, 1000);  // token 2
    r.om.on_executed(exe(2, 10, kPx, 5), 20);
    r.om.on_executed(exe(2, 10, kPx, 6), 21);
    OT_CHECK_EQ(r.om.find(2)->cum_qty, Qty{20});
    // Match numbers are per order: another order may see the same one.
    live_order(r, Side::buy, kPx, 1000);  // token 3
    r.om.on_executed(exe(3, 10, kPx, 5), 22);
    OT_CHECK_EQ(r.om.find(3)->cum_qty, Qty{10});
}

// The memory is a small window per order, not an unbounded set: repeats of a long-gone
// execution are not recognised (documented limit) but the most recent ones always are.
OT_TEST(the_repeat_window_covers_the_most_recent_executions_of_an_order) {
    Rig r;
    const oms::OrderId id = live_order(r, Side::buy, kPx, 100'000);
    constexpr std::uint64_t kN = oms::OrderManager::kMatchMemory;
    for (std::uint64_t m = 1; m <= 3 * kN; ++m) r.om.on_executed(exe(1, 1, kPx, m), 10 + m);
    OT_CHECK_EQ(r.om.find(id)->cum_qty, Qty{3 * kN});
    // Every one of the last kN is recognised as a repeat.
    for (std::uint64_t m = 2 * kN + 1; m <= 3 * kN; ++m) r.om.on_executed(exe(1, 1, kPx, m), 100);
    OT_CHECK_EQ(r.om.find(id)->cum_qty, Qty{3 * kN});
    OT_CHECK_EQ(r.om.stats().invalid_reports, kN);
    // A malformed report is not remembered: the same match can still arrive correctly.
    r.om.on_executed(exe(1, 0, kPx, 9999), 101);
    r.om.on_executed(exe(1, 1, kPx, 9999), 102);
    OT_CHECK_EQ(r.om.find(id)->cum_qty, Qty{3 * kN + 1});
}

OT_TEST(malformed_executions_are_dropped) {
    Rig r;
    live_order(r, Side::buy, kPx, 100);
    const std::uint64_t before = fingerprint(r);
    r.om.on_executed(exe(1, 0, kPx), 10);                    // no shares
    r.om.on_executed(exe(1, 10, 0), 11);                     // no price
    r.om.on_executed(exe(1, 10, -1), 12);                    // negative price
    r.om.on_executed(exe(1, 10, 0x1'0000'0000LL), 13);       // not a u32 price
    OT_CHECK_EQ(fingerprint(r), before);
    OT_CHECK_EQ(r.om.stats().invalid_reports, std::uint64_t{4});
    r.om.on_executed(exe(1, 10, 0xFFFF'FFFFLL), 14);          // the largest price is fine
    OT_CHECK_EQ(r.om.find(1)->cum_qty, Qty{10});
    OT_CHECK_EQ(r.om.find(1)->avg_price, Price{0xFFFF'FFFFLL});
}

OT_TEST(a_fill_before_accepted_is_booked_and_acknowledges_the_order) {
    Rig r;
    r.om.submit(req(kAapl, Side::buy, kPx, 100), 1);
    r.om.on_executed(exe(1, 30, kPx), 2);
    OT_STATE(r.om, 1, "live");
    OT_CHECK_EQ(r.om.find(1)->cum_qty, Qty{30});
    OT_CHECK_EQ(r.risk.position(kAapl).qty, std::int64_t{30});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{70});
    // The late Accepted still supplies the reference, without disturbing the fill.
    r.om.on_accepted(acc(1, 321), 3);
    OT_STATE(r.om, 1, "live");
    OT_CHECK_EQ(r.om.find(1)->exchange_ref, OrderRef{321});
    OT_CHECK_EQ(r.om.find(1)->cum_qty, Qty{30});
    r.om.on_accepted(acc(1, 322), 4);  // but only once
    OT_CHECK_EQ(r.om.find(1)->exchange_ref, OrderRef{321});
}

// ---------------------------------------------------------------------------------------------
// cancel
// ---------------------------------------------------------------------------------------------

OT_TEST(cancel_flow_pending_cancel_then_canceled_with_reason) {
    Rig r;
    live_order(r, Side::buy, kPx, 100);
    OT_SUBMIT(r.om.cancel(1, 5000), "ok");
    OT_CHECK_EQ(r.gw.cancels.size(), std::size_t{1});
    OT_CHECK(r.gw.cancels[0].token == tok(1));
    OT_CHECK_EQ(r.gw.cancels[0].shares, Qty{0});  // 0 = cancel everything that is open
    OT_CHECK_EQ(r.gw.times.back(), Nanos{5000});
    OT_STATE(r.om, 1, "pending_cancel");
    OT_CHECK_EQ(r.om.find(1)->last_update, Nanos{5000});
    OT_CHECK_EQ(r.om.open_orders(), std::size_t{1});  // still working until the exchange answers
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{100});

    // Cancelling twice is refused and sends nothing.
    OT_SUBMIT(r.om.cancel(1, 5001), "bad_state");
    OT_CHECK_EQ(r.gw.cancels.size(), std::size_t{1});

    r.om.on_canceled(can(1, 100, 'U'), 6000);
    OT_STATE(r.om, 1, "canceled");
    const oms::OrderInfo* i = r.om.find(1);
    OT_CHECK_EQ(i->reason, 'U');
    OT_CHECK_EQ(i->leaves_qty, Qty{0});
    OT_CHECK_EQ(i->cum_qty, Qty{0});
    OT_CHECK_EQ(i->last_update, Nanos{6000});
    OT_CHECK_EQ(r.om.open_orders(), std::size_t{0});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
    OT_CHECK_EQ(r.risk.open_orders(), 0u);

    OT_SUBMIT(r.om.cancel(1, 7000), "bad_state");  // terminal
    OT_SUBMIT(r.om.cancel(0, 7000), "unknown_order");
    OT_SUBMIT(r.om.cancel(2, 7000), "unknown_order");
    OT_CHECK_EQ(r.gw.cancels.size(), std::size_t{1});
}

OT_TEST(cancel_race_a_fill_after_the_cancel_was_sent) {
    Rig r;
    live_order(r, Side::buy, kPx, 100);
    r.om.on_executed(exe(1, 40, kPx), 10);
    OT_SUBMIT(r.om.cancel(1, 11), "ok");
    // The exchange executed 30 more before it saw the cancel.
    r.om.on_executed(exe(1, 30, kPx), 12);
    OT_STATE(r.om, 1, "pending_cancel");
    OT_CHECK_EQ(r.om.find(1)->cum_qty, Qty{70});
    OT_CHECK_EQ(r.om.find(1)->leaves_qty, Qty{30});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{30});
    // Then the cancel removes the remainder.
    r.om.on_canceled(can(1, 30, 'U'), 13);
    OT_STATE(r.om, 1, "canceled");
    OT_CHECK_EQ(r.om.find(1)->cum_qty, Qty{70});
    OT_CHECK_EQ(r.om.find(1)->leaves_qty, Qty{0});
    OT_CHECK_EQ(r.risk.position(kAapl).qty, std::int64_t{70});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
}

OT_TEST(cancel_race_where_the_fill_completes_the_order) {
    Rig r;
    live_order(r, Side::sell, kPx, 100);
    OT_SUBMIT(r.om.cancel(1, 10), "ok");
    r.om.on_executed(exe(1, 100, kPx), 11);  // the whole order traded before the cancel arrived
    OT_STATE(r.om, 1, "filled");
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::sell), std::int64_t{0});
    // The exchange ignores a cancel for a dead order, but if a Canceled did arrive it is late.
    const std::uint64_t before = fingerprint(r);
    r.om.on_canceled(can(1, 100), 12);
    OT_CHECK_EQ(fingerprint(r), before);
    OT_CHECK_EQ(r.om.stats().late_reports, std::uint64_t{1});
}

OT_TEST(partial_cancel_keeps_the_order_working) {
    Rig r;
    live_order(r, Side::buy, kPx, 100);
    r.om.on_canceled(can(1, 30, 'S'), 10);  // supervisory reduction
    OT_STATE(r.om, 1, "live");
    const oms::OrderInfo* i = r.om.find(1);
    OT_CHECK_EQ(i->leaves_qty, Qty{70});
    OT_CHECK_EQ(i->cum_qty, Qty{0});
    OT_CHECK_EQ(i->req.qty, Qty{100});  // the request is what was asked for, not what is left
    OT_CHECK_EQ(i->reason, 'S');
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{70});
    OT_CHECK_EQ(r.om.open_orders(), std::size_t{1});

    // A decrement larger than what is open is cut to it and ends the order.
    r.om.on_canceled(can(1, 500, 'U'), 11);
    OT_STATE(r.om, 1, "canceled");
    OT_CHECK_EQ(r.om.stats().clamped_reports, std::uint64_t{1});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
    OT_CHECK_EQ(r.risk.open_orders(), 0u);

    // A partial cancel followed by fills for the rest: filled, though cum < the original request.
    live_order(r, Side::buy, kPx, 100);  // token 2
    r.om.on_canceled(can(2, 30), 12);
    r.om.on_executed(exe(2, 70, kPx), 13);
    OT_STATE(r.om, 2, "filled");
    OT_CHECK_EQ(r.om.find(2)->cum_qty, Qty{70});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});

    // A zero decrement is meaningless.
    live_order(r, Side::buy, kPx, 10);  // token 3
    const std::uint64_t before = fingerprint(r);
    r.om.on_canceled(can(3, 0), 14);
    OT_CHECK_EQ(fingerprint(r), before);
}

OT_TEST(cancel_before_the_ack_is_ordered_behind_it) {
    {  // cancel sent while pending_new: Accepted then Canceled
        Rig r;
        r.om.submit(req(kAapl, Side::buy, kPx, 100), 1);
        OT_SUBMIT(r.om.cancel(1, 2), "ok");
        OT_CHECK(r.gw.cancels[0].token == tok(1));
        OT_STATE(r.om, 1, "pending_cancel");
        r.om.on_accepted(acc(1, 9), 3);
        OT_STATE(r.om, 1, "pending_cancel");  // the ack must not undo the cancel request
        OT_CHECK_EQ(r.om.find(1)->exchange_ref, OrderRef{9});
        r.om.on_canceled(can(1, 100), 4);
        OT_STATE(r.om, 1, "canceled");
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
    }
    {  // cancel sent while pending_new: the order was refused, so the cancel is moot
        Rig r;
        r.om.submit(req(kAapl, Side::buy, kPx, 100), 1);
        OT_SUBMIT(r.om.cancel(1, 2), "ok");
        r.om.on_rejected(rej(1, 'S'), 3);
        OT_STATE(r.om, 1, "rejected");
        OT_CHECK_EQ(r.om.find(1)->reason, 'S');
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
    }
}

// ---------------------------------------------------------------------------------------------
// replace
// ---------------------------------------------------------------------------------------------

OT_TEST(replace_growth_swaps_the_token_and_updates_the_terms) {
    Rig r;
    live_order(r, Side::buy, kPx, 100);  // token 1
    OT_SUBMIT(r.om.replace(1, 1'001'000, 150, 5000), "ok");
    OT_CHECK_EQ(r.gw.replaces.size(), std::size_t{1});
    const ouch::ReplaceOrder& m = r.gw.replaces[0];
    OT_CHECK(m.existing == tok(1));
    OT_CHECK(m.replacement == tok(2));
    OT_CHECK_EQ(m.shares, Qty{150});
    OT_CHECK_EQ(m.price, Price{1'001'000});
    OT_CHECK_EQ(m.time_in_force, ouch::kTifSystemHours);
    OT_CHECK_EQ(r.gw.times.back(), Nanos{5000});

    // Until the exchange answers the terms are unchanged, but the growth is already reserved.
    OT_STATE(r.om, 1, "pending_replace");
    const oms::OrderInfo* i = r.om.find(1);
    OT_CHECK_EQ(i->req.price, Price{kPx});
    OT_CHECK_EQ(i->req.qty, Qty{100});
    OT_CHECK_EQ(i->leaves_qty, Qty{100});
    OT_CHECK_EQ(r.om.reserved_qty(1), Qty{150});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{150});
    OT_CHECK_EQ(r.risk.open_orders(), 1u);  // growth does not count as a second order

    // The order still trades under its old token while the replace is in flight.
    r.om.on_executed(exe(1, 20, kPx, 1), 5100);
    OT_STATE(r.om, 1, "pending_replace");
    OT_CHECK_EQ(i->leaves_qty, Qty{80});
    OT_CHECK_EQ(r.om.reserved_qty(1), Qty{130});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{130});

    // Exchange view: total liable 150, executed 20, so 130 open.
    r.om.on_replaced(rpl(1, 2, 130, 1'001'000, 7777), 5200);
    OT_STATE(r.om, 1, "live");
    OT_CHECK_EQ(i->req.price, Price{1'001'000});
    OT_CHECK_EQ(i->req.qty, Qty{150});
    OT_CHECK_EQ(i->cum_qty, Qty{20});
    OT_CHECK_EQ(i->leaves_qty, Qty{130});
    OT_CHECK_EQ(i->cum_qty + i->leaves_qty, i->req.qty);
    OT_CHECK_EQ(i->exchange_ref, OrderRef{7777});
    OT_CHECK_EQ(i->last_update, Nanos{5200});
    OT_CHECK_EQ(r.om.reserved_qty(1), Qty{130});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{130});
    OT_CHECK_EQ(r.risk.open_orders(), 1u);

    // The old token is dead, the new one is the order's name from now on.
    const std::uint64_t before = fingerprint(r);
    r.om.on_executed(exe(1, 10, kPx), 5300);
    OT_CHECK_EQ(fingerprint(r), before);
    OT_CHECK_EQ(r.om.stats().unknown_tokens, std::uint64_t{1});
    r.om.on_executed(exe(2, 30, kPx, 2), 5301);
    OT_CHECK_EQ(i->cum_qty, Qty{50});
    OT_CHECK_EQ(i->leaves_qty, Qty{100});
    OT_SUBMIT(r.om.cancel(1, 5400), "ok");
    OT_CHECK(r.gw.cancels.back().token == tok(2));
}

OT_TEST(replace_shrink_keeps_exposure_until_the_exchange_confirms) {
    Rig r;
    live_order(r, Side::sell, kPx, 100);
    r.risk.set_kill_switch(true);  // reducing risk stays possible while everything else is halted
    OT_SUBMIT(r.om.replace(1, 990'000, 60, 10), "ok");
    OT_CHECK_EQ(r.gw.replaces[0].shares, Qty{60});
    OT_CHECK_EQ(r.om.reserved_qty(1), Qty{100});  // may still fill in full until acknowledged
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::sell), std::int64_t{100});
    r.om.on_replaced(rpl(1, 2, 60, 990'000), 11);
    OT_STATE(r.om, 1, "live");
    OT_CHECK_EQ(r.om.find(1)->req.qty, Qty{60});
    OT_CHECK_EQ(r.om.find(1)->req.price, Price{990'000});
    OT_CHECK_EQ(r.om.find(1)->leaves_qty, Qty{60});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::sell), std::int64_t{60});

    // Growing is checked and the halted engine refuses it, without touching the order.
    const std::uint64_t before = fingerprint(r);
    OT_SUBMIT(r.om.replace(1, 990'000, 61, 12), "rejected_by_risk");
    OT_CHECK_EQ(fingerprint(r), before);
    OT_STATE(r.om, 1, "live");
    OT_CHECK_EQ(r.gw.replaces.size(), std::size_t{1});
}

OT_TEST(replace_growth_is_risk_checked_against_the_additional_quantity) {
    risk::Limits l = wide();
    l.max_position = 150;
    l.price_band_bps = 100;
    Rig r(l);
    r.ref.px[kAapl] = kPx;
    live_order(r, Side::buy, kPx, 100);
    // 100 working + 51 more = 151 > 150.
    const std::uint64_t before = fingerprint(r);
    OT_SUBMIT(r.om.replace(1, kPx, 151, 10), "rejected_by_risk");
    OT_CHECK_EQ(fingerprint(r), before);
    OT_CHECK_EQ(r.gw.replaces.size(), std::size_t{0});
    // The added shares are priced against the reference band too: 2 % away is refused.
    OT_SUBMIT(r.om.replace(1, 1'020'000, 120, 11), "rejected_by_risk");
    OT_CHECK_EQ(fingerprint(r), before);
    // Exactly at both limits passes.
    OT_SUBMIT(r.om.replace(1, 1'010'000, 150, 12), "ok");
    OT_CHECK_EQ(r.om.reserved_qty(1), Qty{150});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{150});
    // While it is in flight the reservation counts against a new order on the same side.
    OT_SUBMIT(r.om.submit(req(kAapl, Side::buy, kPx, 1), 13).status, "rejected_by_risk");
}

OT_TEST(replace_argument_validation_and_state_rules) {
    Rig r;
    OT_SUBMIT(r.om.replace(0, kPx, 10, 1), "unknown_order");
    OT_SUBMIT(r.om.replace(1, kPx, 10, 1), "unknown_order");

    r.om.submit(req(kAapl, Side::buy, kPx, 100), 1);  // pending_new
    OT_SUBMIT(r.om.replace(1, kPx, 50, 2), "bad_state");
    r.om.on_accepted(acc(1), 3);
    r.om.on_executed(exe(1, 40, kPx), 4);  // cum 40, leaves 60

    const std::uint64_t before = fingerprint(r);
    OT_SUBMIT(r.om.replace(1, kPx, 0, 5), "invalid_request");
    OT_SUBMIT(r.om.replace(1, kPx, 39, 5), "invalid_request");       // below what has executed
    OT_SUBMIT(r.om.replace(1, kPx, 40, 5), "invalid_request");       // nothing would stay open
    OT_SUBMIT(r.om.replace(1, kPx, kMaxOrderQty + 1, 5), "invalid_request");
    OT_SUBMIT(r.om.replace(1, 0, 50, 5), "invalid_request");
    OT_SUBMIT(r.om.replace(1, -1, 50, 5), "invalid_request");
    OT_SUBMIT(r.om.replace(1, 0x1'0000'0000LL, 50, 5), "invalid_request");
    OT_CHECK_EQ(fingerprint(r), before);
    OT_CHECK_EQ(r.gw.replaces.size(), std::size_t{0});

    OT_SUBMIT(r.om.replace(1, kPx, 41, 6), "ok");  // one share left open is the smallest legal size
    OT_STATE(r.om, 1, "pending_replace");
    // A second replace, or a replace after cancel, is refused while one is pending.
    OT_SUBMIT(r.om.replace(1, kPx, 80, 7), "bad_state");
    OT_CHECK_EQ(r.gw.replaces.size(), std::size_t{1});
    r.om.on_replaced(rpl(1, 2, 1, kPx), 8);
    OT_CHECK_EQ(r.om.find(1)->leaves_qty, Qty{1});
    OT_CHECK_EQ(r.om.find(1)->req.qty, Qty{41});

    OT_SUBMIT(r.om.cancel(1, 9), "ok");
    OT_SUBMIT(r.om.replace(1, kPx, 80, 10), "bad_state");  // pending_cancel
    r.om.on_canceled(can(2, 1), 11);
    OT_STATE(r.om, 1, "canceled");
    OT_SUBMIT(r.om.replace(1, kPx, 80, 12), "bad_state");  // terminal
    OT_CHECK_EQ(r.gw.replaces.size(), std::size_t{1});
}

OT_TEST(replace_of_a_partly_filled_order_uses_the_total_size) {
    // The example from the protocol description: 500 entered, 100 executed.
    {   // (a) replace with 500 total keeps the 400 that are open
        Rig r;
        live_order(r, Side::buy, kPx, 500);
        r.om.on_executed(exe(1, 100, kPx), 10);
        OT_SUBMIT(r.om.replace(1, kPx, 500, 11), "ok");
        OT_CHECK_EQ(r.om.reserved_qty(1), Qty{400});  // no growth: 500 - 100 open equals what is open
        r.om.on_replaced(rpl(1, 2, 400, kPx), 12);
        OT_CHECK_EQ(r.om.find(1)->leaves_qty, Qty{400});
        OT_CHECK_EQ(r.om.find(1)->req.qty, Qty{500});
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{400});
    }
    {   // (b) replace with 600 total exposes 500 new shares
        Rig r;
        live_order(r, Side::buy, kPx, 500);
        r.om.on_executed(exe(1, 100, kPx), 10);
        OT_SUBMIT(r.om.replace(1, kPx, 600, 11), "ok");
        OT_CHECK_EQ(r.om.reserved_qty(1), Qty{500});
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{500});
        r.om.on_replaced(rpl(1, 2, 500, kPx), 12);
        OT_CHECK_EQ(r.om.find(1)->leaves_qty, Qty{500});
        OT_CHECK_EQ(r.om.find(1)->req.qty, Qty{600});
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{500});
    }
    {   // (c) the execution was in flight when the replace was sent: it precedes Replaced
        Rig r;
        live_order(r, Side::buy, kPx, 500);
        OT_SUBMIT(r.om.replace(1, kPx, 500, 10), "ok");
        r.om.on_executed(exe(1, 100, kPx), 11);
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{400});
        r.om.on_replaced(rpl(1, 2, 400, kPx), 12);
        OT_CHECK_EQ(r.om.find(1)->cum_qty, Qty{100});
        OT_CHECK_EQ(r.om.find(1)->leaves_qty, Qty{400});
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{400});
    }
}

OT_TEST(rejected_replace_returns_to_live_with_the_old_terms) {
    Rig r;
    live_order(r, Side::buy, kPx, 100);
    OT_SUBMIT(r.om.replace(1, 1'005'000, 150, 10), "ok");
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{150});
    // The rejection names the replacement token.
    r.om.on_rejected(rej(2, 'X'), 11);
    OT_STATE(r.om, 1, "live");
    const oms::OrderInfo* i = r.om.find(1);
    OT_CHECK_EQ(i->req.price, Price{kPx});
    OT_CHECK_EQ(i->req.qty, Qty{100});
    OT_CHECK_EQ(i->leaves_qty, Qty{100});
    OT_CHECK_EQ(i->reason, 'X');
    OT_CHECK_EQ(i->last_update, Nanos{11});
    OT_CHECK_EQ(r.om.reserved_qty(1), Qty{100});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{100});
    OT_CHECK_EQ(r.risk.open_orders(), 1u);
    OT_CHECK_EQ(r.om.open_orders(), std::size_t{1});

    // The replacement token is gone: a late Replaced for it is a stranger.
    const std::uint64_t before = fingerprint(r);
    r.om.on_replaced(rpl(1, 2, 150, 1'005'000), 12);
    OT_CHECK_EQ(fingerprint(r), before);
    OT_CHECK_EQ(r.om.stats().unknown_tokens, std::uint64_t{1});

    // The order can be replaced again, with a fresh token, and the original still trades.
    OT_SUBMIT(r.om.replace(1, kPx, 120, 13), "ok");
    OT_CHECK(r.gw.replaces[1].existing == tok(1));
    OT_CHECK(r.gw.replaces[1].replacement == tok(3));
    r.om.on_replaced(rpl(1, 3, 120, kPx), 14);
    OT_STATE(r.om, 1, "live");
    OT_CHECK_EQ(i->req.qty, Qty{120});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{120});
}

OT_TEST(replaced_report_is_never_trusted_to_add_exposure) {
    {   // the exchange claims more open shares than were asked for
        Rig r;
        live_order(r, Side::buy, kPx, 100);
        OT_SUBMIT(r.om.replace(1, kPx, 120, 10), "ok");
        r.om.on_replaced(rpl(1, 2, 5000, kPx), 11);
        OT_CHECK_EQ(r.om.find(1)->leaves_qty, Qty{120});
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{120});
    }
    {   // a shrink acknowledged with more shares than the order had
        Rig r;
        live_order(r, Side::buy, kPx, 100);
        OT_SUBMIT(r.om.replace(1, kPx, 60, 10), "ok");
        r.om.on_replaced(rpl(1, 2, 100, kPx), 11);  // more than the 60 asked for: cut to 60
        OT_CHECK_EQ(r.om.find(1)->leaves_qty, Qty{60});
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{60});
    }
    {   // the exchange reports fewer open shares (for instance a decrement of its own)
        Rig r;
        live_order(r, Side::buy, kPx, 100);
        OT_SUBMIT(r.om.replace(1, kPx, 100, 10), "ok");
        r.om.on_replaced(rpl(1, 2, 70, kPx), 11);
        OT_CHECK_EQ(r.om.find(1)->leaves_qty, Qty{70});
        OT_CHECK_EQ(r.om.find(1)->req.qty, Qty{100});
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{70});
    }
    {   // a missing price in the report falls back to the price that was requested
        Rig r;
        live_order(r, Side::buy, kPx, 100);
        OT_SUBMIT(r.om.replace(1, 1'003'000, 100, 10), "ok");
        r.om.on_replaced(rpl(1, 2, 100, 0), 11);
        OT_CHECK_EQ(r.om.find(1)->req.price, Price{1'003'000});
    }
}

OT_TEST(replaced_report_that_does_not_match_the_pending_replace_is_dropped) {
    Rig r;
    live_order(r, Side::buy, kPx, 100);
    // No replace outstanding: Replaced on the active token is out of order.
    std::uint64_t before = fingerprint(r);
    r.om.on_replaced(rpl(9, 1, 100, kPx), 5);
    OT_CHECK_EQ(fingerprint(r), before);

    OT_SUBMIT(r.om.replace(1, kPx, 120, 6), "ok");
    before = fingerprint(r);
    r.om.on_replaced(rpl(7, 2, 120, kPx), 7);          // wrong previous token
    r.om.on_replaced(rpl(1, 2, 120, kPx, 6000, 'Z'), 7);  // malformed order state
    OT_CHECK_EQ(fingerprint(r), before);
    OT_CHECK_EQ(r.om.stats().invalid_reports, std::uint64_t{3});
    // Reports other than Replaced/Rejected on the replacement token are out of order.
    r.om.on_executed(exe(2, 10, kPx), 8);
    r.om.on_canceled(can(2, 10), 8);
    r.om.on_accepted(acc(2), 8);
    OT_CHECK_EQ(fingerprint(r), before);
    OT_CHECK_EQ(r.om.stats().invalid_reports, std::uint64_t{6});
    // The correct one still works afterwards.
    r.om.on_replaced(rpl(1, 2, 120, kPx), 9);
    OT_STATE(r.om, 1, "live");
    OT_CHECK_EQ(r.om.find(1)->leaves_qty, Qty{120});
}

OT_TEST(replaced_order_dead_on_arrival_is_a_terminal_cancel) {
    Rig r;
    live_order(r, Side::buy, kPx, 100);
    r.om.on_executed(exe(1, 10, kPx), 9);
    OT_SUBMIT(r.om.replace(1, kPx, 200, 10), "ok");
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{190});
    r.om.on_replaced(rpl(1, 2, 190, kPx, 6000, 'D'), 11);
    OT_STATE(r.om, 1, "canceled");
    OT_CHECK_EQ(r.om.find(1)->leaves_qty, Qty{0});
    OT_CHECK_EQ(r.om.find(1)->cum_qty, Qty{10});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
    OT_CHECK_EQ(r.risk.open_orders(), 0u);
    OT_CHECK_EQ(r.om.open_orders(), std::size_t{0});
}

OT_TEST(exchange_cancel_races_a_replace) {
    Rig r;
    live_order(r, Side::buy, kPx, 100);
    OT_SUBMIT(r.om.replace(1, kPx, 150, 10), "ok");
    // The exchange cancelled the order on its own before it saw the replace.
    r.om.on_canceled(can(1, 100, 'T'), 11);
    OT_STATE(r.om, 1, "canceled");
    OT_CHECK_EQ(r.om.find(1)->reason, 'T');
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
    // The replace can then only fail, and its token is already forgotten.
    const std::uint64_t before = fingerprint(r);
    r.om.on_rejected(rej(2, 'O'), 12);
    OT_CHECK_EQ(fingerprint(r), before);
    OT_CHECK_EQ(r.om.stats().unknown_tokens, std::uint64_t{1});
}

OT_TEST(cancel_while_a_replace_is_pending_targets_the_replacement_token) {
    {   // the replace succeeds, then the cancel takes the order out
        Rig r;
        live_order(r, Side::buy, kPx, 100);
        OT_SUBMIT(r.om.replace(1, kPx, 150, 10), "ok");
        OT_SUBMIT(r.om.cancel(1, 11), "ok");
        OT_CHECK(r.gw.cancels[0].token == tok(2));
        OT_STATE(r.om, 1, "pending_cancel");
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{150});
        r.om.on_replaced(rpl(1, 2, 150, kPx), 12);
        OT_STATE(r.om, 1, "pending_cancel");  // the cancel is still in flight
        OT_CHECK_EQ(r.om.find(1)->req.qty, Qty{150});
        OT_CHECK_EQ(r.om.find(1)->leaves_qty, Qty{150});
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{150});
        r.om.on_canceled(can(2, 150), 13);
        OT_STATE(r.om, 1, "canceled");
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
    }
    {   // the replace fails, and the cancel that named its token died with it
        Rig r;
        live_order(r, Side::buy, kPx, 100);
        OT_SUBMIT(r.om.replace(1, kPx, 150, 10), "ok");
        OT_SUBMIT(r.om.cancel(1, 11), "ok");
        r.om.on_rejected(rej(2), 12);
        OT_STATE(r.om, 1, "live");
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{100});
        OT_SUBMIT(r.om.cancel(1, 13), "ok");  // and it can be asked again
        OT_CHECK(r.gw.cancels[1].token == tok(1));
    }
    {   // the replace and the cancel are both overtaken by a full exchange-side cancel
        Rig r;
        live_order(r, Side::buy, kPx, 100);
        OT_SUBMIT(r.om.replace(1, kPx, 150, 10), "ok");
        OT_SUBMIT(r.om.cancel(1, 11), "ok");
        r.om.on_canceled(can(1, 100, 'S'), 12);
        OT_STATE(r.om, 1, "canceled");
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
    }
}

OT_TEST(token_table_does_not_fill_up_across_many_replaces) {
    // Two orders' worth of table, one order replaced over and over: every superseded token and every
    // token of a refused replace must be forgotten, or the table would run dry after a few rounds.
    Rig r(wide(), 2);
    live_order(r, Side::buy, kPx, 10);
    std::uint64_t active = 1;
    for (int round = 0; round < 60; ++round) {
        const Qty size = 10 + static_cast<Qty>(round % 3);
        OT_SUBMIT(r.om.replace(1, kPx, size, 100 + round), "ok");
        const std::uint64_t next = token_id(r.gw.replaces.back().replacement);
        OT_CHECK_EQ(next, static_cast<std::uint64_t>(round + 2));
        if (round % 4 == 3) {
            r.om.on_rejected(rej(next), 200 + round);  // refused: the order keeps its token
        } else {
            r.om.on_replaced(rpl(active, next, size, kPx), 200 + round);
            active = next;
        }
        OT_STATE(r.om, 1, "live");
    }
    OT_CHECK_EQ(r.gw.replaces.size(), std::size_t{60});
    // The table still has room for the second order, and reports for it are matched.
    OT_SUBMIT(r.om.submit(req(kAapl, Side::sell, kPx, 5), 999).status, "ok");
    OT_CHECK_EQ(r.om.stats().unknown_tokens, std::uint64_t{0});
}

OT_TEST(exposure_of_several_orders_is_released_share_for_share) {
    Rig r;
    live_order(r, Side::buy, kPx, 100);   // token 1
    live_order(r, Side::buy, kPx, 50);    // token 2
    live_order(r, Side::sell, kPx, 70);   // token 3
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{150});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::sell), std::int64_t{70});
    r.om.on_canceled(can(1, 100), 10);
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{50});
    r.om.on_executed(exe(2, 20, kPx), 11);
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{30});
    r.om.on_canceled(can(2, 5), 12);  // partial cancel on top of the partial fill
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{25});
    r.om.on_executed(exe(2, 25, kPx), 13);
    OT_STATE(r.om, 2, "filled");
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::sell), std::int64_t{70});
    OT_CHECK_EQ(r.om.open_orders(), std::size_t{1});
    // Duplicate terminal reports cannot release order 3's shares.
    r.om.on_canceled(can(1, 100), 14);
    r.om.on_executed(exe(2, 25, kPx), 15);
    r.om.on_rejected(rej(1), 16);
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::sell), std::int64_t{70});
    OT_CHECK_EQ(r.risk.position(kAapl).qty, std::int64_t{45});  // 20 + 25 bought
}

OT_TEST(open_order_count_stays_exact_across_growing_replaces) {
    risk::Limits l = wide();
    l.max_open_orders = 3;
    Rig r(l);
    live_order(r, Side::buy, kPx, 100);   // token 1
    live_order(r, Side::sell, kPx, 100);  // token 2
    OT_CHECK_EQ(r.risk.open_orders(), 2u);
    // Growing an order and having the exchange refuse it must not leave a phantom order behind.
    for (Qty size = 110; size <= 150; size += 10) {
        OT_SUBMIT(r.om.replace(1, kPx, size, size), "ok");
        OT_CHECK_EQ(r.risk.open_orders(), 2u);
        r.om.on_rejected(rej(token_id(r.gw.replaces.back().replacement)), size);
        OT_STATE(r.om, 1, "live");
        OT_CHECK_EQ(r.risk.open_orders(), 2u);
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{100});
    }
    // Nor may a growth that succeeds.
    OT_SUBMIT(r.om.replace(1, kPx, 200, 1000), "ok");
    r.om.on_replaced(rpl(1, token_id(r.gw.replaces.back().replacement), 200, kPx), 1001);
    OT_CHECK_EQ(r.risk.open_orders(), 2u);
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{200});
    // So exactly one more order fits under the limit of three.
    OT_SUBMIT(r.om.submit(req(kMsft, Side::buy, kPx, 10), 1002).status, "ok");
    const auto res = r.om.submit(req(kGoog, Side::buy, kPx, 10), 1003);
    OT_SUBMIT(res.status, "rejected_by_risk");
    OT_REJECT(res.risk_reason, risk::Reject::open_orders);
}

// ---------------------------------------------------------------------------------------------
// late, unknown and duplicate reports
// ---------------------------------------------------------------------------------------------

OT_TEST(reports_for_terminal_orders_change_nothing) {
    for (int how = 0; how < 3; ++how) {
        Rig r;
        live_order(r, Side::buy, kPx, 100);  // token 1
        if (how == 0) {
            r.om.on_executed(exe(1, 100, kPx), 10);
        } else if (how == 1) {
            r.om.cancel(1, 10);
            r.om.on_canceled(can(1, 100), 11);
        } else {
            r.om.submit(req(kAapl, Side::buy, kPx, 100), 10);  // token 2, never acknowledged
            r.om.on_rejected(rej(2), 11);
        }
        const oms::OrderId id = how == 2 ? 2 : 1;
        const std::uint64_t token = id;
        OT_CHECK(oms::is_terminal(r.om.find(id)->status));
        const oms::OrderInfo snapshot = *r.om.find(id);
        const std::uint64_t before = fingerprint(r);
        const std::uint64_t late_before = r.om.stats().late_reports;

        r.om.on_accepted(acc(token), 100);
        r.om.on_executed(exe(token, 10, kPx), 101);
        r.om.on_canceled(can(token, 10), 102);
        r.om.on_rejected(rej(token), 103);
        r.om.on_replaced(rpl(token, token, 10, kPx), 104);

        OT_CHECK(same_info(*r.om.find(id), snapshot));
        OT_CHECK_EQ(fingerprint(r), before);
        OT_CHECK_EQ(r.om.stats().late_reports, late_before + 5);
        OT_SUBMIT(r.om.cancel(id, 105), "bad_state");
        OT_SUBMIT(r.om.replace(id, kPx, 50, 106), "bad_state");
    }
}

OT_TEST(reports_with_unknown_tokens_are_ignored_and_counted) {
    Rig r;
    live_order(r, Side::buy, kPx, 100);  // token 1
    const std::uint64_t before = fingerprint(r);
    const ouch::Token strangers[] = {
        tok(999),                             // never issued
        ouch::Token::from_text("1"),          // aliases order 1 as a number, but is not our token
        ouch::Token::from_text("ABC"),        // not numeric
        ouch::Token::from_text(" 1"),         // leading space
        ouch::Token::from_text("00000000000 1"),  // embedded space
        ouch::Token{},                        // blank
        ouch::Token::invalid(),               // '#' fill
    };
    std::uint64_t expect = 0;
    for (const ouch::Token& t : strangers) {
        ouch::Accepted a;  a.token = t;
        ouch::Executed x;  x.token = t;  x.shares = 10;  x.price = kPx;
        ouch::Canceled c;  c.token = t;  c.decrement = 10;
        ouch::Rejected j;  j.token = t;
        ouch::Replaced p;  p.a.token = t;  p.a.shares = 10;  p.a.price = kPx;  p.previous = tok(1);
        r.om.on_accepted(a, 50);
        r.om.on_executed(x, 51);
        r.om.on_canceled(c, 52);
        r.om.on_rejected(j, 53);
        r.om.on_replaced(p, 54);
        expect += 5;
        OT_CHECK_EQ(r.om.stats().unknown_tokens, expect);
    }
    OT_CHECK_EQ(fingerprint(r), before);
    OT_CHECK_EQ(r.om.stats().late_reports, std::uint64_t{0});
    OT_CHECK_EQ(r.om.stats().invalid_reports, std::uint64_t{0});
}

// ---------------------------------------------------------------------------------------------
// cancel_all
// ---------------------------------------------------------------------------------------------

OT_TEST(cancel_all_covers_every_working_order_once) {
    Rig r;
    r.om.submit(req(kAapl, Side::buy, kPx, 10), 1);    // 1: pending_new
    live_order(r, Side::buy, kPx, 20);                 // 2: live
    live_order(r, Side::sell, kPx, 30);                // 3: pending_replace below
    live_order(r, Side::buy, kPx, 40);                 // 4: pending_cancel already
    live_order(r, Side::buy, kPx, 50);                 // 5: filled
    live_order(r, Side::buy, kPx, 60);                 // 6: canceled
    live_order(r, Side::buy, kPx, 70);                 // 7: live
    OT_SUBMIT(r.om.replace(3, kPx, 35, 20), "ok");
    OT_SUBMIT(r.om.cancel(4, 21), "ok");
    r.om.on_executed(exe(5, 50, kPx), 22);
    OT_SUBMIT(r.om.cancel(6, 23), "ok");
    r.om.on_canceled(can(6, 60), 24);
    const std::uint64_t repl_token = *r.gw.replaces[0].replacement.to_id();
    const std::size_t cancels_before = r.gw.cancels.size();  // orders 4 and 6
    const std::size_t updates_before = r.lis.updates.size();

    OT_CHECK_EQ(r.om.cancel_all(100), std::size_t{4});  // orders 1, 2, 3, 7
    OT_CHECK_EQ(r.gw.cancels.size(), cancels_before + 4);
    OT_CHECK(r.gw.cancels[cancels_before + 0].token == tok(1));
    OT_CHECK(r.gw.cancels[cancels_before + 1].token == tok(2));
    OT_CHECK(r.gw.cancels[cancels_before + 2].token == tok(repl_token));  // replace in flight
    OT_CHECK(r.gw.cancels[cancels_before + 3].token == tok(7));
    for (std::size_t k = 0; k < 4; ++k) OT_CHECK_EQ(r.gw.cancels[cancels_before + k].shares, Qty{0});
    OT_CHECK_EQ(r.lis.updates.size(), updates_before + 4);
    for (oms::OrderId id : {1, 2, 3, 4, 7}) OT_STATE(r.om, id, "pending_cancel");
    OT_STATE(r.om, 5, "filled");
    OT_STATE(r.om, 6, "canceled");
    OT_CHECK_EQ(r.om.find(2)->last_update, Nanos{100});
    OT_CHECK_EQ(r.om.find(4)->last_update, Nanos{21});  // was already cancelling: untouched

    OT_CHECK_EQ(r.om.cancel_all(101), std::size_t{0});
    OT_CHECK_EQ(r.gw.cancels.size(), cancels_before + 4);
}

OT_TEST(cancel_all_reports_only_what_the_gateway_accepted) {
    Rig r;
    for (int k = 0; k < 5; ++k) live_order(r, Side::buy, kPx, 10);
    r.gw.budget = 2;
    OT_CHECK_EQ(r.om.cancel_all(10), std::size_t{2});
    OT_STATE(r.om, 1, "pending_cancel");
    OT_STATE(r.om, 2, "pending_cancel");
    OT_STATE(r.om, 3, "live");
    OT_STATE(r.om, 5, "live");
    r.gw.set_open(true);
    OT_CHECK_EQ(r.om.cancel_all(11), std::size_t{3});  // the retry finishes the job
    for (oms::OrderId id = 1; id <= 5; ++id) OT_STATE(r.om, id, "pending_cancel");
    Rig empty;
    OT_CHECK_EQ(empty.om.cancel_all(1), std::size_t{0});
}

// ---------------------------------------------------------------------------------------------
// listener behaviour
// ---------------------------------------------------------------------------------------------

OT_TEST(manager_works_without_a_listener) {
    Rig r(wide(), 16, false);
    live_order(r, Side::buy, kPx, 100);
    r.om.on_executed(exe(1, 40, kPx), 10);
    OT_SUBMIT(r.om.replace(1, kPx, 150, 11), "ok");
    r.om.on_replaced(rpl(1, 2, 110, kPx), 12);
    OT_SUBMIT(r.om.cancel(1, 13), "ok");
    r.om.on_canceled(can(2, 110), 14);
    OT_STATE(r.om, 1, "canceled");
    OT_CHECK_EQ(r.risk.position(kAapl).qty, std::int64_t{40});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
    OT_CHECK(r.lis.updates.empty());  // never attached
}

OT_TEST(listener_may_act_on_the_order_from_inside_a_callback) {
    Rig r;
    live_order(r, Side::buy, kPx, 100);
    bool cancelled = false;
    r.lis.on_fill_hook = [&](const oms::Fill& f) {
        if (!cancelled) {
            cancelled = true;
            OT_SUBMIT(r.om.cancel(f.id, 200), "ok");
        }
    };
    r.om.on_executed(exe(1, 40, kPx), 100);
    OT_STATE(r.om, 1, "pending_cancel");
    OT_CHECK_EQ(r.gw.cancels.size(), std::size_t{1});
    // The last update the listener saw is the freshest state, not the state at the fill.
    OT_CHECK_EQ(std::string_view(name(r.lis.updates.back().status)), std::string_view("pending_cancel"));
    OT_CHECK_EQ(r.lis.updates.back().cum_qty, Qty{40});

    // A callback may also submit a new order, including from the update of a terminal order.
    r.lis.on_fill_hook = nullptr;
    r.lis.on_update_hook = [&](const oms::OrderInfo& i) {
        if (i.status == oms::OrderStatus::canceled && r.om.orders_submitted() == 1) {
            OT_SUBMIT(r.om.submit(req(kAapl, Side::sell, kPx, 5), 300).status, "ok");
        }
    };
    r.om.on_canceled(can(1, 60), 250);
    OT_STATE(r.om, 1, "canceled");
    OT_CHECK_EQ(r.om.orders_submitted(), std::size_t{2});
    OT_STATE(r.om, 2, "pending_new");
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::sell), std::int64_t{5});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
}

// ---------------------------------------------------------------------------------------------
// Regressions found by review: open-order count, races, repeats, amend limits, rate budget,
// capacity, risk-reducing orders
// ---------------------------------------------------------------------------------------------

// A far order rests for good while another is sent and cancelled over and over. The count of
// working orders must stay at one (it used to creep up by one per cycle until max_open_orders
// refused everything).
OT_TEST(cancel_and_resubmit_cycles_do_not_inflate_the_open_order_count) {
    risk::Limits l = wide();
    l.max_open_orders = 10;
    Rig r(l, 64);
    live_order(r, Side::buy, kPx, 100);  // token 1, rests forever
    for (int k = 0; k < 200; ++k) {
        const auto res = r.om.submit(req(kAapl, Side::buy, kPx, 100), 10 + k);
        OT_SUBMIT(res.status, "ok");
        if (res.status != oms::SubmitStatus::ok) return;
        const std::uint64_t t = token_id(r.gw.enters.back().token);
        r.om.on_accepted(acc(t), 11 + k);
        OT_SUBMIT(r.om.cancel(res.id, 12 + k), "ok");
        OT_CHECK_EQ(r.risk.open_orders(), 2u);
        r.om.on_canceled(can(t, 100), 13 + k);
        OT_CHECK_EQ(r.risk.open_orders(), 1u);
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{100});
    }
}

// Every way an order can end takes exactly one off the count, however much of it was filled,
// cancelled or amended before.
OT_TEST(open_order_count_is_exact_through_every_way_an_order_ends) {
    Rig r(wide(), 64);
    live_order(r, Side::buy, kPx, 100);   // 1: several orders on one side, so the count is not
    live_order(r, Side::buy, kPx, 100);   // 2: derivable from the quantity
    live_order(r, Side::buy, kPx, 100);   // 3
    live_order(r, Side::buy, kPx, 100);   // 4
    live_order(r, Side::buy, kPx, 100);   // 5
    OT_CHECK_EQ(r.risk.open_orders(), 5u);

    r.om.on_executed(exe(1, 40, kPx), 10);               // partial fill: still working
    r.om.on_canceled(can(2, 30), 10);                    // partial cancel: still working
    OT_CHECK_EQ(r.risk.open_orders(), 5u);
    r.om.on_executed(exe(1, 60, kPx), 11);               // 1 filled
    OT_CHECK_EQ(r.risk.open_orders(), 4u);
    r.om.on_canceled(can(2, 70), 12);                    // 2 cancelled
    OT_CHECK_EQ(r.risk.open_orders(), 3u);
    r.om.on_accepted(acc(99), 12);                       // unknown token: nothing
    OT_SUBMIT(r.om.replace(3, kPx, 250, 13), "ok");      // growth in flight: still one order
    OT_CHECK_EQ(r.risk.open_orders(), 3u);
    r.om.on_replaced(rpl(3, token_id(r.gw.replaces.back().replacement), 250, kPx), 14);
    OT_CHECK_EQ(r.risk.open_orders(), 3u);
    r.om.on_canceled(can(token_id(r.gw.replaces.back().replacement), 250), 15);  // 3 cancelled
    OT_CHECK_EQ(r.risk.open_orders(), 2u);
    r.om.on_accepted(acc(4, 1, 'D'), 16);                // 4 was accepted already: dropped
    OT_CHECK_EQ(r.risk.open_orders(), 2u);
    const auto dead = r.om.submit(req(kAapl, Side::buy, kPx, 10), 17);  // 6, dies on arrival
    r.om.on_accepted(acc(token_id(r.gw.enters.back().token), 1, 'D'), 18);
    OT_STATE(r.om, dead.id, "canceled");
    OT_CHECK_EQ(r.risk.open_orders(), 2u);
    const auto bad = r.om.submit(req(kAapl, Side::buy, kPx, 10), 19);   // 7, refused
    r.om.on_rejected(rej(token_id(r.gw.enters.back().token)), 20);
    OT_STATE(r.om, bad.id, "rejected");
    OT_CHECK_EQ(r.risk.open_orders(), 2u);
    OT_CHECK_EQ(r.om.open_orders(), std::size_t{2});
    // The two left are 4 and 5, with all their quantity.
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{200});
}

// The exchange reopens (total - executed) on a replace even when a partial cancel shrank the
// order first. That is a fact about the exchange's book: the OMS follows it, and the executions
// that then arrive all reach the position.
OT_TEST(replace_racing_a_partial_cancel_keeps_every_later_fill) {
    Rig r(wide(), 64);
    const oms::OrderId id = live_order(r, Side::buy, kPx, 100);  // token 1
    OT_SUBMIT(r.om.replace(id, kPx, 95, 3), "ok");               // shrink, token 2 in flight
    r.om.on_canceled(can(1, 71), 4);                             // before the exchange saw it
    OT_CHECK_EQ(r.om.find(id)->leaves_qty, Qty{29});
    // The ledger still covers what the replace may reopen: 95 - 0.
    OT_CHECK_EQ(r.om.reserved_qty(id), Qty{95});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{95});

    r.om.on_replaced(rpl(1, 2, 95, kPx), 5);                     // exchange: open = 95
    OT_STATE(r.om, id, "live");
    OT_CHECK_EQ(r.om.find(id)->leaves_qty, Qty{95});
    OT_CHECK_EQ(r.om.reserved_qty(id), Qty{95});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{95});
    OT_CHECK_EQ(r.risk.open_orders(), 1u);

    r.om.on_executed(exe(2, 60, kPx), 6);
    r.om.on_executed(exe(2, 35, kPx), 7);
    OT_STATE(r.om, id, "filled");
    OT_CHECK_EQ(r.om.find(id)->cum_qty, Qty{95});
    OT_CHECK_EQ(r.risk.position(kAapl).qty, std::int64_t{95});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
    OT_CHECK_EQ(r.risk.open_orders(), 0u);
    OT_CHECK_EQ(r.om.stats().clamped_reports, std::uint64_t{0});
    OT_CHECK_EQ(r.om.stats().invalid_reports, std::uint64_t{0});
}

// Same race with a growing replace, and with a cancel that empties the order while the replace
// is in flight (the replace then finds nothing and the exchange refuses it).
OT_TEST(replace_races_keep_the_ledger_on_the_exchanges_numbers) {
    {   // grow 100 -> 150, exchange partial-cancels 40 first, then reopens 150
        Rig r;
        const oms::OrderId id = live_order(r, Side::buy, kPx, 100);
        OT_SUBMIT(r.om.replace(id, kPx, 150, 3), "ok");
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{150});
        r.om.on_canceled(can(1, 40), 4);
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{150});  // floor: the reopen
        r.om.on_replaced(rpl(1, 2, 150, kPx), 5);
        OT_CHECK_EQ(r.om.find(id)->leaves_qty, Qty{150});
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{150});
    }
    {   // the replace is refused after a partial cancel: only what really stays open is held
        Rig r;
        const oms::OrderId id = live_order(r, Side::buy, kPx, 100);
        OT_SUBMIT(r.om.replace(id, kPx, 150, 3), "ok");
        r.om.on_canceled(can(1, 40), 4);
        r.om.on_rejected(rej(2), 5);
        OT_STATE(r.om, id, "live");
        OT_CHECK_EQ(r.om.find(id)->leaves_qty, Qty{60});
        OT_CHECK_EQ(r.om.reserved_qty(id), Qty{60});
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{60});
    }
    {   // fills during the flight reduce the floor too
        Rig r;
        const oms::OrderId id = live_order(r, Side::buy, kPx, 100);
        OT_SUBMIT(r.om.replace(id, kPx, 150, 3), "ok");
        r.om.on_executed(exe(1, 30, kPx), 4);
        OT_CHECK_EQ(r.om.reserved_qty(id), Qty{120});  // 150 - 30
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{120});
        r.om.on_replaced(rpl(1, 2, 120, kPx), 5);
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{120});
        OT_CHECK_EQ(r.risk.position(kAapl).qty, std::int64_t{30});
    }
    {   // the order is emptied while the replace is in flight
        Rig r;
        const oms::OrderId id = live_order(r, Side::buy, kPx, 100);
        OT_SUBMIT(r.om.replace(id, kPx, 150, 3), "ok");
        r.om.on_canceled(can(1, 100), 4);
        OT_STATE(r.om, id, "canceled");
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
        OT_CHECK_EQ(r.risk.open_orders(), 0u);
        r.om.on_rejected(rej(2), 5);  // the exchange refuses the replace of a dead order
        OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
    }
}

// A replace is judged on the new open quantity and price, not only on the added shares. One
// limit per rig, so each check can only be refused by the limit it is about.
OT_TEST(replace_cannot_bypass_the_size_limit) {
    risk::Limits l = wide();
    l.max_order_qty = 1000;
    Rig r(l);
    const oms::OrderId id = live_order(r, Side::buy, kPx, 1000);  // token 1
    const std::uint64_t before = fingerprint(r);
    // Growth of 500 fits max_order_qty on its own; the new open quantity 1500 does not.
    OT_SUBMIT(r.om.replace(id, kPx, 1500, 10), "rejected_by_risk");
    // Nor does a replace that adds nothing get to exceed it: 1001 only after executions.
    OT_SUBMIT(r.om.replace(id, kPx, 1001, 11), "rejected_by_risk");
    OT_CHECK_EQ(fingerprint(r), before);
    OT_CHECK_EQ(r.gw.replaces.size(), std::size_t{0});
    OT_SUBMIT(r.om.replace(id, kPx, 1000, 12), "ok");
    r.om.on_replaced(rpl(1, 2, 1000, kPx), 13);
    // The size limit sees what is open after the executions: 1400 total, 400 done, 1000 open.
    r.om.on_executed(exe(2, 400, kPx), 14);
    OT_SUBMIT(r.om.replace(id, kPx, 1400, 15), "ok");
    OT_CHECK_EQ(r.gw.replaces.size(), std::size_t{2});
}

OT_TEST(replace_cannot_bypass_the_notional_limit) {
    risk::Limits l = wide();
    l.max_order_notional = 1000 * kPx;  // 1000 shares at 100.0000
    Rig r(l);
    const oms::OrderId id = live_order(r, Side::buy, kPx, 1000);
    const std::uint64_t before = fingerprint(r);
    OT_SUBMIT(r.om.replace(id, kPx + 1, 1000, 10), "rejected_by_risk");  // no growth, dearer
    OT_SUBMIT(r.om.replace(id, kPx, 1001, 11), "rejected_by_risk");      // growth
    OT_CHECK_EQ(fingerprint(r), before);
    OT_SUBMIT(r.om.replace(id, kPx + 1000, 990, 12), "ok");              // 990 * 100.1 is under
    OT_CHECK_EQ(r.gw.replaces.size(), std::size_t{1});
}

OT_TEST(replace_cannot_bypass_the_price_band) {
    risk::Limits l = wide();
    l.price_band_bps = 100;  // 1 %
    Rig r(l);
    r.ref.px[kAapl] = kPx;
    const oms::OrderId id = live_order(r, Side::buy, kPx, 100);
    const std::uint64_t before = fingerprint(r);
    OT_SUBMIT(r.om.replace(id, 1'020'000, 100, 10), "rejected_by_risk");  // re-price only
    OT_SUBMIT(r.om.replace(id, 1'020'000, 50, 11), "rejected_by_risk");   // shrink and re-price
    OT_CHECK_EQ(fingerprint(r), before);
    OT_SUBMIT(r.om.replace(id, 1'010'000, 50, 12), "ok");                 // exactly at the band
}

// Amending while the book is halted or full: only what adds exposure is refused.
OT_TEST(replace_that_adds_nothing_is_exempt_from_kill_switch_rate_and_open_order_limits) {
    risk::Limits l = wide();
    l.max_orders_per_second = 2;
    l.max_open_orders = 1;
    Rig r(l);
    const oms::OrderId id = live_order(r, Side::buy, kPx, 100);  // admission 1 of 2, open 1 of 1
    // A growing replace is not a new order: it is not refused for max_open_orders, but it does
    // spend rate budget (admission 2).
    OT_SUBMIT(r.om.replace(id, kPx, 120, 10), "ok");
    r.om.on_replaced(rpl(1, 2, 120, kPx), 11);
    OT_REJECT(r.risk.check(kAapl, Side::buy, kPx, 1, 0, 12), risk::Reject::rate_limit);
    // A shrink or re-price adds nothing: no budget, no kill switch.
    r.risk.set_kill_switch(true);
    OT_SUBMIT(r.om.replace(id, kPx + 1000, 110, 13), "ok");
    r.om.on_replaced(rpl(2, 3, 110, kPx + 1000), 14);
    OT_SUBMIT(r.om.replace(id, kPx, 110, 15), "ok");
    // ...but growing is refused by the kill switch.
    r.om.on_replaced(rpl(3, 4, 110, kPx), 16);
    OT_SUBMIT(r.om.replace(id, kPx, 111, 17), "rejected_by_risk");
}

// The gateway refusing a message after the risk verdict must not cost rate budget.
OT_TEST(a_send_the_gateway_refuses_does_not_spend_rate_budget) {
    risk::Limits l = wide();
    l.max_orders_per_second = 2;
    Rig r(l);
    r.gw.set_open(false);
    for (int k = 0; k < 10; ++k) {
        OT_SUBMIT(r.om.submit(req(kAapl, Side::buy, kPx, 1), 100 + k).status, "gateway_busy");
    }
    r.gw.set_open(true);
    OT_SUBMIT(r.om.submit(req(kAapl, Side::buy, kPx, 1), 200).status, "ok");
    OT_SUBMIT(r.om.submit(req(kAapl, Side::buy, kPx, 1), 201).status, "ok");
    OT_SUBMIT(r.om.submit(req(kAapl, Side::buy, kPx, 1), 202).status, "rejected_by_risk");  // now it is full

    // A growing replace the gateway refuses is free too.
    Rig q(l);
    const oms::OrderId id = live_order(q, Side::buy, kPx, 10);  // admission 1 of 2
    q.gw.set_open(false);
    for (int k = 0; k < 10; ++k) OT_SUBMIT(q.om.replace(id, kPx, 20, 300 + k), "gateway_busy");
    q.gw.set_open(true);
    OT_SUBMIT(q.om.replace(id, kPx, 20, 400), "ok");  // admission 2 of 2
    OT_REJECT(q.risk.check(kAapl, Side::buy, kPx, 1, 0, 401), risk::Reject::rate_limit);
}

// Finished orders give their slots back. The table is sized for 4 working orders here but many
// thousands pass through it.
OT_TEST(finished_orders_give_their_slots_back) {
    Rig r(wide(), 4);
    std::uint64_t token = 0;
    for (oms::OrderId k = 1; k <= 5000; ++k) {
        const auto res = r.om.submit(req(kAapl, Side::buy, kPx, 10), k);
        OT_SUBMIT(res.status, "ok");
        if (res.status != oms::SubmitStatus::ok) return;
        OT_CHECK_EQ(res.id, k);  // ids keep counting and are never reused
        const std::uint64_t t = token_id(r.gw.enters.back().token);
        OT_CHECK(t > token);  // nor are tokens
        token = t;
        r.om.on_accepted(acc(t), k);
        r.om.on_executed(exe(t, 10, kPx), k);
        OT_STATE(r.om, k, "filled");
    }
    OT_CHECK_EQ(r.om.orders_submitted(), std::size_t{5000});
    OT_CHECK_EQ(r.risk.position(kAapl).qty, std::int64_t{50'000});
    OT_CHECK_EQ(r.risk.open_orders(), 0u);
    OT_CHECK(r.om.find(1) == nullptr);       // long gone
    OT_CHECK(r.om.find(5000) != nullptr);    // the newest finished orders are kept
    OT_CHECK(r.om.find(4997) != nullptr);
    OT_CHECK(r.om.find(5001) == nullptr);
}

// The default table (65536 slots) used to answer `capacity` for good after that many orders.
OT_TEST(default_capacity_is_not_a_lifetime_limit) {
    Rig r(wide(), oms::OrderManager::Config{}.max_orders, false);
    const std::size_t total = oms::OrderManager::Config{}.max_orders + 5000;
    for (std::size_t k = 1; k <= total; ++k) {
        const auto res = r.om.submit(req(kAapl, Side::buy, kPx, 1), k);
        if (res.status != oms::SubmitStatus::ok) {
            OT_SUBMIT(res.status, "ok");
            return;
        }
        const std::uint64_t t = token_id(r.gw.enters.back().token);
        r.om.on_accepted(acc(t), k);
        r.om.on_executed(exe(t, 1, kPx), k);
    }
    OT_CHECK_EQ(r.risk.position(kAapl).qty, static_cast<std::int64_t>(total));
}

// A slot is taken from the oldest finished order; working orders are never recycled, however
// old, and a recycled order is gone for every purpose.
OT_TEST(recycling_takes_the_oldest_finished_order_and_never_a_working_one) {
    Rig r(wide(), 3);
    const oms::OrderId a = live_order(r, Side::buy, kPx, 10);   // id 1 token 1: rests forever
    const oms::OrderId b = live_order(r, Side::buy, kPx, 10);   // id 2 token 2
    const oms::OrderId c = live_order(r, Side::buy, kPx, 10);   // id 3 token 3
    OT_SUBMIT(r.om.submit(req(kAapl, Side::buy, kPx, 10), 50).status, "capacity");
    r.om.on_executed(exe(3, 10, kPx), 51);                       // c finishes first
    r.om.on_executed(exe(2, 10, kPx), 52);                       // then b
    OT_CHECK(r.om.find(b) != nullptr && r.om.find(c) != nullptr);

    const auto d = r.om.submit(req(kAapl, Side::sell, kPx + 5, 7), 53);  // takes c's slot
    OT_SUBMIT(d.status, "ok");
    OT_CHECK_EQ(d.id, oms::OrderId{4});
    OT_CHECK(r.om.find(c) == nullptr);
    OT_CHECK(r.om.find(b) != nullptr);
    OT_CHECK(r.om.find(a) != nullptr);
    const oms::OrderInfo* di = r.om.find(d.id);
    OT_CHECK(di != nullptr && di->req.side == Side::sell && di->req.qty == 7 && di->cum_qty == 0);
    OT_CHECK(di != nullptr && di->id == 4 && di->status == oms::OrderStatus::pending_new);
    OT_CHECK(r.gw.enters.back().token == tok(4));

    // The recycled order: every call answers unknown, every late report is counted unknown and
    // changes nothing, in particular not the order that now owns the slot.
    OT_SUBMIT(r.om.cancel(c, 60), "unknown_order");
    OT_SUBMIT(r.om.replace(c, kPx, 20, 60), "unknown_order");
    OT_CHECK_EQ(r.om.reserved_qty(c), Qty{0});
    const std::uint64_t before = fingerprint(r);
    const std::uint64_t unknown = r.om.stats().unknown_tokens;
    r.om.on_executed(exe(3, 10, kPx), 61);
    r.om.on_canceled(can(3, 10), 61);
    r.om.on_accepted(acc(3), 61);
    r.om.on_rejected(rej(3), 61);
    r.om.on_replaced(rpl(3, 99, 10, kPx), 61);
    OT_CHECK_EQ(r.om.stats().unknown_tokens, unknown + 5);
    OT_CHECK_EQ(fingerprint(r), before);
    // b is finished but still tabled: its late reports are "late", not "unknown".
    r.om.on_executed(exe(2, 10, kPx), 62);
    OT_CHECK_EQ(r.om.stats().late_reports, std::uint64_t{1});

    // The working order is untouched by all of this and is cancelled in id order with the rest.
    OT_CHECK_EQ(r.om.cancel_all(70), std::size_t{2});  // ids 1 and 4; b is finished
    OT_CHECK(r.gw.cancels.size() == 2 && r.gw.cancels[0].token == tok(1) && r.gw.cancels[1].token == tok(4));
    r.om.on_canceled(can(4, 7), 71);
    r.om.on_canceled(can(1, 10), 71);
    OT_CHECK_EQ(r.risk.open_orders(), 0u);
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::sell), std::int64_t{0});
}

// The token table keeps up with recycling: it never fills with the tokens of forgotten orders,
// replacements included.
OT_TEST(token_table_survives_recycling_with_replaces) {
    Rig r(wide(), 4);
    for (oms::OrderId k = 1; k <= 3000; ++k) {
        OT_SUBMIT(r.om.submit(req(kAapl, Side::buy, kPx, 10), k).status, "ok");
        const std::uint64_t t = token_id(r.gw.enters.back().token);
        r.om.on_accepted(acc(t), k);
        OT_SUBMIT(r.om.replace(k, kPx, 12, k), "ok");
        const std::uint64_t t2 = token_id(r.gw.replaces.back().replacement);
        r.om.on_replaced(rpl(t, t2, 12, kPx), k);
        OT_SUBMIT(r.om.replace(k, kPx, 9, k), "ok");  // left in flight, then the order ends
        r.om.on_executed(exe(t2, 12, kPx), k);
        OT_STATE(r.om, k, "filled");
    }
    OT_CHECK_EQ(r.risk.open_orders(), 0u);
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
    OT_CHECK_EQ(r.risk.position(kAapl).qty, std::int64_t{3000 * 12});
}

OT_TEST(first_token_is_configurable) {
    RecordingGateway gw;
    FakeReference ref;
    FakeSymbols syms;
    RecordingListener lis;
    risk::RiskEngine risk(wide(), 8);
    oms::OrderManager::Config c;
    c.max_orders = 8;
    c.first_token = 500;
    oms::OrderManager om(c, gw, risk, ref, syms, &lis);
    const auto a = om.submit(req(kAapl, Side::buy, kPx, 10), 1);
    OT_SUBMIT(a.status, "ok");
    OT_CHECK_EQ(a.id, oms::OrderId{1});  // ids are independent of tokens
    OT_CHECK(gw.enters[0].token == tok(500));
    om.on_accepted(acc(1), 2);  // the old numbering is a stranger now
    OT_CHECK_EQ(om.stats().unknown_tokens, std::uint64_t{1});
    om.on_accepted(acc(500), 3);
    OT_STATE(om, 1, "live");
    OT_SUBMIT(om.replace(1, kPx, 20, 4), "ok");
    OT_CHECK(gw.replaces[0].replacement == tok(501));

    // 0 is not a usable start; the default is 1.
    oms::OrderManager::Config z;
    z.first_token = 0;
    OT_CHECK_EQ(oms::OrderManager::Config{}.first_token, std::uint64_t{1});
    RecordingGateway gw2;
    oms::OrderManager om2(z, gw2, risk, ref, syms, nullptr);
    OT_SUBMIT(om2.submit(req(kAapl, Side::buy, kPx, 1), 1).status, "ok");
    OT_CHECK(gw2.enters[0].token == tok(1));
}

// Orders that shrink an over-limit position are not blocked by the position limit.
OT_TEST(orders_that_reduce_an_over_limit_position_are_allowed) {
    risk::Limits l = wide();
    l.max_position = 100;
    Rig r(l);
    r.risk.on_fill(kAapl, Side::buy, 150, kPx);  // fills landed beyond the cap
    OT_SUBMIT(r.om.submit(req(kAapl, Side::sell, kPx, 40), 1).status, "ok");
    const auto more = r.om.submit(req(kAapl, Side::buy, kPx, 1), 2);
    OT_SUBMIT(more.status, "rejected_by_risk");
    OT_REJECT(more.risk_reason, risk::Reject::position);
}

// A listener that submits from on_fill of a fill that finished its order may be handed that
// order's slot. The update that follows must still describe the finished order.
OT_TEST(a_submit_from_the_fill_callback_may_take_the_slot_of_the_order_that_just_finished) {
    Rig r(wide(), 1);
    live_order(r, Side::buy, kPx, 10);
    r.lis.on_fill_hook = [&](const oms::Fill&) {
        r.lis.on_fill_hook = nullptr;
        OT_SUBMIT(r.om.submit(req(kAapl, Side::sell, kPx, 5), 200).status, "ok");
    };
    r.om.on_executed(exe(1, 10, kPx), 100);
    OT_CHECK(r.om.find(1) == nullptr);  // its slot now belongs to order 2
    OT_STATE(r.om, 2, "pending_new");
    OT_CHECK_EQ(r.om.find(2)->req.qty, Qty{5});
    OT_CHECK(!r.lis.updates.empty());
    OT_CHECK_EQ(r.lis.updates.back().id, oms::OrderId{1});
    OT_CHECK_EQ(std::string_view(name(r.lis.updates.back().status)), std::string_view("filled"));
    OT_CHECK_EQ(r.lis.updates.back().cum_qty, Qty{10});
    OT_CHECK_EQ(r.risk.position(kAapl).qty, std::int64_t{10});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::sell), std::int64_t{5});
    OT_CHECK_EQ(r.risk.open_qty(kAapl, Side::buy), std::int64_t{0});
    OT_CHECK_EQ(r.risk.open_orders(), 1u);
}

// ---------------------------------------------------------------------------------------------
// Randomized play
// ---------------------------------------------------------------------------------------------

namespace {

constexpr std::size_t kRandomOrders = 12;  // small, so finished slots are recycled again and again

// Coverage of the random play: each entry counts how often a situation was reached, so the test
// can insist that every path it is meant to exercise really was exercised.
enum Hit : std::size_t {
    h_submit_ok, h_submit_risk, h_submit_busy, h_submit_invalid, h_submit_capacity,
    h_cancel_ok, h_cancel_during_replace, h_cancel_bad_state, h_cancel_unknown,
    h_replace_ok, h_replace_growth, h_replace_risk, h_replace_busy, h_replace_bad_state,
    h_replace_invalid, h_cancel_all,
    h_accept, h_accept_dead, h_accept_late, h_reject, h_exec, h_exec_clamped, h_exec_completes,
    h_exec_before_ack, h_cancel_report, h_cancel_partial, h_cancel_full, h_replaced,
    h_replaced_dead, h_replaced_cut, h_replaced_while_cancelling, h_replace_rejected,
    h_illegal_0, h_illegal_1, h_illegal_2, h_illegal_3, h_illegal_4, h_illegal_5, h_illegal_6,
    h_illegal_7, h_illegal_8, h_illegal_9, h_illegal_10, h_count
};
constexpr const char* kHitNames[h_count] = {
    "submit ok", "submit risk", "submit busy", "submit invalid", "submit capacity",
    "cancel ok", "cancel during replace", "cancel bad_state", "cancel unknown",
    "replace ok", "replace growth", "replace risk", "replace busy", "replace bad_state",
    "replace invalid", "cancel_all",
    "accepted", "accepted dead", "accepted after a fill", "rejected", "executed", "executed clamped",
    "executed completes", "executed before ack", "canceled", "canceled partial", "canceled full",
    "replaced", "replaced dead", "replaced cut", "replaced while cancelling", "replace rejected",
    "illegal unknown token", "illegal spelling", "illegal late", "illegal second accepted",
    "illegal reject of held order", "illegal execution", "illegal zero cancel",
    "illegal accepted state", "illegal replaced", "illegal on replacement token",
    "illegal repeated execution"};

// Drives one manager with a stream of random calls and reports, checking the ledger invariants
// after every step. `allow_growth` = false confines replaces to sizes the order already holds, so
// the strict invariant "risk open quantity == sum of leaves" applies at every step; with growth
// the ledger holds the reserved extra as well and is checked against the manager's reservation,
// which is itself bounded by what the test knows (>= leaves, == leaves when nothing is pending).
class Scenario {
public:
    Scenario(std::uint64_t seed, bool allow_growth)
        : rng_(seed), grow_(allow_growth), rig_(limits(), kRandomOrders) {
        rig_.ref.px[kAapl] = kPx;
        rig_.ref.px[kMsft] = kPx;  // GOOG has no reference: the band is skipped for it
    }

    std::uint64_t run(int steps) {
        for (int s = 0; s < steps; ++s) {
            now_ += 1 + rng_.bounded(5000);
            step();
            check_all();
        }
        return digest();
    }

    const std::array<std::uint64_t, h_count>& hits() const { return hits_; }
    std::uint64_t applied_reports() const { return applied_; }
    std::uint64_t dropped_reports() const { return dropped_; }
    std::size_t orders() const { return rig_.om.orders_submitted(); }

private:
    struct Shadow {  // what the test knows about the exchange's view of one order
        std::uint64_t tok{};
        std::uint64_t pend_tok{};
        Qty pend_qty{};
        Price pend_px{};
        bool accepted{};
        bool acked{};
        std::uint64_t last_match{};  // the last execution booked, to replay it as a duplicate
        Qty last_shares{};
        Price last_px{};
    };
    struct Tally {  // fills as the listener saw them
        std::int64_t qty{};
        std::uint64_t notional{};
    };

    static risk::Limits limits() {
        risk::Limits l;
        l.max_order_qty = 200;
        l.max_position = 600;
        l.price_band_bps = 300;
        return l;
    }

    void hit(Hit h) { ++hits_[h]; }

    oms::OrderManager& om() { return rig_.om; }
    const oms::OrderInfo& info(oms::OrderId id) { return *rig_.om.find(id); }  // id must be tabled
    bool tabled(oms::OrderId id) { return rig_.om.find(id) != nullptr; }
    Shadow& shadow(oms::OrderId id) {
        if (shadow_.size() <= id) shadow_.resize(id + 1);
        return shadow_[id];
    }

    Price random_price() { return kPx + rng_.range(-40'000, 40'000); }

    // A random order id, biased by `want_open`: -1 any, 1 non-terminal, 0 terminal. 0 if none found.
    oms::OrderId pick(int want_open) {
        const std::size_t n = om().orders_submitted();
        if (n == 0) return 0;
        for (int tries = 0; tries < 12; ++tries) {
            const oms::OrderId id = 1 + rng_.bounded(n);
            // An order whose slot was reused counts as finished (its id is gone, not its token).
            const bool open = tabled(id) && !oms::is_terminal(info(id).status);
            if (want_open < 0 || open == (want_open == 1)) return id;
        }
        return 0;
    }

    void step() {
        const std::uint64_t p = rng_.bounded(100);
        if (p < 15) return do_submit();
        if (p < 20) return do_cancel();
        if (p < 28) return do_replace();
        if (p < 29) return do_cancel_all();
        if (p < 32) {
            gw_open_ = rng_.chance(85, 100);
            rig_.gw.set_open(gw_open_);
            return;
        }
        if (p < 34) {
            rig_.risk.set_kill_switch(rng_.chance(15, 100));
            return;
        }
        if (rng_.chance(78, 100)) return legal_report(); else return illegal_report();
    }

    // ---- actions ------------------------------------------------------------------------

    void do_submit() {
        const std::uint64_t before = fingerprint(rig_);
        const std::size_t n = om().orders_submitted();
        const bool table_full = om().open_orders() == kRandomOrders;  // every slot holds a working order
        oms::OrderRequest q = req(static_cast<Locate>(1 + rng_.bounded(3)),
                                  rng_.chance(1, 2) ? Side::buy : Side::sell, random_price(),
                                  static_cast<Qty>(1 + rng_.bounded(220)),
                                  rng_.chance(1, 5) ? oms::Tif::ioc : oms::Tif::day);
        bool malformed = false;
        if (rng_.chance(1, 12)) {
            malformed = true;
            switch (rng_.bounded(4)) {
                case 0: q.qty = 0; break;
                case 1: q.price = 0; break;
                case 2: q.qty = kMaxOrderQty + 1; break;
                default: q.locate = 9; break;  // no symbol
            }
        }
        const auto res = om().submit(q, now_);
        if (res.status == oms::SubmitStatus::ok) {
            OT_CHECK(!malformed);
            OT_CHECK_EQ(res.id, static_cast<oms::OrderId>(n + 1));
            OT_CHECK_EQ(om().orders_submitted(), n + 1);
            const ouch::EnterOrder& m = rig_.gw.enters.back();
            OT_CHECK_EQ(m.shares, q.qty);
            OT_CHECK_EQ(m.price, q.price);
            OT_CHECK(m.side == q.side);
            OT_CHECK_EQ(m.time_in_force, q.tif == oms::Tif::ioc ? ouch::kTifIoc : ouch::kTifSystemHours);
            Shadow& sh = shadow(res.id);
            sh = Shadow{};
            sh.tok = token_id(m.token);
            OT_STATE(om(), res.id, "pending_new");
            OT_CHECK_EQ(info(res.id).leaves_qty, q.qty);
            hit(h_submit_ok);
        } else {
            switch (res.status) {
                case oms::SubmitStatus::rejected_by_risk: hit(h_submit_risk); break;
                case oms::SubmitStatus::gateway_busy: hit(h_submit_busy); break;
                case oms::SubmitStatus::invalid_request: hit(h_submit_invalid); break;
                case oms::SubmitStatus::capacity: hit(h_submit_capacity); break;
                default: OT_CHECK(false); break;
            }
            OT_CHECK_EQ(res.id, oms::OrderId{0});
            OT_CHECK_EQ(fingerprint(rig_), before);
            if (malformed) OT_SUBMIT(res.status, "invalid_request");
            if (!malformed && table_full) OT_SUBMIT(res.status, "capacity");
            if (!malformed && !table_full) OT_CHECK(res.status != oms::SubmitStatus::capacity);
            if (!malformed && !table_full && !gw_open_ && res.status != oms::SubmitStatus::gateway_busy) {
                OT_SUBMIT(res.status, "rejected_by_risk");
            }
        }
    }

    void do_cancel() {
        const oms::OrderId id = rng_.chance(1, 20) ? om().orders_submitted() + 1 : pick(-1);
        if (id == 0) return;
        const std::uint64_t before = fingerprint(rig_);
        const bool exists = om().find(id) != nullptr;
        const oms::OrderInfo pre = exists ? info(id) : oms::OrderInfo{};
        const auto s = om().cancel(id, now_);
        if (!exists) {
            OT_SUBMIT(s, "unknown_order");
            hit(h_cancel_unknown);
        } else if (oms::is_terminal(pre.status) || pre.status == oms::OrderStatus::pending_cancel) {
            OT_SUBMIT(s, "bad_state");
            hit(h_cancel_bad_state);
        } else if (!gw_open_) {
            OT_SUBMIT(s, "gateway_busy");
        } else {
            OT_SUBMIT(s, "ok");
            hit(h_cancel_ok);
            if (pre.status == oms::OrderStatus::pending_replace) hit(h_cancel_during_replace);
            OT_STATE(om(), id, "pending_cancel");
            const Shadow& sh = shadow(id);
            const std::uint64_t want = pre.status == oms::OrderStatus::pending_replace ? sh.pend_tok : sh.tok;
            OT_CHECK(rig_.gw.cancels.back().token == tok(want));
            OT_CHECK_EQ(rig_.gw.cancels.back().shares, Qty{0});
            OT_CHECK_EQ(info(id).leaves_qty, pre.leaves_qty);
            return;
        }
        OT_CHECK_EQ(fingerprint(rig_), before);
    }

    void do_replace() {
        const oms::OrderId id = rng_.chance(1, 20) ? om().orders_submitted() + 1 : pick(1);
        if (id == 0) return;
        const std::uint64_t before = fingerprint(rig_);
        const bool exists = om().find(id) != nullptr;
        const oms::OrderInfo pre = exists ? info(id) : oms::OrderInfo{};
        Qty new_qty;
        if (!exists) {
            new_qty = 10;
        } else if (grow_) {
            new_qty = static_cast<Qty>(rng_.bounded(260));
        } else {
            // Stay within the size the order already carries: no growth reservation.
            const Qty room = pre.leaves_qty;
            new_qty = rng_.chance(1, 8) ? static_cast<Qty>(rng_.bounded(pre.cum_qty + 1))  // at/below cum
                                        : pre.cum_qty + 1 + static_cast<Qty>(rng_.bounded(room));
        }
        const Price px = random_price();
        const auto s = om().replace(id, px, new_qty, now_);
        if (!exists) {
            OT_SUBMIT(s, "unknown_order");
        } else if (pre.status != oms::OrderStatus::live) {
            OT_SUBMIT(s, "bad_state");
            hit(h_replace_bad_state);
        } else if (new_qty == 0 || new_qty <= pre.cum_qty) {
            OT_SUBMIT(s, "invalid_request");
            hit(h_replace_invalid);
        } else if (s == oms::SubmitStatus::ok) {
            hit(h_replace_ok);
            OT_STATE(om(), id, "pending_replace");
            Shadow& sh = shadow(id);
            const ouch::ReplaceOrder& m = rig_.gw.replaces.back();
            OT_CHECK(m.existing == tok(sh.tok));
            OT_CHECK_EQ(m.shares, new_qty);
            OT_CHECK_EQ(m.price, px);
            sh.pend_tok = token_id(m.replacement);
            sh.pend_qty = new_qty;
            sh.pend_px = px;
            const Qty open_after = new_qty - pre.cum_qty;
            const Qty growth = open_after > pre.leaves_qty ? open_after - pre.leaves_qty : 0;
            if (growth != 0) hit(h_replace_growth);
            OT_CHECK_EQ(om().reserved_qty(id), pre.leaves_qty + growth);
            OT_CHECK_EQ(info(id).leaves_qty, pre.leaves_qty);
            OT_CHECK_EQ(info(id).req.qty, pre.req.qty);
            return;
        } else {
            OT_CHECK(s == oms::SubmitStatus::gateway_busy || s == oms::SubmitStatus::rejected_by_risk);
            hit(s == oms::SubmitStatus::gateway_busy ? h_replace_busy : h_replace_risk);
            if (!gw_open_ && s != oms::SubmitStatus::gateway_busy) {
                OT_SUBMIT(s, "rejected_by_risk");
            }
        }
        OT_CHECK_EQ(fingerprint(rig_), before);
    }

    void do_cancel_all() {
        std::size_t working = 0;
        for (oms::OrderId id = 1; id <= om().orders_submitted(); ++id) {
            if (!tabled(id)) continue;
            const auto st = info(id).status;
            if (!oms::is_terminal(st) && st != oms::OrderStatus::pending_cancel) ++working;
        }
        const std::size_t sent = om().cancel_all(now_);
        OT_CHECK_EQ(sent, gw_open_ ? working : std::size_t{0});
        if (sent != 0) hit(h_cancel_all);
        if (gw_open_) {
            for (oms::OrderId id = 1; id <= om().orders_submitted(); ++id) {
                if (!tabled(id)) continue;
                const auto st = info(id).status;
                OT_CHECK(oms::is_terminal(st) || st == oms::OrderStatus::pending_cancel);
            }
        }
    }

    // ---- legal reports ------------------------------------------------------------------

    enum class Rep { accept, reject, exec, cancel, replaced, reject_replace };

    void legal_report() {
        const oms::OrderId id = pick(1);
        if (id == 0) return;
        const oms::OrderInfo pre = info(id);
        const Shadow sh = shadow(id);
        struct Cand { Rep rep; int weight; };
        std::array<Cand, 6> cands{};
        int n = 0;
        auto add = [&](Rep r, int w) { cands[static_cast<std::size_t>(n++)] = {r, w}; };
        const bool replace_out = sh.pend_tok != 0;
        switch (pre.status) {
            case oms::OrderStatus::pending_new:
                add(Rep::accept, 55); add(Rep::reject, 20); add(Rep::exec, 12); add(Rep::cancel, 8);
                break;
            case oms::OrderStatus::live:
                add(Rep::exec, 60); add(Rep::cancel, 25);
                if (!sh.accepted) add(Rep::accept, 10);
                break;
            case oms::OrderStatus::pending_cancel:
                add(Rep::cancel, 40); add(Rep::exec, 20);
                if (!sh.accepted) add(Rep::accept, 5);
                if (!sh.acked && !replace_out) add(Rep::reject, 10);
                if (replace_out) { add(Rep::replaced, 25); add(Rep::reject_replace, 10); }
                break;
            case oms::OrderStatus::pending_replace:
                add(Rep::replaced, 45); add(Rep::reject_replace, 20); add(Rep::exec, 20);
                add(Rep::cancel, 15);
                if (!sh.accepted) add(Rep::accept, 5);
                break;
            default: return;
        }
        int total = 0;
        for (int k = 0; k < n; ++k) total += cands[static_cast<std::size_t>(k)].weight;
        int roll = static_cast<int>(rng_.bounded(static_cast<std::uint64_t>(total)));
        Rep rep = cands[0].rep;
        for (int k = 0; k < n; ++k) {
            roll -= cands[static_cast<std::size_t>(k)].weight;
            if (roll < 0) { rep = cands[static_cast<std::size_t>(k)].rep; break; }
        }
        const std::uint64_t drops = dropped(om());
        switch (rep) {
            case Rep::accept: report_accept(id, pre); break;
            case Rep::reject: report_reject(id, pre); break;
            case Rep::exec: report_exec(id, pre); break;
            case Rep::cancel: report_cancel(id, pre); break;
            case Rep::replaced: report_replaced(id, pre); break;
            case Rep::reject_replace: report_reject_replace(id, pre); break;
        }
        OT_CHECK_EQ(dropped(om()), drops);  // a legal report is never counted as dropped
        ++applied_;
    }

    static oms::OrderStatus after_ack(oms::OrderStatus s) {
        return s == oms::OrderStatus::pending_new ? oms::OrderStatus::live : s;
    }

    void report_accept(oms::OrderId id, const oms::OrderInfo& pre) {
        Shadow& sh = shadow(id);
        const bool dead = rng_.chance(1, 12);
        hit(dead ? h_accept_dead : h_accept);
        if (pre.status != oms::OrderStatus::pending_new && !sh.acked) hit(h_accept_late);
        const OrderRef ref = 1000 + id;
        om().on_accepted(acc(sh.tok, ref, dead ? 'D' : 'L'), now_);
        sh.accepted = sh.acked = true;
        const oms::OrderInfo& post = info(id);
        OT_CHECK_EQ(post.exchange_ref, ref);
        if (dead) {
            OT_STATE(om(), id, pre.cum_qty >= pre.req.qty ? "filled" : "canceled");
            OT_CHECK_EQ(post.leaves_qty, Qty{0});
        } else {
            OT_CHECK_EQ(std::string_view(name(post.status)), std::string_view(name(after_ack(pre.status))));
            OT_CHECK_EQ(post.leaves_qty, pre.leaves_qty);
        }
    }

    void report_reject(oms::OrderId id, const oms::OrderInfo&) {
        Shadow& sh = shadow(id);
        hit(h_reject);
        om().on_rejected(rej(sh.tok, 'X'), now_);
        OT_STATE(om(), id, "rejected");
        OT_CHECK_EQ(info(id).reason, 'X');
        OT_CHECK_EQ(info(id).leaves_qty, Qty{0});
    }

    void report_exec(oms::OrderId id, const oms::OrderInfo& pre) {
        Shadow& sh = shadow(id);
        const Qty over = rng_.chance(1, 6) ? 1 + static_cast<Qty>(rng_.bounded(5)) : 0;
        const Qty shares = rng_.chance(1, 3) ? pre.leaves_qty + over
                                             : 1 + static_cast<Qty>(rng_.bounded(pre.leaves_qty + over));
        const Price px = 960'000 + static_cast<Price>(rng_.bounded(80'001));
        const std::size_t fills_before = rig_.lis.fills.size();
        hit(h_exec);
        if (shares > pre.leaves_qty) hit(h_exec_clamped);
        if (pre.status == oms::OrderStatus::pending_new) hit(h_exec_before_ack);
        om().on_executed(exe(sh.tok, shares, px, ++match_), now_);
        sh.acked = true;
        sh.last_match = match_;
        sh.last_shares = shares;
        sh.last_px = px;
        // An execution is a fact: all of it is booked, also the part beyond the open quantity
        // the manager believed in; only leaves stops at zero.
        const Qty closed = std::min(shares, pre.leaves_qty);
        const oms::OrderInfo& post = info(id);
        OT_CHECK_EQ(post.cum_qty, pre.cum_qty + shares);
        OT_CHECK_EQ(post.leaves_qty, pre.leaves_qty - closed);
        OT_CHECK_EQ(post.req.qty, std::max(pre.req.qty, post.cum_qty));
        OT_CHECK_EQ(rig_.lis.fills.size(), fills_before + 1);
        OT_CHECK_EQ(rig_.lis.fills.back().qty, shares);
        OT_CHECK_EQ(rig_.lis.fills.back().price, px);
        if (post.leaves_qty == 0) {
            OT_STATE(om(), id, "filled");
            hit(h_exec_completes);
        } else {
            OT_CHECK_EQ(std::string_view(name(post.status)), std::string_view(name(after_ack(pre.status))));
        }
    }

    void report_cancel(oms::OrderId id, const oms::OrderInfo& pre) {
        Shadow& sh = shadow(id);
        const Qty over = rng_.chance(1, 8) ? 1 + static_cast<Qty>(rng_.bounded(4)) : 0;
        const Qty dec = rng_.chance(2, 5) ? pre.leaves_qty + over
                                          : 1 + static_cast<Qty>(rng_.bounded(pre.leaves_qty + over));
        hit(h_cancel_report);
        om().on_canceled(can(sh.tok, dec, 'U'), now_);
        sh.acked = true;
        const Qty applied = std::min(dec, pre.leaves_qty);
        const oms::OrderInfo& post = info(id);
        OT_CHECK_EQ(post.leaves_qty, pre.leaves_qty - applied);
        OT_CHECK_EQ(post.cum_qty, pre.cum_qty);
        OT_CHECK_EQ(post.reason, 'U');
        if (post.leaves_qty == 0) {
            OT_STATE(om(), id, "canceled");
            hit(h_cancel_full);
        } else {
            OT_CHECK_EQ(std::string_view(name(post.status)), std::string_view(name(after_ack(pre.status))));
            hit(h_cancel_partial);
        }
    }

    void report_replaced(oms::OrderId id, const oms::OrderInfo& pre) {
        Shadow& sh = shadow(id);
        const Qty asked = sh.pend_qty > pre.cum_qty ? sh.pend_qty - pre.cum_qty : 0;
        const bool dead = rng_.chance(1, 6);
        Qty open = asked;
        if (rng_.chance(1, 4)) open = static_cast<Qty>(rng_.bounded(asked + 1));
        if (rng_.chance(1, 8)) open = asked + 1 + static_cast<Qty>(rng_.bounded(9));  // overstated
        const bool no_price = rng_.chance(1, 6);
        const Price px = no_price ? 0 : sh.pend_px;
        const std::uint64_t old_tok = sh.tok;
        hit(dead ? h_replaced_dead : h_replaced);
        if (!dead && open < asked) hit(h_replaced_cut);
        if (pre.status == oms::OrderStatus::pending_cancel) hit(h_replaced_while_cancelling);
        om().on_replaced(rpl(old_tok, sh.pend_tok, open, px, 2000 + id, dead ? 'D' : 'L'), now_);
        sh.tok = sh.pend_tok;
        sh.pend_tok = 0;
        // The exchange's open quantity is the truth (capped at what was asked), also when it is
        // more than the manager held.
        const Qty want = dead ? 0 : std::min(open, asked);
        const oms::OrderInfo& post = info(id);
        OT_CHECK_EQ(post.leaves_qty, want);
        OT_CHECK_EQ(post.cum_qty, pre.cum_qty);
        OT_CHECK_EQ(post.req.qty, std::max(sh.pend_qty, pre.cum_qty));  // never below what has executed
        OT_CHECK_EQ(post.req.price, no_price ? sh.pend_px : px);
        OT_CHECK_EQ(post.exchange_ref, OrderRef{2000 + id});
        if (want == 0) {
            OT_STATE(om(), id, post.cum_qty >= post.req.qty ? "filled" : "canceled");
        } else {
            const auto st = pre.status == oms::OrderStatus::pending_replace ? oms::OrderStatus::live : pre.status;
            OT_CHECK_EQ(std::string_view(name(post.status)), std::string_view(name(st)));
            OT_CHECK_EQ(om().reserved_qty(id), want);
        }
    }

    void report_reject_replace(oms::OrderId id, const oms::OrderInfo& pre) {
        Shadow& sh = shadow(id);
        hit(h_replace_rejected);
        om().on_rejected(rej(sh.pend_tok, 'X'), now_);
        sh.pend_tok = 0;
        const oms::OrderInfo& post = info(id);
        OT_STATE(om(), id, "live");
        OT_CHECK_EQ(post.req.qty, pre.req.qty);
        OT_CHECK_EQ(post.req.price, pre.req.price);
        OT_CHECK_EQ(post.leaves_qty, pre.leaves_qty);
        OT_CHECK_EQ(om().reserved_qty(id), pre.leaves_qty);
    }

    // ---- illegal reports: must be counted once and change nothing -----------------------

    void illegal_report() {
        const std::uint64_t before = fingerprint(rig_);
        const std::uint64_t drops = dropped(om());
        if (!inject_illegal()) return;
        hit(static_cast<Hit>(h_illegal_0 + last_illegal_));
        OT_CHECK_EQ(fingerprint(rig_), before);
        OT_CHECK_EQ(dropped(om()), drops + 1);
        ++dropped_;
    }

    // Sends one report of type `which` (0..4) addressed by `t`.
    void send_report(int which, const ouch::Token& t) {
        switch (which) {
            case 0: { ouch::Accepted a; a.token = t; om().on_accepted(a, now_); break; }
            case 1: { ouch::Executed x; x.token = t; x.shares = 5; x.price = kPx; om().on_executed(x, now_); break; }
            case 2: { ouch::Canceled c; c.token = t; c.decrement = 5; om().on_canceled(c, now_); break; }
            case 3: { ouch::Rejected j; j.token = t; om().on_rejected(j, now_); break; }
            default: {
                ouch::Replaced p; p.a.token = t; p.a.shares = 5; p.a.price = kPx; p.previous = tok(1);
                om().on_replaced(p, now_);
                break;
            }
        }
    }

    bool inject_illegal() {
        const int kind = static_cast<int>(rng_.bounded(11));
        last_illegal_ = static_cast<std::size_t>(kind);
        const int which = static_cast<int>(rng_.bounded(5));
        switch (kind) {
            case 0:  // never issued
                send_report(which, tok(50'000'000 + rng_.bounded(1000)));
                return true;
            case 1: {  // right number, wrong spelling
                const oms::OrderId id = pick(-1);
                if (id == 0) return false;
                const std::string digits = std::to_string(shadow(id).tok);
                const ouch::Token forms[] = {ouch::Token::from_text(digits),
                                             ouch::Token::from_text(" " + digits), ouch::Token{},
                                             ouch::Token::from_text("Q" + digits)};
                send_report(which, forms[rng_.bounded(4)]);
                return true;
            }
            case 2: {  // the order is finished
                const oms::OrderId id = pick(0);
                if (id == 0) return false;
                send_report(which, tok(shadow(id).tok));
                return true;
            }
            case 3: {  // a second Accepted
                const oms::OrderId id = pick(1);
                if (id == 0 || !shadow(id).accepted) return false;
                ouch::Accepted a = acc(shadow(id).tok, 1);
                om().on_accepted(a, now_);
                return true;
            }
            case 4: {  // Rejected for an order the exchange holds
                const oms::OrderId id = pick(1);
                if (id == 0 || !shadow(id).acked) return false;
                om().on_rejected(rej(shadow(id).tok), now_);
                return true;
            }
            case 5: {  // Executed that makes no sense
                const oms::OrderId id = pick(1);
                if (id == 0) return false;
                const std::uint64_t t = shadow(id).tok;
                switch (rng_.bounded(4)) {
                    case 0: om().on_executed(exe(t, 0, kPx), now_); break;
                    case 1: om().on_executed(exe(t, 5, 0), now_); break;
                    case 2: om().on_executed(exe(t, 5, -7), now_); break;
                    default: om().on_executed(exe(t, 5, 0x1'0000'0000LL), now_); break;
                }
                return true;
            }
            case 6: {  // Canceled by zero shares
                const oms::OrderId id = pick(1);
                if (id == 0) return false;
                om().on_canceled(can(shadow(id).tok, 0), now_);
                return true;
            }
            case 7: {  // Accepted with a bad order state
                const oms::OrderId id = pick(1);
                if (id == 0 || shadow(id).accepted) return false;
                om().on_accepted(acc(shadow(id).tok, 1, 'X'), now_);
                return true;
            }
            case 8: {  // Replaced that is not the answer to a replace
                const oms::OrderId id = pick(1);
                if (id == 0) return false;
                const Shadow& sh = shadow(id);
                if (sh.pend_tok == 0) {
                    om().on_replaced(rpl(sh.tok, sh.tok, 5, kPx), now_);  // nothing outstanding
                } else if (rng_.chance(1, 2)) {
                    om().on_replaced(rpl(sh.tok + 1000, sh.pend_tok, 5, kPx), now_);  // wrong previous
                } else {
                    om().on_replaced(rpl(sh.tok, sh.pend_tok, 5, kPx, 1, 'Z'), now_);  // bad state char
                }
                return true;
            }
            case 9: {  // anything but Replaced/Rejected on an unconfirmed replacement token
                const oms::OrderId id = pick(1);
                if (id == 0 || shadow(id).pend_tok == 0) return false;
                send_report(static_cast<int>(rng_.bounded(3)), tok(shadow(id).pend_tok));
                return true;
            }
            default: {  // the last execution again, same match number
                const oms::OrderId id = pick(1);
                if (id == 0 || shadow(id).last_match == 0) return false;
                const Shadow& sh = shadow(id);
                om().on_executed(exe(sh.tok, sh.last_shares, sh.last_px, sh.last_match), now_);
                return true;
            }
        }
    }

    // ---- invariants ---------------------------------------------------------------------

    void check_all() {
        // Fold new fills into the independent tallies.
        for (; fills_seen_ < rig_.lis.fills.size(); ++fills_seen_) {
            const oms::Fill& f = rig_.lis.fills[fills_seen_];
            OT_CHECK(f.qty > 0);
            if (tally_.size() <= f.id) tally_.resize(f.id + 1);
            Tally& t = tally_[f.id];
            const std::int64_t signed_qty = f.side == Side::buy ? f.qty : -static_cast<std::int64_t>(f.qty);
            t.qty += f.qty;
            t.notional += static_cast<std::uint64_t>(f.price) * f.qty;
            position_[f.locate] += signed_qty;
        }

        const std::size_t n = om().orders_submitted();
        std::size_t open = 0;
        std::int64_t sum_leaves[8][2] = {};
        std::int64_t sum_reserved[8][2] = {};
        for (oms::OrderId id = 1; id <= n; ++id) {
            if (!tabled(id)) continue;  // finished, and its slot has been reused
            const oms::OrderInfo& i = info(id);
            OT_CHECK(i.leaves_qty + i.cum_qty <= i.req.qty);
            OT_CHECK(i.cum_qty <= i.req.qty);
            OT_CHECK(i.req.qty >= 1 && i.req.qty <= kMaxOrderQty);

            const Tally t = id < tally_.size() ? tally_[id] : Tally{};
            OT_CHECK_EQ(static_cast<std::int64_t>(i.cum_qty), t.qty);
            OT_CHECK_EQ(i.avg_price, t.qty > 0 ? static_cast<Price>(t.notional / static_cast<std::uint64_t>(t.qty)) : Price{0});

            const Qty reserved = om().reserved_qty(id);
            if (oms::is_terminal(i.status)) {
                OT_CHECK_EQ(i.leaves_qty, Qty{0});
                OT_CHECK_EQ(reserved, Qty{0});
                if (final_.size() <= id) final_.resize(id + 1);
                if (!have_final(id)) {
                    final_[id] = {true, i};
                } else {
                    OT_CHECK(same_info(final_[id].second, i));  // terminal orders never change
                }
                continue;
            }
            ++open;
            OT_CHECK(i.leaves_qty >= 1);
            // Re-derived from the shadow, not from the manager: the ledger holds the leaves, or
            // while a replace is in flight the larger of that and the open size it asks for.
            const Shadow& sh = shadow(id);
            Qty expect = i.leaves_qty;
            if (sh.pend_tok != 0 && sh.pend_qty > i.cum_qty) {
                expect = std::max(expect, sh.pend_qty - i.cum_qty);
            }
            OT_CHECK_EQ(reserved, expect);
            if (!grow_ && sh.pend_tok == 0) OT_CHECK_EQ(reserved, i.leaves_qty);
            OT_CHECK(i.req.locate < 8);
            sum_leaves[i.req.locate][index(i.req.side)] += i.leaves_qty;
            sum_reserved[i.req.locate][index(i.req.side)] += reserved;
        }
        OT_CHECK_EQ(om().open_orders(), open);

        std::int64_t all_open = 0;
        for (Locate l = 0; l < 8; ++l) {
            for (int s = 0; s < 2; ++s) {
                const Side side = s == 0 ? Side::buy : Side::sell;
                OT_CHECK_EQ(rig_.risk.open_qty(l, side), sum_reserved[l][s]);
                OT_CHECK(sum_reserved[l][s] >= sum_leaves[l][s]);
                all_open += sum_reserved[l][s];
            }
            OT_CHECK_EQ(rig_.risk.position(l).qty, position_[l]);
        }
        (void)all_open;
        // The count is exact, not an estimate: one per working order, through every cancel,
        // fill, growing replace and recycled slot.
        OT_CHECK_EQ(static_cast<std::size_t>(rig_.risk.open_orders()), open);
    }

    bool have_final(oms::OrderId id) const { return final_[id].first; }

    // Everything the run produced, folded into one number: the same seed must give the same one.
    std::uint64_t digest() {
        Digest d;
        for (const oms::Fill& f : rig_.lis.fills) {
            d.update(f.id); d.update(f.qty); d.update(static_cast<std::uint64_t>(f.price));
            d.update(f.ts); d.update(f.match);
        }
        for (const oms::OrderInfo& i : rig_.lis.updates) {
            d.update(i.id); d.update(static_cast<std::uint64_t>(i.status)); d.update(i.cum_qty);
            d.update(i.leaves_qty); d.update(static_cast<std::uint64_t>(i.avg_price));
            d.update(i.exchange_ref); d.update(i.last_update);
        }
        for (const ouch::EnterOrder& m : rig_.gw.enters) { d.update(token_id(m.token)); d.update(m.shares); }
        for (const ouch::CancelOrder& m : rig_.gw.cancels) d.update(token_id(m.token));
        for (const ouch::ReplaceOrder& m : rig_.gw.replaces) {
            d.update(token_id(m.existing)); d.update(token_id(m.replacement)); d.update(m.shares);
        }
        d.update(static_cast<std::uint64_t>(rig_.risk.realized_pnl()));
        d.update(fingerprint(rig_));
        return d.value();
    }

    Rng rng_;
    bool grow_;
    Rig rig_;
    Nanos now_{1'000'000};
    bool gw_open_{true};
    std::uint64_t match_{0};
    std::uint64_t applied_{0};
    std::uint64_t dropped_{0};
    std::array<std::uint64_t, h_count> hits_{};
    std::size_t last_illegal_{0};
    std::size_t fills_seen_{0};
    std::vector<Shadow> shadow_;
    std::vector<Tally> tally_;
    std::vector<std::pair<bool, oms::OrderInfo>> final_;
    std::array<std::int64_t, 8> position_{};  // signed sum of listener fills per locate
};

}  // namespace

namespace {

// Runs the scenario over several seeds and returns the summed coverage.
std::array<std::uint64_t, h_count> play(std::uint64_t first_seed, int seeds, bool growth, int steps) {
    std::array<std::uint64_t, h_count> total{};
    for (int k = 0; k < seeds; ++k) {
        Scenario s(first_seed + static_cast<std::uint64_t>(k), growth);
        s.run(steps);
        for (std::size_t h = 0; h < h_count; ++h) total[h] += s.hits()[h];
    }
    return total;
}

void require_covered(const std::array<std::uint64_t, h_count>& hits,
                     std::initializer_list<std::size_t> except = {}) {
    for (std::size_t h = 0; h < h_count; ++h) {
        if (std::find(except.begin(), except.end(), h) == except.end() && hits[h] == 0) {
            ::ot_test::fail(__FILE__, __LINE__, std::string("never exercised: ") + kHitNames[h]);
        }
    }
}

}  // namespace

OT_TEST(random_play_keeps_the_ledger_consistent_without_growth) {
    // Replaces never grow an order here, so risk.open_qty must equal the sum of leaves of the
    // non-terminal orders on each side at every one of the checked steps.
    const auto hits = play(1, 5, false, 5000);
    OT_CHECK(hits[h_exec] > 300 && hits[h_submit_ok] > 300);
    OT_CHECK_EQ(hits[h_replace_growth], std::uint64_t{0});
    require_covered(hits, {h_replace_growth, h_replace_risk});  // only growth is risk checked
}

OT_TEST(random_play_keeps_the_ledger_consistent_with_growing_replaces) {
    const auto hits = play(101, 10, true, 5000);
    OT_CHECK(hits[h_exec] > 300 && hits[h_submit_ok] > 300);
    require_covered(hits);
}

OT_TEST(random_play_is_deterministic_for_a_seed) {
    const std::uint64_t a = Scenario(7, true).run(3000);
    const std::uint64_t b = Scenario(7, true).run(3000);
    const std::uint64_t c = Scenario(8, true).run(3000);
    OT_CHECK_EQ(a, b);
    OT_CHECK(a != c);
    const std::uint64_t d = Scenario(7, false).run(3000);
    const std::uint64_t e = Scenario(7, false).run(3000);
    OT_CHECK_EQ(d, e);
}

OT_TEST_MAIN()

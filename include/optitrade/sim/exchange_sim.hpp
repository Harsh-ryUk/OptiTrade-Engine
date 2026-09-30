#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include <vector>

#include "optitrade/book/market_books.hpp"
#include "optitrade/core/flat_hash_map.hpp"
#include "optitrade/core/slab_pool.hpp"
#include "optitrade/core/types.hpp"
#include "optitrade/itch/decoder.hpp"
#include "optitrade/itch/messages.hpp"
#include "optitrade/oms/interfaces.hpp"
#include "optitrade/ouch/codec.hpp"
#include "optitrade/ouch/messages.hpp"

// Exchange simulator: an OrderGateway that answers OUCH order entry with OUCH
// reports, using the recorded ITCH feed as the only source of market activity.
//
// The simulator is a model, not a matching engine. Its purpose is to give a
// strategy replayed against historical data a defensible, deterministic and
// conservative answer to "would my order have been filled, and when". The rules:
//
// Inputs and time
//   * on_itch() feeds the same messages the engine sees. The simulator owns a
//     private MarketBooks built from them; our own orders are never added to it.
//   * Time is always passed in. There is no clock inside: `now` of send() and
//     on_itch()/advance()/drain_reports() is the caller's simulated time.
//   * An order sent at t "arrives" at t + order_latency_ns. Before each market
//     message (and in advance()) every request with arrival <= now is processed,
//     in send order, against the book as it stood BEFORE that message; a request
//     arriving exactly at a message's timestamp therefore sees the pre-message
//     book. Events caused by an arrival are stamped with the arrival time, events
//     caused by a market message with that message's `now`. The internal event
//     clock never runs backwards (a stale `now` is clamped up), so the report
//     queue is always ordered by delivery time.
//   * Every report is delivered at event time + report_latency_ns. The timestamp
//     inside the OUCH message is the event time.
//   * send() calls must carry non-decreasing `now` values; the request queue is
//     FIFO and a request is processed only when it reaches the front.
//
// Order entry (validated on arrival, in this order)
//   token not alphanumeric (or blank), or already used by a working order -> Rejected 'O'
//   stock not in the feed's directory                                     -> Rejected 'S'
//   shares == 0 -> 'O', shares > 999'999 -> 'Z'
//   price <= 0 or above 200'000.0000 (the highest legal ITCH price)       -> Rejected 'X'
//   order table full                                                      -> Rejected 'O'
// then Accepted (state 'L'). Time in force 0 is IOC; every other value is treated
// as good until the end of the run (no timeouts). Display, minimum quantity,
// short-sale and sweep flags are echoed but have no effect.
// Token uniqueness is enforced against WORKING orders. A finished order's slot and
// token are recycled at once, which bounds max_orders by concurrency rather than by
// the length of the run. The order manager mints strictly increasing tokens, so it
// never notices.
//
// Matching. When the order's side crosses the opposite side of the simulator's
// book (buy: ask <= limit, sell: bid >= limit), it walks the displayed levels best
// first and takes min(remaining, displayed at level) at each level's price
// (Executed, liquidity 'R'). Market impact is not modelled: the displayed book is
// not reduced, so the next arrival sees the same liquidity again. This is the
// optimistic direction of the approximation and is deliberate for a research
// simulator; keep order sizes small relative to displayed depth. An IOC remainder
// is canceled with reason 'I'. A DAY remainder rests.
//
// Queue position. A resting order joins the BACK of its price level:
//   ahead = displayed quantity at that side and price
//         + leaves of our own earlier orders at the same price.
// Displayed executions (ITCH E, and C at its display level) at that side and price
// remove min(shares, ahead) from the queue in front of us; only shares beyond
// `ahead` fill our order (Executed at the level price for E, at the C message's
// execution price for C, liquidity 'A'). Partial fills are normal; the fill of one
// message is split over our orders at the level in arrival order.
// Conservative cancel handling: cancels and deletes by OTHER participants (X, D,
// and the delete half of U) never advance our position, because the feed does not
// tell us where in the queue they sat. Only our own cancel or replace does, and
// then only for orders behind ours. Non-displayed trades (P) are ignored.
//
// Trade-through. After an ITCH add or replace, if the opposite best price has
// moved through a resting limit (bid: best ask <= limit, ask: best bid >= limit),
// the remainder fills at OUR limit price, because our order would have been the
// better price and would have traded first. The full remaining quantity is filled,
// not limited to what the crossing level displays: another optimistic
// approximation, consistent with the no-impact rule.
//
// Cancel (X). `shares` is the new intended size: the most that may execute in
// total after the cancel. 0 cancels everything open. It never increases an order
// (such a request is ignored) and a reduction keeps queue position. Reports:
// Canceled, reason 'U'. A cancel for an unknown or finished token is ignored,
// as on the real venue ("too late to cancel" produces no message).
//
// Replace (U). `shares` is the total liable for the chain, executions included, so
// the open quantity becomes shares - executed. A replace always loses priority:
// the order leaves its queue and re-enters at the back of its (possibly new)
// price, and if the new price is marketable it matches at once like a new order.
// Replaced is reported with the replacement token, the previous token, and the open
// quantity. A replace of an unknown token, or onto a token that is already used, is
// ignored. A replace of a live order with invalid terms (bad price, shares not above
// the executed quantity, shares >= 1'000'000) cancels the live order (Canceled 'U'),
// which is what the venue's specification prescribes.
//
// Determinism. No wall clock, no randomness, no hash-order iteration: orders at a
// level are visited in arrival order, requests in send order, and reports leave in
// the order they were generated. The same inputs produce byte-identical reports.
//
// Storage. Everything is sized in the constructor from SimConfig: the request
// queue (max_orders entries), the order pool and token index (max_orders), the
// per-instrument order lists (2 x 65536 handles) and the report queue
// (max(1024, 2 x max_orders) entries). Nothing grows afterwards. send() returns
// false when the request queue is full. If the report queue is full the report is
// dropped and counted in dropped_reports(); drain regularly. MarketBooks follows its
// own allocation policy (see market_books.hpp).
//
// Not thread safe. drain_reports' callback must not call drain_reports.
namespace optitrade::sim {

struct SimConfig {
    Nanos order_latency_ns{50'000};
    Nanos report_latency_ns{50'000};
    std::size_t max_orders{1u << 16};
    book::MarketBooks::Config books{};
};

class ExchangeSim final : public oms::OrderGateway {
public:
    explicit ExchangeSim(const SimConfig& config)
        : cfg_(config),
          books_(config.books),
          requests_(config.max_orders),
          orders_(config.max_orders),
          by_token_(config.max_orders),
          heads_(2 * kLocates, kNil),
          tails_(2 * kLocates, kNil),
          reports_(std::max<std::size_t>(1024, 2 * config.max_orders)) {}

    bool send(const ouch::EnterOrder& m, Nanos now) override {
        Request r;
        r.kind = Kind::enter;
        r.token = m.token;
        r.side = m.side;
        r.shares = m.shares;
        r.stock = m.stock;
        r.price = m.price;
        r.tif = m.time_in_force;
        std::memcpy(r.firm, m.firm, sizeof r.firm);
        r.display = m.display;
        r.capacity = m.capacity;
        r.sweep = m.intermarket_sweep;
        r.min_qty = m.min_qty;
        r.cross = m.cross_type;
        return enqueue(r, now);
    }

    bool send(const ouch::CancelOrder& m, Nanos now) override {
        Request r;
        r.kind = Kind::cancel;
        r.token = m.token;
        r.shares = m.shares;
        return enqueue(r, now);
    }

    bool send(const ouch::ReplaceOrder& m, Nanos now) override {
        Request r;
        r.kind = Kind::replace;
        r.token = m.existing;
        r.replacement = m.replacement;
        r.shares = m.shares;
        r.price = m.price;
        r.tif = m.time_in_force;
        r.display = m.display;
        r.sweep = m.intermarket_sweep;
        r.min_qty = m.min_qty;
        return enqueue(r, now);
    }

    // One unframed ITCH message stamped `now`. Requests that have arrived are
    // processed first, then the message is applied and the fills it causes are
    // generated. Messages the decoder rejects are ignored: the simulator mirrors
    // the engine's feed, which reports those errors itself.
    void on_itch(std::span<const std::byte> msg, Nanos now) {
        advance(now);
        clock_ = std::max(clock_, now);
        Router router{*this};
        itch::decode(msg, router);
    }

    // Processes every request with arrival <= now without a market message.
    void advance(Nanos now) noexcept {
        while (queued_ != 0 && requests_[req_head_].arrival <= now) {
            const Request r = requests_[req_head_];
            req_head_ = (req_head_ + 1) % requests_.size();
            --queued_;
            clock_ = std::max(clock_, r.arrival);
            switch (r.kind) {
                case Kind::enter: on_enter(r); break;
                case Kind::cancel: on_cancel(r); break;
                case Kind::replace: on_replace(r); break;
            }
        }
    }

    // Calls f(unframed OUCH message, delivery time) for every report due at `now`,
    // oldest first. The span is valid only during the call.
    template <class F>
    void drain_reports(Nanos now, F&& f) {
        while (report_count_ != 0) {
            const Report& r = reports_[report_head_];
            if (r.deliver > now) break;
            f(std::span<const std::byte>(r.bytes.data(), r.length), r.deliver);
            report_head_ = (report_head_ + 1) % reports_.size();
            --report_count_;
        }
    }

    std::size_t pending_reports() const noexcept { return report_count_; }
    std::uint64_t fills() const noexcept { return fills_; }  // Executed reports generated
    std::size_t resting_orders() const noexcept { return orders_.size(); }
    std::uint64_t dropped_reports() const noexcept { return dropped_reports_; }
    const book::MarketBooks& books() const noexcept { return books_; }

private:
    using Handle = std::uint32_t;
    static constexpr Handle kNil = SlabPool<int>::kNull;  // "no order" in the per-level lists

    static constexpr std::size_t kLocates = std::size_t{1} << 16;
    static constexpr Price kMaxPrice = 2'000'000'000;  // 200'000.0000, highest legal ITCH price
    static constexpr char kRemoved = 'R';
    static constexpr char kAdded = 'A';

    enum class Kind : std::uint8_t { enter, cancel, replace };

    // A request in flight: the union of the three inbound messages. Kept as a
    // struct, not as wire bytes, so invalid prices and sizes can be answered
    // instead of failing to encode.
    struct Request {
        Nanos arrival{};
        Kind kind{Kind::enter};
        ouch::Token token;        // enter: token; cancel: token; replace: existing token
        ouch::Token replacement;  // replace only
        Side side{Side::buy};
        Qty shares{};
        Symbol stock;
        Price price{};
        std::uint32_t tif{};
        char firm[4]{' ', ' ', ' ', ' '};
        char display{'Y'};
        char capacity{'P'};
        char sweep{'N'};
        Qty min_qty{};
        char cross{'N'};
    };

    struct Order {
        ouch::Token token;
        Locate locate{};
        Side side{Side::buy};
        Price price{};
        Qty leaves{};
        Qty filled{};                // executed so far, kept across replaces
        std::uint64_t ahead{};       // shares queued in front of us at our price
        std::uint32_t tif{};
        Symbol stock;
        char firm[4]{' ', ' ', ' ', ' '};
        char display{'Y'};
        char capacity{'P'};
        char sweep{'N'};
        Qty min_qty{};
        char cross{'N'};
        OrderRef ref{};
        bool resting{false};         // linked into its (locate, side) list
        Handle prev{kNil};           // per (locate, side) list in arrival order
        Handle next{kNil};
    };

    using Pool = SlabPool<Order>;
    static_assert(kNil == Pool::kNull);

    struct Report {
        Nanos deliver{};
        std::size_t length{};
        std::array<std::byte, ouch::kMaxMessageLength> bytes{};
    };

    struct TokenHash {
        std::uint64_t operator()(const ouch::Token& t) const noexcept {
            std::uint64_t a = 0;
            std::uint64_t b = 0;
            std::memcpy(&a, t.raw().data(), 8);
            std::memcpy(&b, t.raw().data() + ouch::kTokenSize - 8, 8);  // overlaps a; covers all 14 bytes
            return mix64(a ^ mix64(b));
        }
    };

    // Routes decoded ITCH messages to apply(); the generic overload just updates the books.
    struct Router {
        ExchangeSim& sim;
        template <class M>
        void on(const M& m) noexcept { sim.apply(m); }
    };

    // ---- market data --------------------------------------------------------

    template <class M>
    void apply(const M& m) noexcept { books_.on(m); }

    void apply(const itch::AddOrder& m) noexcept {
        if (books_.on(m) == book::Applied::ok) sweep_through(m.h.locate);
    }

    void apply(const itch::OrderExecuted& m) noexcept {
        const book::MarketBooks::OrderInfo* o = books_.order(m.ref);
        if (o == nullptr) {
            books_.on(m);
            return;
        }
        const book::MarketBooks::OrderInfo info = *o;  // the order may be gone after on()
        if (books_.on(m) == book::Applied::ok) consume(info.locate, info.side, info.price, info.price, m.shares);
    }

    void apply(const itch::OrderExecutedPrice& m) noexcept {
        const book::MarketBooks::OrderInfo* o = books_.order(m.ref);
        if (o == nullptr) {
            books_.on(m);
            return;
        }
        const book::MarketBooks::OrderInfo info = *o;
        // The display level is consumed at the order's own price; the trade prints at m.price.
        if (books_.on(m) == book::Applied::ok) consume(info.locate, info.side, info.price, m.price, m.shares);
    }

    void apply(const itch::OrderReplace& m) noexcept {
        const book::MarketBooks::OrderInfo* o = books_.order(m.old_ref);
        const bool known = o != nullptr;
        const Locate loc = known ? o->locate : Locate{};
        if (books_.on(m) == book::Applied::ok && known) sweep_through(loc);
    }

    // Our orders whose limit the opposite best has reached fill at their own limit.
    void sweep_through(Locate loc) noexcept {
        const book::OrderBook* bk = books_.book(loc);
        if (bk == nullptr) return;
        for (const Side side : {Side::buy, Side::sell}) {
            Handle h = heads_[slot(loc, side)];
            if (h == kNil) continue;
            const std::optional<book::Level> far_touch = bk->best(opposite(side));
            if (!far_touch) continue;
            while (h != kNil) {
                const Handle next = orders_[h].next;
                Order& o = orders_[h];
                if (crosses(side, o.price, far_touch->price)) {
                    execute(o, o.leaves, o.price, kAdded);
                    remove_order(h);
                }
                h = next;
            }
        }
    }

    // `shares` executed at display level `level` on `side`. Each of our orders at
    // the level loses `shares` from the queue in front of it and fills with what
    // is left over. Every order is judged against the same `shares`: an order
    // further back has all the earlier ones inside its `ahead`, so the result is
    // the same as consuming the queue front to back.
    void consume(Locate loc, Side side, Price level, Price exec_price, Qty shares) noexcept {
        Handle h = heads_[slot(loc, side)];
        while (h != kNil) {
            const Handle next = orders_[h].next;
            Order& o = orders_[h];
            if (o.price == level) {
                if (shares <= o.ahead) {
                    o.ahead -= shares;
                } else {
                    const std::uint64_t excess = shares - o.ahead;
                    o.ahead = 0;
                    execute(o, static_cast<Qty>(std::min<std::uint64_t>(o.leaves, excess)), exec_price, kAdded);
                    if (o.leaves == 0) remove_order(h);
                }
            }
            h = next;
        }
    }

    // ---- request processing --------------------------------------------------

    static bool is_alnum(char c) noexcept {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }

    // Alphanumeric characters followed only by spaces, at least one character.
    static bool valid_token(const ouch::Token& t) noexcept {
        const auto& c = t.raw();
        std::size_t n = 0;
        while (n < ouch::kTokenSize && is_alnum(c[n])) ++n;
        if (n == 0) return false;
        for (std::size_t i = n; i < ouch::kTokenSize; ++i) {
            if (c[i] != ' ') return false;
        }
        return true;
    }

    static bool crosses(Side side, Price limit, Price other) noexcept {
        return side == Side::buy ? other <= limit : other >= limit;
    }

    void reject(const ouch::Token& token, char reason) noexcept {
        emit(ouch::Rejected{clock_, token, reason});
    }

    void on_enter(const Request& r) noexcept {
        if (!valid_token(r.token) || by_token_.find(r.token) != nullptr) return reject(r.token, 'O');
        const std::optional<Locate> loc = books_.locate(r.stock);
        if (!loc) return reject(r.token, 'S');
        if (r.shares == 0) return reject(r.token, 'O');
        if (r.shares > kMaxOrderQty) return reject(r.token, 'Z');
        if (r.price <= 0 || r.price > kMaxPrice) return reject(r.token, 'X');
        const Handle h = orders_.allocate();
        if (h == kNil) return reject(r.token, 'O');
        if (by_token_.insert(r.token, h).first == nullptr) {
            orders_.release(h);
            return reject(r.token, 'O');
        }
        Order& o = orders_[h];
        o.token = r.token;
        o.locate = *loc;
        o.side = r.side;
        o.price = r.price;
        o.leaves = r.shares;
        o.tif = r.tif;
        o.stock = r.stock;
        std::memcpy(o.firm, r.firm, sizeof o.firm);
        o.display = r.display;
        o.capacity = r.capacity;
        o.sweep = r.sweep;
        o.min_qty = r.min_qty;
        o.cross = r.cross;
        o.ref = next_ref_++;
        emit(accepted_of(o, o.leaves));
        work(h);
    }

    void on_cancel(const Request& r) noexcept {
        Handle* found = by_token_.find(r.token);
        if (found == nullptr) return;
        const Handle h = *found;
        Order& o = orders_[h];
        // r.shares is the most that may execute in total; what may stay open is that
        // minus what already executed.
        const Qty keep = r.shares > o.filled ? r.shares - o.filled : 0;
        if (keep >= o.leaves) return;  // a cancel never grows an order
        const Qty removed = o.leaves - keep;
        emit(ouch::Canceled{clock_, o.token, removed, 'U'});
        if (keep == 0) {
            remove_order(h);
            return;
        }
        shrink_followers(h, removed);
        o.leaves = keep;
    }

    void on_replace(const Request& r) noexcept {
        Handle* found = by_token_.find(r.token);
        if (found == nullptr) return;
        if (!valid_token(r.replacement) || by_token_.find(r.replacement) != nullptr) return;
        const Handle h = *found;
        Order& o = orders_[h];
        const bool valid = r.price > 0 && r.price <= kMaxPrice && r.shares > o.filled && r.shares <= kMaxOrderQty;
        if (!valid) {
            emit(ouch::Canceled{clock_, o.token, o.leaves, 'U'});
            remove_order(h);
            return;
        }
        // Priority is lost: leave the queue (shrinking what is behind us), swap the
        // token, and re-enter as a new arrival.
        shrink_followers(h, o.leaves);
        unlink(h);
        const ouch::Token previous = o.token;
        by_token_.erase(previous);
        by_token_.insert(r.replacement, h);  // cannot fail: the erase just freed an entry
        o.token = r.replacement;
        o.price = r.price;
        o.leaves = r.shares - o.filled;
        o.tif = r.tif;
        o.display = r.display;
        o.sweep = r.sweep;
        o.min_qty = r.min_qty;
        o.ref = next_ref_++;
        emit(ouch::Replaced{accepted_of(o, o.leaves), previous});
        work(h);
    }

    // Matches a new or replaced order against the displayed book, then either
    // finishes it or rests it at the back of its level. `h` is invalid on return
    // if the order finished.
    void work(Handle h) noexcept {
        Order& o = orders_[h];
        const book::OrderBook* bk = books_.book(o.locate);
        if (bk != nullptr) {
            const Side opp = opposite(o.side);
            for (std::size_t i = 0; o.leaves > 0 && i < bk->depth(opp); ++i) {
                const book::Level lv = bk->level(opp, i);
                if (!crosses(o.side, o.price, lv.price)) break;
                execute(o, std::min(o.leaves, lv.qty), lv.price, kRemoved);
            }
        }
        if (o.leaves == 0) return remove_order(h);
        if (o.tif == ouch::kTifIoc) {
            emit(ouch::Canceled{clock_, o.token, o.leaves, 'I'});
            return remove_order(h);
        }
        std::uint64_t ahead = bk != nullptr ? bk->qty_at(o.side, o.price) : 0;
        for (Handle j = heads_[slot(o.locate, o.side)]; j != kNil; j = orders_[j].next) {
            if (orders_[j].price == o.price) ahead += orders_[j].leaves;
        }
        o.ahead = ahead;
        link_tail(h);
    }

    void execute(Order& o, Qty qty, Price price, char liquidity) noexcept {
        o.leaves -= qty;
        o.filled += qty;
        ++fills_;
        emit(ouch::Executed{clock_, o.token, qty, price, liquidity, ++match_});
    }

    // ---- order storage -------------------------------------------------------

    static std::size_t slot(Locate loc, Side side) noexcept { return (std::size_t{loc} << 1) | index(side); }

    void link_tail(Handle h) noexcept {
        Order& o = orders_[h];
        const std::size_t s = slot(o.locate, o.side);
        o.prev = tails_[s];
        o.next = kNil;
        o.resting = true;
        if (tails_[s] == kNil) {
            heads_[s] = h;
        } else {
            orders_[tails_[s]].next = h;
        }
        tails_[s] = h;
    }

    void unlink(Handle h) noexcept {
        Order& o = orders_[h];
        const std::size_t s = slot(o.locate, o.side);
        if (o.prev == kNil) {
            heads_[s] = o.next;
        } else {
            orders_[o.prev].next = o.next;
        }
        if (o.next == kNil) {
            tails_[s] = o.prev;
        } else {
            orders_[o.next].prev = o.prev;
        }
        o.prev = o.next = kNil;
        o.resting = false;
    }

    // `removed` shares of order h leave the queue: orders behind it at the same
    // price have that much less in front of them.
    void shrink_followers(Handle h, Qty removed) noexcept {
        const Order& o = orders_[h];
        for (Handle j = o.next; j != kNil; j = orders_[j].next) {
            Order& f = orders_[j];
            if (f.price == o.price) f.ahead -= std::min<std::uint64_t>(f.ahead, removed);
        }
    }

    // Takes an order out for good. An order that finished during its own arrival step
    // was never linked, so there is no queue to repair.
    void remove_order(Handle h) noexcept {
        const Order& o = orders_[h];
        if (o.resting) {
            shrink_followers(h, o.leaves);
            unlink(h);
        }
        by_token_.erase(o.token);
        orders_.release(h);
    }

    // ---- request and report queues ---------------------------------------------

    static Nanos saturating_add(Nanos a, Nanos b) noexcept {
        return a > ~Nanos{0} - b ? ~Nanos{0} : a + b;
    }

    bool enqueue(Request r, Nanos now) noexcept {
        if (queued_ == requests_.size()) return false;
        r.arrival = saturating_add(now, cfg_.order_latency_ns);
        requests_[(req_head_ + queued_) % requests_.size()] = r;
        ++queued_;
        return true;
    }

    ouch::Accepted accepted_of(const Order& o, Qty shares) const noexcept {
        ouch::Accepted a;
        a.ts = clock_;
        a.token = o.token;
        a.side = o.side;
        a.shares = shares;
        a.stock = o.stock;
        a.price = o.price;
        a.time_in_force = o.tif;
        std::memcpy(a.firm, o.firm, sizeof a.firm);
        a.display = o.display;
        a.ref = o.ref;
        a.capacity = o.capacity;
        a.intermarket_sweep = o.sweep;
        a.min_qty = o.min_qty;
        a.cross_type = o.cross;
        a.order_state = 'L';
        a.bbo_weight = ' ';
        return a;
    }

    template <class M>
    void emit(const M& m) noexcept {
        if (report_count_ == reports_.size()) {
            ++dropped_reports_;
            return;
        }
        Report& r = reports_[(report_head_ + report_count_) % reports_.size()];
        r.length = ouch::encode(m, r.bytes);
        if (r.length == 0) {  // unreachable while prices are validated; never queue an empty report
            ++dropped_reports_;
            return;
        }
        r.deliver = saturating_add(clock_, cfg_.report_latency_ns);
        ++report_count_;
    }

    SimConfig cfg_;
    book::MarketBooks books_;

    std::vector<Request> requests_;  // ring
    std::size_t req_head_{0};
    std::size_t queued_{0};

    Pool orders_;
    FlatHashMap<ouch::Token, Handle, TokenHash> by_token_;
    std::vector<Handle> heads_;  // per (locate, side): first and last resting order
    std::vector<Handle> tails_;

    std::vector<Report> reports_;  // ring
    std::size_t report_head_{0};
    std::size_t report_count_{0};

    Nanos clock_{0};
    OrderRef next_ref_{1};
    std::uint64_t match_{0};
    std::uint64_t fills_{0};
    std::uint64_t dropped_reports_{0};
};

}  // namespace optitrade::sim

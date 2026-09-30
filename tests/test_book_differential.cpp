// Differential test of MarketBooks against a deliberately naive reference.
//
// The reference is std::map per side per instrument plus std::unordered_map for
// orders, written from the rules in market_books.hpp and sharing no code with the
// implementation. Long seeded random streams (every ITCH order message, plus the
// invalid ones: unknown and duplicate references, over-cancels, over-executions,
// zero sizes, lying header locates) are applied to both after which, after EVERY
// message, the Applied result, the complete ladder of every instrument on both
// sides, the point queries, the counters and the order table must agree.
//
// The price range is tiny compared with the message count, so levels collide,
// grow, shrink and empty out constantly. Separate runs shrink the order table and
// the per-side level limit to drive the capacity and eviction paths, where the
// reference implements the documented overflow policy in map form.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "check.hpp"
#include "optitrade/book/market_books.hpp"
#include "optitrade/core/rng.hpp"
#include "optitrade/itch/decoder.hpp"
#include "optitrade/itch/encoder.hpp"

using namespace optitrade;
using book::Applied;
using book::Level;
using book::MarketBooks;
using book::OrderBook;

namespace {

constexpr Side B = Side::buy;
constexpr Side S = Side::sell;
constexpr std::uint64_t kQtyMax = 0xFFFFFFFFull;

// ---------------------------------------------------------------------------
// Reference model
// ---------------------------------------------------------------------------

struct RefLevel {
    std::uint64_t qty{};
    std::uint32_t orders{};
    std::uint64_t gen{};  // which incarnation of the price level this is
};

class RefSide {
public:
    RefSide(bool bids, std::size_t cap) : bids_(bids), cap_(cap) {}

    // Returns the incarnation the order landed on, or 0 if it was not stored.
    std::uint64_t add(Price p, Qty q) {
        auto it = levels_.find(p);
        if (it != levels_.end()) {
            if (it->second.qty + q > kQtyMax) {
                ++overflows_;
                return 0;
            }
            it->second.qty += q;
            ++it->second.orders;
            return it->second.gen;
        }
        if (levels_.size() >= cap_) {
            ++overflows_;
            if (cap_ == 0) return 0;
            const Price worst = bids_ ? levels_.begin()->first : levels_.rbegin()->first;
            if (bids_ ? p < worst : p > worst) return 0;
            levels_.erase(worst);
        }
        levels_[p] = RefLevel{q, 1, next_gen_};
        return next_gen_++;
    }

    void take(Price p, Qty q, bool whole, std::uint64_t gen) {
        auto it = levels_.find(p);
        if (it == levels_.end() || it->second.gen != gen) return;  // untracked or stale
        it->second.qty -= q;
        if (it->second.qty == 0) {
            levels_.erase(it);
        } else if (whole && it->second.orders > 1) {
            --it->second.orders;
        }
    }

    using Ladder = std::vector<std::pair<Price, RefLevel>>;

    // Best first. Fills a caller-owned buffer so the per-message comparison does not allocate.
    void ladder_into(Ladder& out) const {
        out.assign(levels_.begin(), levels_.end());
        if (bids_) std::reverse(out.begin(), out.end());
    }
    std::optional<Price> best() const {
        if (levels_.empty()) return std::nullopt;
        return bids_ ? levels_.rbegin()->first : levels_.begin()->first;
    }
    std::uint64_t overflows() const { return overflows_; }

private:
    bool bids_;
    std::size_t cap_;
    std::map<Price, RefLevel> levels_;
    std::uint64_t next_gen_{1};
    std::uint64_t overflows_{0};
};

struct RefOrder {
    Locate locate;
    Side side;
    Price price;
    Qty qty;
    std::uint64_t gen;  // 0: not on the ladder
};

struct RefBook {
    RefBook(std::size_t cap) : bids(true, cap), asks(false, cap) {}
    RefSide bids;
    RefSide asks;
    RefSide& side(Side s) { return s == B ? bids : asks; }
    const RefSide& side(Side s) const { return s == B ? bids : asks; }
};

std::string key(const Symbol& s) { return std::string(s.raw().data(), Symbol::kSize); }

class Reference {
public:
    Reference(std::size_t max_orders, std::size_t max_levels, std::size_t max_symbols)
        : max_orders_(max_orders), max_levels_(max_levels), max_symbols_(max_symbols) {}

    Applied on(const itch::StockDirectory& m) {
        if (m.symbol.empty()) return Applied::invalid;
        const auto known = by_symbol_.find(key(m.symbol));
        if (known != by_symbol_.end()) return known->second == m.h.locate ? Applied::ignored : Applied::invalid;
        if (symbol_of_.count(m.h.locate) != 0) return Applied::invalid;
        if (by_symbol_.size() >= max_symbols_) return Applied::capacity;
        book(m.h.locate);
        by_symbol_[key(m.symbol)] = m.h.locate;
        symbol_of_[m.h.locate] = m.symbol;
        return Applied::ok;
    }

    Applied on(const itch::AddOrder& m) {
        if (m.shares == 0) return Applied::invalid;
        if (orders_.count(m.ref) != 0) return Applied::duplicate_order;
        if (orders_.size() >= max_orders_) return Applied::capacity;
        if (!insert(m.ref, m.h.locate, m.side, m.price, m.shares)) return Applied::capacity;
        ++applied_;
        return Applied::ok;
    }

    Applied on(const itch::OrderExecuted& m) { return reduce(m.ref, m.shares, nullptr, true); }
    Applied on(const itch::OrderExecutedPrice& m) { return reduce(m.ref, m.shares, &m.price, true); }
    Applied on(const itch::OrderCancel& m) { return reduce(m.ref, m.shares, nullptr, false); }

    Applied on(const itch::OrderDelete& m) {
        auto it = orders_.find(m.ref);
        if (it == orders_.end()) return Applied::unknown_order;
        drop(it);
        ++applied_;
        return Applied::ok;
    }

    Applied on(const itch::OrderReplace& m) {
        auto it = orders_.find(m.old_ref);
        if (it == orders_.end()) return Applied::unknown_order;
        if (m.shares == 0) return Applied::invalid;
        if (orders_.count(m.new_ref) != 0) return Applied::duplicate_order;
        const RefOrder old = it->second;
        drop(it);
        if (!insert(m.new_ref, old.locate, old.side, m.price, m.shares)) return Applied::capacity;
        ++applied_;
        return Applied::ok;
    }

    Applied on(const itch::Trade& m) {
        last_trade_[m.h.locate] = m.price;
        return Applied::ignored;
    }
    Applied on(const itch::SystemEvent&) { return Applied::ignored; }

    // Queries used by the comparison.
    const RefBook* find_book(Locate l) const {
        const auto it = books_.find(l);
        return it == books_.end() ? nullptr : &it->second;
    }
    const std::unordered_map<OrderRef, RefOrder>& orders() const { return orders_; }
    bool contains(OrderRef r) const { return orders_.count(r) != 0; }
    const RefOrder* order(OrderRef r) const {
        const auto it = orders_.find(r);
        return it == orders_.end() ? nullptr : &it->second;
    }
    Price last_trade(Locate l) const {
        const auto it = last_trade_.find(l);
        return it == last_trade_.end() ? 0 : it->second;
    }
    std::optional<Locate> locate_of(const Symbol& s) const {
        const auto it = by_symbol_.find(key(s));
        if (it == by_symbol_.end()) return std::nullopt;
        return it->second;
    }
    Symbol symbol_of(Locate l) const {
        const auto it = symbol_of_.find(l);
        return it == symbol_of_.end() ? Symbol() : it->second;
    }
    std::uint64_t applied() const { return applied_; }
    std::uint64_t overflows() const {
        std::uint64_t n = 0;
        for (const auto& kv : books_) n += kv.second.bids.overflows() + kv.second.asks.overflows();
        return n;
    }

private:
    RefBook& book(Locate l) { return books_.try_emplace(l, RefBook(max_levels_)).first->second; }

    bool insert(OrderRef ref, Locate loc, Side side, Price price, Qty qty) {
        const std::uint64_t gen = book(loc).side(side).add(price, qty);
        orders_[ref] = RefOrder{loc, side, price, qty, gen};
        return gen != 0;
    }

    void drop(std::unordered_map<OrderRef, RefOrder>::iterator it) {
        const RefOrder o = it->second;
        if (o.gen != 0) book(o.locate).side(o.side).take(o.price, o.qty, true, o.gen);
        orders_.erase(it);
    }

    Applied reduce(OrderRef ref, Qty shares, const Price* trade_price, bool execution) {
        auto it = orders_.find(ref);
        if (it == orders_.end()) return Applied::unknown_order;
        if (shares == 0) return Applied::invalid;
        RefOrder& o = it->second;
        if (shares > o.qty) {
            drop(it);
            return Applied::invalid;
        }
        const Locate loc = o.locate;
        const Price traded = trade_price != nullptr ? *trade_price : o.price;
        if (shares == o.qty) {
            drop(it);
        } else {
            if (o.gen != 0) book(loc).side(o.side).take(o.price, shares, false, o.gen);
            o.qty -= shares;
        }
        if (execution) last_trade_[loc] = traded;
        ++applied_;
        return Applied::ok;
    }

    std::size_t max_orders_;
    std::size_t max_levels_;
    std::size_t max_symbols_;
    std::map<Locate, RefBook> books_;
    std::unordered_map<OrderRef, RefOrder> orders_;
    std::map<std::string, Locate> by_symbol_;
    std::map<Locate, Symbol> symbol_of_;
    std::map<Locate, Price> last_trade_;
    std::uint64_t applied_{0};
};

// ---------------------------------------------------------------------------
// Driver: random stream generator + lock-step comparison
// ---------------------------------------------------------------------------

struct Params {
    std::size_t max_orders = 1u << 14;
    std::size_t max_levels = 256;
    std::size_t max_symbols = 1u << 16;
    unsigned locates = 5;
    Price base = 10'000;
    Price range = 12;         // prices are base .. base + range - 1
    unsigned add_weight = 30; // relative to 60 for everything else combined
    unsigned drain_weight = 8; // add weight during drain phases
    unsigned phase_len = 4000; // messages per fill / drain phase; 0 = never drain
    bool compare = true;      // false: generate only (timing corpus)
    bool record = false;      // keep the wire form of every message
    unsigned sums_every = 50; // cadence of the order-table sweep
};

class Driver {
public:
    Driver(const Params& p, std::uint64_t seed)
        : p_(p),
          seed_(seed),
          rng_(seed),
          mb_(MarketBooks::Config{p.max_orders, p.max_levels, p.max_symbols}),
          ref_(p.max_orders, p.max_levels, p.max_symbols) {
        for (unsigned i = 0; i < p.locates; ++i) watch_.push_back(static_cast<Locate>(i));
        watch_.push_back(kWild[0]);
        watch_.push_back(kWild[1]);
        for (unsigned i = 0; i < kSymbols; ++i) symbols_.push_back(Symbol("S" + std::to_string(i)));
        seen_depth_.assign(watch_.size(), {0, 0});
        expect_.resize(static_cast<std::size_t>(p.range) + 2);
    }

    // Applies `n` random messages; false as soon as the two sides disagree.
    bool run(std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            if (!one()) return false;
        }
        return p_.compare ? sweep_orders(true) : true;
    }

    const MarketBooks& books() const { return mb_; }
    const Reference& reference() const { return ref_; }
    const std::vector<std::byte>& wire() const { return wire_; }
    const std::array<std::uint64_t, 7>& histogram() const { return hist_; }
    std::uint64_t empties() const { return empties_; }
    std::uint64_t zero_share_adds() const { return zero_adds_; }
    std::uint64_t messages() const { return step_; }

private:
    static constexpr unsigned kSymbols = 6;
    static constexpr Locate kWild[2] = {40000, 65535};

    // ---- random helpers ----
    bool coin() { return rng_.chance(1, 2); }
    Locate pick_locate() {
        if (rng_.chance(1, 100)) return kWild[rng_.bounded(2)];
        return static_cast<Locate>(rng_.bounded(p_.locates));
    }
    Price pick_price() { return p_.base + static_cast<Price>(rng_.bounded(static_cast<std::uint64_t>(p_.range))); }
    Qty pick_qty() {
        const std::uint64_t r = rng_.bounded(1000);
        if (r < 5) return 0;
        if (r < 8) return static_cast<Qty>(0xFFFFFF00u + rng_.bounded(256));  // exercises 32-bit level saturation
        return static_cast<Qty>(1 + rng_.bounded(300));
    }
    itch::Header header(Locate l) { return itch::Header{l, 0, step_}; }
    Locate lying_locate() { return static_cast<Locate>(rng_.next()); }

    // A reference that is live right now, or 0 if there is none. Dead pool
    // entries are dropped as they are met.
    OrderRef pick_live() {
        for (int tries = 0; tries < 8 && !pool_.empty(); ++tries) {
            const std::size_t i = static_cast<std::size_t>(rng_.bounded(pool_.size()));
            const OrderRef r = pool_[i];
            if (ref_.contains(r)) return r;
            pool_[i] = pool_.back();
            pool_.pop_back();
        }
        return 0;
    }
    OrderRef pick_unknown() {
        for (;;) {
            const OrderRef r = 1 + rng_.bounded(next_ref_ + 5);
            if (!ref_.contains(r)) return r;
        }
    }
    OrderRef pick_target() {
        if (rng_.chance(88, 100)) {
            const OrderRef r = pick_live();
            if (r != 0) return r;
        }
        return pick_unknown();
    }
    // Partial, exact, over, or zero, relative to what the reference says remains.
    Qty reduction_for(OrderRef r) {
        const RefOrder* o = ref_.order(r);
        if (o == nullptr) return static_cast<Qty>(rng_.bounded(300));
        const std::uint64_t q = o->qty;
        const std::uint64_t k = rng_.bounded(100);
        if (k < 55 && q > 1) return static_cast<Qty>(1 + rng_.bounded(q - 1));
        if (k < 80) return static_cast<Qty>(q);
        if (k < 95) return static_cast<Qty>(std::min<std::uint64_t>(q + 1 + rng_.bounded(50), kQtyMax));
        return 0;
    }

    // ---- lock-step application ----
    template <class M>
    bool apply(const M& m, const char* kind, std::initializer_list<OrderRef> touched = {}) {
        const Applied got = mb_.on(m);
        const Applied want = ref_.on(m);
        ++hist_[static_cast<std::size_t>(want)];
        if (p_.record) {
            std::byte buf[64];
            const std::size_t n = itch::encode_framed(m, buf);
            if (n == 0) return fail(kind, "encoder refused the message");
            wire_.insert(wire_.end(), buf, buf + n);
        }
        if (!p_.compare) return true;
        if (got != want) {
            return fail(kind, std::string("result ") + book::to_string(got) + " vs reference " + book::to_string(want));
        }
        for (const OrderRef r : touched) {
            if (!same_order(r)) return fail(kind, "order " + std::to_string(r) + " differs");
        }
        return compare_state(kind);
    }

    bool one() {
        ++step_;
        // Alternate fill and drain phases so ladders grow deep and then empty out.
        const bool draining = p_.phase_len != 0 && (step_ / p_.phase_len) % 2 == 1;
        const unsigned add_w = draining ? p_.drain_weight : p_.add_weight;
        const std::uint64_t r = rng_.bounded(add_w + 60);
        std::uint64_t edge = add_w;
        if (r < edge) return gen_add();
        if (r < (edge += 12)) return gen_delete();
        if (r < (edge += 12)) return gen_cancel();
        if (r < (edge += 12)) return gen_exec();
        if (r < (edge += 6)) return gen_exec_price();
        if (r < (edge += 12)) return gen_replace();
        if (r < (edge += 3)) return gen_trade();
        if (r < (edge += 2)) return gen_directory();
        return apply(itch::SystemEvent{header(0), 'O'}, "system");
    }

    bool gen_add() {
        itch::AddOrder m;
        m.h = header(pick_locate());
        m.ref = next_ref_++;
        const std::uint64_t k = rng_.bounded(100);
        if (k < 2) {
            if (const OrderRef live = pick_live(); live != 0) m.ref = live;  // duplicate of a live order
        } else if (k < 4) {
            m.ref = 1 + rng_.bounded(next_ref_ - 1);                          // live duplicate or a reused dead ref
        }
        m.side = coin() ? B : S;
        m.shares = pick_qty();
        if (m.shares == 0) ++zero_adds_;
        m.symbol = symbols_[rng_.bounded(kSymbols)];
        m.price = pick_price();
        m.has_attribution = rng_.chance(1, 4);
        if (m.has_attribution) std::memcpy(m.mpid, "MPID", 4);
        const bool had = ref_.contains(m.ref);
        const bool ok = apply(m, "add", {m.ref});
        if (!had && ref_.contains(m.ref)) pool_.push_back(m.ref);
        return ok;
    }

    bool gen_delete() {
        const OrderRef r = pick_target();
        return apply(itch::OrderDelete{header(lying_locate()), r}, "delete", {r});
    }

    bool gen_cancel() {
        const OrderRef r = pick_target();
        return apply(itch::OrderCancel{header(lying_locate()), r, reduction_for(r)}, "cancel", {r});
    }

    bool gen_exec() {
        const OrderRef r = pick_target();
        return apply(itch::OrderExecuted{header(lying_locate()), r, reduction_for(r), step_}, "execute", {r});
    }

    bool gen_exec_price() {
        const OrderRef r = pick_target();
        // Half the time at the order's own price, half anywhere: the level consumed
        // must be the order's, whatever the execution price says.
        const RefOrder* o = ref_.order(r);
        const Price px = (o != nullptr && coin()) ? o->price : pick_price() + static_cast<Price>(rng_.bounded(3));
        return apply(itch::OrderExecutedPrice{header(lying_locate()), r, reduction_for(r), step_, coin(), px},
                     "execute_price", {r});
    }

    bool gen_replace() {
        itch::OrderReplace m;
        m.h = header(lying_locate());
        m.old_ref = pick_target();
        m.new_ref = next_ref_++;
        const std::uint64_t k = rng_.bounded(100);
        if (k < 3) {
            m.new_ref = m.old_ref;                                              // equal to the old ref
        } else if (k < 6) {
            if (const OrderRef live = pick_live(); live != 0) m.new_ref = live; // collides with another order
        }
        m.shares = pick_qty();
        const RefOrder* o = ref_.order(m.old_ref);
        m.price = (o != nullptr && rng_.chance(1, 4)) ? o->price : pick_price();
        const bool had = ref_.contains(m.new_ref);
        const bool ok = apply(m, "replace", {m.old_ref, m.new_ref});
        if (!had && ref_.contains(m.new_ref)) pool_.push_back(m.new_ref);
        return ok;
    }

    bool gen_trade() {
        itch::Trade t;
        t.h = header(pick_locate());
        t.side = coin() ? B : S;
        t.shares = static_cast<Qty>(1 + rng_.bounded(1000));
        t.symbol = symbols_[rng_.bounded(kSymbols)];
        t.price = pick_price();
        t.match = step_;
        return apply(t, "trade");
    }

    bool gen_directory() {
        itch::StockDirectory d;
        d.h = header(pick_locate());
        d.symbol = rng_.chance(1, 10) ? Symbol() : symbols_[rng_.bounded(kSymbols)];
        return apply(d, "directory");
    }

    // ---- comparison ----
    bool fail(const char* kind, const std::string& what) {
        ot_test::fail(__FILE__, __LINE__,
                      "seed " + std::to_string(seed_) + " message " + std::to_string(step_) + " (" + kind + "): " + what);
        return false;
    }

    bool compare_side(Locate l, Side s, const OrderBook& b, const RefSide& r, const char* kind) {
        r.ladder_into(want_);
        const RefSide::Ladder& want = want_;
        // Built only on failure: this runs seven times per message.
        const auto where = [&] { return "locate " + std::to_string(l) + (s == B ? " bid " : " ask "); };
        if (b.depth(s) != want.size()) {
            return fail(kind, where() + "depth " + std::to_string(b.depth(s)) + " vs " + std::to_string(want.size()));
        }
        for (std::size_t i = 0; i < want.size(); ++i) {
            const Level got = b.level(s, i);
            if (got.price != want[i].first || got.qty != want[i].second.qty || got.orders != want[i].second.orders) {
                return fail(kind, where() + "level " + std::to_string(i) + " is {" + std::to_string(got.price) + "," +
                                      std::to_string(got.qty) + "," + std::to_string(got.orders) + "} vs {" +
                                      std::to_string(want[i].first) + "," + std::to_string(want[i].second.qty) + "," +
                                      std::to_string(want[i].second.orders) + "}");
            }
            if (i > 0 && !(s == B ? b.level(s, i - 1).price > got.price : b.level(s, i - 1).price < got.price)) {
                return fail(kind, where() + "not strictly ordered");
            }
        }
        const std::optional<Level> best = b.best(s);
        if (best.has_value() != !want.empty() || (best && !(*best == b.level(s, 0)))) return fail(kind, where() + "best()");
        if (want.size() > b.max_levels_per_side()) return fail(kind, where() + "exceeds the level limit");

        // Point queries: every price of the active range plus one either side.
        std::fill(expect_.begin(), expect_.end(), std::uint64_t{0});
        for (const auto& w : want) expect_[static_cast<std::size_t>(w.first - (p_.base - 1))] = w.second.qty;
        for (Price px = p_.base - 1; px <= p_.base + p_.range; ++px) {
            if (b.qty_at(s, px) != expect_[static_cast<std::size_t>(px - (p_.base - 1))]) {
                return fail(kind, where() + "qty_at(" + std::to_string(px) + ")");
            }
        }
        for (const std::size_t top : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{5}, std::size_t{1000}}) {
            std::uint64_t sum = 0;
            for (std::size_t i = 0; i < want.size() && i < top; ++i) sum += want[i].second.qty;
            if (b.total_qty(s, top) != std::min(sum, kQtyMax)) return fail(kind, where() + "total_qty(" + std::to_string(top) + ")");
        }
        return true;
    }

    bool compare_state(const char* kind) {
        if (mb_.live_orders() != ref_.orders().size()) {
            return fail(kind, "live_orders " + std::to_string(mb_.live_orders()) + " vs " + std::to_string(ref_.orders().size()));
        }
        if (mb_.applied() != ref_.applied()) return fail(kind, "applied()");
        if (mb_.overflows() != ref_.overflows()) return fail(kind, "overflows()");
        for (std::size_t wi = 0; wi < watch_.size(); ++wi) {
            const Locate l = watch_[wi];
            const OrderBook* b = mb_.book(l);
            const RefBook* r = ref_.find_book(l);
            if ((b != nullptr) != (r != nullptr)) return fail(kind, "book presence for locate " + std::to_string(l));
            if (mb_.last_trade(l) != ref_.last_trade(l)) return fail(kind, "last_trade for locate " + std::to_string(l));
            if (!(mb_.symbol(l) == ref_.symbol_of(l))) return fail(kind, "symbol for locate " + std::to_string(l));
            if (b == nullptr) continue;
            if (!compare_side(l, B, *b, r->bids, kind) || !compare_side(l, S, *b, r->asks, kind)) return false;
            const auto bb = r->bids.best();
            const auto aa = r->asks.best();
            if (b->crossed() != (bb && aa && *bb >= *aa)) {
                return fail(kind, "crossed() for locate " + std::to_string(l));
            }
            // Track how often a side drains completely: the "levels empty out" coverage.
            for (int s = 0; s < 2; ++s) {
                const std::size_t d = b->depth(s == 0 ? B : S);
                if (d == 0 && seen_depth_[wi][s] != 0) ++empties_;
                seen_depth_[wi][s] = d;
            }
        }
        for (const Symbol& sym : symbols_) {
            if (mb_.locate(sym) != ref_.locate_of(sym)) return fail(kind, "locate(symbol " + std::string(sym.view()) + ")");
        }
        if (p_.sums_every != 0 && step_ % p_.sums_every == 0) return sweep_orders(step_ % 1000 == 0);
        return true;
    }

    bool same_order(OrderRef r) const {
        const MarketBooks::OrderInfo* got = mb_.order(r);
        const RefOrder* want = ref_.order(r);
        if (got == nullptr || want == nullptr) return got == nullptr && want == nullptr;
        return got->locate == want->locate && got->side == want->side && got->price == want->price && got->qty == want->qty;
    }

    // Order table: every reference order must match, and no dead reference may be alive.
    bool sweep_orders(bool scan_dead_refs) {
        for (const auto& kv : ref_.orders()) {
            if (!same_order(kv.first)) return fail("sweep", "order " + std::to_string(kv.first) + " differs");
        }
        for (OrderRef r = 1; scan_dead_refs && r <= next_ref_ + 3; ++r) {
            if (!ref_.contains(r) && mb_.order(r) != nullptr) return fail("sweep", "order " + std::to_string(r) + " should be dead");
        }
        // Recompute every tracked level from the orders alone: its quantity and order
        // count must be exactly what the live orders added to that incarnation of the
        // level still hold. Orders on evicted levels (older incarnation) are excluded
        // by construction. This checks the reference itself, not only the books.
        {
            std::map<std::tuple<Locate, int, std::uint64_t>, std::pair<std::uint64_t, std::uint32_t>> per_level;
            for (const auto& kv : ref_.orders()) {
                if (kv.second.gen == 0) continue;
                auto& acc = per_level[{kv.second.locate, kv.second.side == B ? 0 : 1, kv.second.gen}];
                acc.first += kv.second.qty;
                ++acc.second;
            }
            for (const Locate l : watch_) {
                const RefBook* rb = ref_.find_book(l);
                if (rb == nullptr) continue;
                for (int s = 0; s < 2; ++s) {
                    (s == 0 ? rb->bids : rb->asks).ladder_into(want_);
                    for (const auto& level : want_) {
                        const auto it = per_level.find({l, s, level.second.gen});
                        if (it == per_level.end() || it->second.first != level.second.qty ||
                            it->second.second != level.second.orders) {
                            return fail("sweep", "tracked level does not equal the sum of its live orders");
                        }
                    }
                }
            }
        }
        // The ladder never shows more than the live orders hold, per instrument and side.
        std::map<std::pair<Locate, int>, std::uint64_t> live;
        for (const auto& kv : ref_.orders()) live[{kv.second.locate, kv.second.side == B ? 0 : 1}] += kv.second.qty;
        for (const Locate l : watch_) {
            const OrderBook* b = mb_.book(l);
            if (b == nullptr) continue;
            for (int s = 0; s < 2; ++s) {
                std::uint64_t shown = 0;
                for (std::size_t i = 0; i < b->depth(s == 0 ? B : S); ++i) shown += b->level(s == 0 ? B : S, i).qty;
                if (shown > live[{l, s}]) return fail("sweep", "ladder shows more than the live orders hold");
            }
        }
        return true;
    }

    Params p_;
    std::uint64_t seed_;
    Rng rng_;
    MarketBooks mb_;
    Reference ref_;
    std::vector<Locate> watch_;
    std::vector<Symbol> symbols_;
    std::vector<OrderRef> pool_;
    std::vector<std::byte> wire_;
    std::array<std::uint64_t, 7> hist_{};
    std::vector<std::array<std::size_t, 2>> seen_depth_;
    RefSide::Ladder want_;                    // scratch for compare_side
    std::vector<std::uint64_t> expect_;       // scratch: expected qty per price of the active range
    std::uint64_t empties_{0};
    std::uint64_t zero_adds_{0};
    std::uint64_t step_{0};
    OrderRef next_ref_{1};
};

std::uint64_t hist(const Driver& d, Applied a) { return d.histogram()[static_cast<std::size_t>(a)]; }

}  // namespace

OT_TEST(differential_wide_capacity) {
    // No limit is ever reached: this is plain aggregation, checked message by message.
    std::uint64_t total = 0, ok = 0, unknown = 0, dup = 0, invalid = 0, ignored = 0, empties = 0;
    for (std::uint64_t seed = 1; seed <= 5; ++seed) {
        Params p;
        Driver d(p, seed);
        OT_CHECK(d.run(72'000));
        total += d.messages();
        ok += hist(d, Applied::ok);
        unknown += hist(d, Applied::unknown_order);
        dup += hist(d, Applied::duplicate_order);
        invalid += hist(d, Applied::invalid);
        ignored += hist(d, Applied::ignored);
        empties += d.empties();
        OT_CHECK_EQ(hist(d, Applied::unknown_symbol), std::uint64_t{0});
    }
    OT_CHECK(total >= 360'000);
    // The stream must actually visit every result class, or a pass would mean little.
    OT_CHECK(ok > 130'000);
    OT_CHECK(unknown > 5'000);
    OT_CHECK(dup > 1'000);
    OT_CHECK(invalid > 5'000);
    OT_CHECK(ignored > 5'000);
    OT_CHECK(empties > 1'000);
}

OT_TEST(differential_more_instruments_and_a_wider_range) {
    Params p;
    p.locates = 12;
    p.range = 30;
    Driver d(p, 77);
    OT_CHECK(d.run(20'000));
    OT_CHECK(hist(d, Applied::ok) > 6'000);
}

OT_TEST(differential_small_order_table) {
    // 40 live orders at most: the table is full most of the time.
    std::uint64_t capacity = 0;
    for (std::uint64_t seed = 101; seed <= 103; ++seed) {
        Params p;
        p.max_orders = 40;
        p.add_weight = 50;
        p.phase_len = 0;
        Driver d(p, seed);
        OT_CHECK(d.run(30'000));
        capacity += hist(d, Applied::capacity);
        OT_CHECK(d.books().live_orders() <= 40);
    }
    OT_CHECK(capacity > 1'500);
}

OT_TEST(differential_small_symbol_table) {
    // Room for three of the six symbols the stream uses: directory messages hit `capacity`.
    std::uint64_t capacity = 0;
    for (std::uint64_t seed = 401; seed <= 402; ++seed) {
        Params p;
        p.max_symbols = 3;
        Driver d(p, seed);
        OT_CHECK(d.run(15'000));
        capacity += hist(d, Applied::capacity);
        std::size_t known = 0;
        for (unsigned i = 0; i < 6; ++i) known += d.books().locate(Symbol("S" + std::to_string(i))).has_value() ? 1 : 0;
        OT_CHECK_EQ(known, std::size_t{3});
    }
    OT_CHECK(capacity > 50);
}

OT_TEST(overflow_policy_matches_the_model) {
    // Four levels per side against a twelve-tick range: refusals, evictions, orders
    // stranded off the ladder and price levels coming back after eviction, all the time.
    std::uint64_t capacity = 0, overflows = 0;
    for (std::uint64_t seed = 201; seed <= 204; ++seed) {
        Params p;
        p.max_levels = 4;
        p.sums_every = 5;  // full order-table sweep every fifth message, the touched orders after every one
        Driver d(p, seed);
        OT_CHECK(d.run(30'000));
        capacity += hist(d, Applied::capacity);
        overflows += d.books().overflows();
        OT_CHECK_EQ(d.books().overflows(), d.reference().overflows());
        for (Locate l = 0; l < 5; ++l) {
            const OrderBook* b = d.books().book(l);
            if (b == nullptr) continue;
            OT_CHECK(b->depth(B) <= 4 && b->depth(S) <= 4);
        }
    }
    OT_CHECK(capacity > 3'000);
    OT_CHECK(overflows > 3'000);
}

OT_TEST(overflow_with_one_level_and_none) {
    for (std::size_t levels : {std::size_t{1}, std::size_t{2}, std::size_t{0}}) {
        Params p;
        p.max_levels = levels;
        p.range = 6;
        Driver d(p, 300 + levels);
        OT_CHECK(d.run(15'000));
        if (levels == 0) {
            OT_CHECK(hist(d, Applied::ok) > 0);  // deletes, executes and cancels of tracked orders still work
            for (Locate l = 0; l < 5; ++l) {
                if (d.books().book(l) != nullptr) OT_CHECK_EQ(d.books().book(l)->depth(B), std::size_t{0});
            }
        }
    }
}

OT_TEST(decoded_wire_stream_ends_in_the_same_state_as_direct_calls) {
    Params p;
    p.record = true;
    Driver d(p, 42);
    OT_CHECK(d.run(60'000));

    struct Feed : itch::NullHandler {
        using itch::NullHandler::on;
        explicit Feed(MarketBooks& b) : books(b) {}
        MarketBooks& books;
        void on(const itch::StockDirectory& m) noexcept { books.on(m); }
        void on(const itch::AddOrder& m) noexcept { books.on(m); }
        void on(const itch::OrderExecuted& m) noexcept { books.on(m); }
        void on(const itch::OrderExecutedPrice& m) noexcept { books.on(m); }
        void on(const itch::OrderCancel& m) noexcept { books.on(m); }
        void on(const itch::OrderDelete& m) noexcept { books.on(m); }
        void on(const itch::OrderReplace& m) noexcept { books.on(m); }
        void on(const itch::Trade& m) noexcept { books.on(m); }
        void on(const itch::SystemEvent& m) noexcept { books.on(m); }
    };
    MarketBooks fresh(MarketBooks::Config{p.max_orders, p.max_levels, p.max_symbols});
    Feed feed(fresh);
    const itch::StreamResult r = itch::decode_stream(std::span<const std::byte>(d.wire()), feed);
    OT_CHECK_EQ(r.consumed, d.wire().size());
    // The only frames the decoder drops are the zero-share adds the books would reject anyway.
    OT_CHECK(d.zero_share_adds() > 50);
    OT_CHECK_EQ(r.skipped, static_cast<std::size_t>(d.zero_share_adds()));
    OT_CHECK_EQ(r.messages, static_cast<std::size_t>(d.messages() - d.zero_share_adds()));

    const MarketBooks& direct = d.books();
    OT_CHECK_EQ(fresh.live_orders(), direct.live_orders());
    OT_CHECK_EQ(fresh.applied(), direct.applied());
    OT_CHECK_EQ(fresh.overflows(), direct.overflows());
    for (const Locate l : {Locate{0}, Locate{1}, Locate{2}, Locate{3}, Locate{4}, Locate{40000}, Locate{65535}}) {
        OT_CHECK_EQ(fresh.last_trade(l), direct.last_trade(l));
        OT_CHECK_EQ(fresh.book(l) != nullptr, direct.book(l) != nullptr);
        if (fresh.book(l) == nullptr) continue;
        for (const Side s : {B, S}) {
            OT_CHECK_EQ(fresh.book(l)->depth(s), direct.book(l)->depth(s));
            for (std::size_t i = 0; i < direct.book(l)->depth(s); ++i) {
                OT_CHECK(fresh.book(l)->level(s, i) == direct.book(l)->level(s, i));
            }
        }
    }
}

// Throughput of decode + book update on a pre-generated stream. Not asserted:
// the figure depends on the build (sanitizers make it meaningless) and the host.
// Run with OT_TIMING=1 in a Release build for a number worth quoting.
OT_TEST(timing_smoke) {
    const char* env = std::getenv("OT_TIMING");
    if (env == nullptr || std::strcmp(env, "1") != 0) {
        std::printf("  timing_smoke skipped (set OT_TIMING=1 to run)\n");
        return;
    }
    Params p;
    p.compare = false;
    p.record = true;
    p.max_levels = 64;
    p.range = 40;
    Driver d(p, 2024);
    OT_CHECK(d.run(1'000'000));
    const std::span<const std::byte> stream(d.wire());

    struct Counting : itch::NullHandler {
        using itch::NullHandler::on;
        explicit Counting(MarketBooks& b) : books(b) {}
        MarketBooks& books;
        void on(const itch::StockDirectory& m) noexcept { books.on(m); }
        void on(const itch::AddOrder& m) noexcept { books.on(m); }
        void on(const itch::OrderExecuted& m) noexcept { books.on(m); }
        void on(const itch::OrderExecutedPrice& m) noexcept { books.on(m); }
        void on(const itch::OrderCancel& m) noexcept { books.on(m); }
        void on(const itch::OrderDelete& m) noexcept { books.on(m); }
        void on(const itch::OrderReplace& m) noexcept { books.on(m); }
        void on(const itch::Trade& m) noexcept { books.on(m); }
    };

    double best = 0;
    std::size_t messages = 0;
    for (int pass = 0; pass < 5; ++pass) {
        MarketBooks mb(MarketBooks::Config{p.max_orders, p.max_levels, p.max_symbols});
        Counting sink(mb);
        const auto t0 = std::chrono::steady_clock::now();
        const itch::StreamResult r = itch::decode_stream(stream, sink);
        const auto t1 = std::chrono::steady_clock::now();
        messages = r.messages;
        const double secs = std::chrono::duration<double>(t1 - t0).count();
        best = std::max(best, static_cast<double>(r.messages) / secs);
    }
    std::printf("  timing_smoke: %zu messages, best of 5 passes %.2f M msgs/s (decode + book, one thread)\n", messages,
                best / 1e6);
}

OT_TEST_MAIN()

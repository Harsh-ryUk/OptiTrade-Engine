#pragma once

// Fuzz body for the order books, shared by the libFuzzer entry point
// (fuzz_book.cpp) and the deterministic smoke test (test_book_fuzz_smoke.cpp).
//
// Every input is examined twice, each time against a fresh MarketBooks with tiny
// limits (8 orders, 2 levels per side, 3 symbols) so that the capacity, eviction
// and stranded-order paths are reached by short inputs:
//
//  * raw: the bytes are a BinaryFILE ITCH stream, decoded straight into the books.
//    Most random streams are noise; coverage-guided mutation is what finds the
//    valid ones.
//  * structured: the bytes are read as 7-byte operations over a handful of
//    locates, references, prices and sizes, turned into real ITCH messages,
//    framed, and pushed through the same decoder. Random bytes make plausible
//    traffic (executes of live orders, replaces, over-cancels) this way.
//
// Nothing is compared with a second implementation here (the differential test
// does that). The body checks, after every message, properties that must hold
// whatever the input, and aborts on the first violation:
//
//  * the result code is one the message type can produce, and it agrees with what
//    happened to the order table: live_orders() moved by exactly the amount the
//    result implies, the touched order has exactly the expected fields, rejected
//    messages changed nothing;
//  * for the touched instrument, both ladders are strictly sorted from the best
//    price, hold at most the level limit, contain no empty level, agree with
//    best(), qty_at() and total_qty(), and crossed() agrees with the best prices;
//  * the quantity shown on a ladder never exceeds the quantity of the live orders
//    on that side, and the order count of a ladder never exceeds their number;
//  * live_orders() equals the number of references that are actually live, and
//    applied() equals the number of order-flow messages that returned ok;
//  * last_trade() moves exactly on executions, trades and nothing else.
//
// Sanitizers cover memory safety; this covers the logic.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "optitrade/book/market_books.hpp"
#include "optitrade/itch/decoder.hpp"
#include "optitrade/itch/encoder.hpp"

#define BOOK_FUZZ_CHECK(cond)                                                                   \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "book fuzz invariant violated: %s (%s:%d)\n", #cond, __FILE__, \
                         __LINE__);                                                             \
            std::abort();                                                                       \
        }                                                                                       \
    } while (0)

namespace optitrade::fuzz {

// What the corpus reached; the smoke test asserts on it so a generator that stops
// exercising the interesting paths cannot pass unnoticed.
struct BookFuzzTally {
    std::array<std::uint64_t, 7> by_result{};  // indexed by book::Applied, all message types
    std::uint64_t messages{};                  // messages that reached the books
    std::uint64_t table_full{};                // adds refused because the order table was full
    std::uint64_t stranded{};                  // orders held in the table but not on a ladder
    std::uint64_t overflows{};                 // OrderBook::overflows() summed over inputs
    std::uint64_t empty_ladders_after_fill{};  // a ladder that went from non-empty to empty
};

namespace book_impl {

using Bytes = std::span<const std::byte>;
using book::Applied;
using book::MarketBooks;
using book::OrderBook;
using Info = MarketBooks::OrderInfo;

inline constexpr std::size_t kMaxOrders = 8;
inline constexpr std::size_t kMaxLevels = 2;
inline constexpr std::size_t kMaxSymbols = 3;

inline MarketBooks::Config small_config() { return {kMaxOrders, kMaxLevels, kMaxSymbols}; }

inline std::optional<Info> snapshot(const MarketBooks& b, OrderRef ref) {
    const Info* o = b.order(ref);
    if (o == nullptr) return std::nullopt;
    return *o;
}

inline bool same(const std::optional<Info>& a, const std::optional<Info>& b) {
    if (a.has_value() != b.has_value()) return false;
    return !a || (a->locate == b->locate && a->side == b->side && a->price == b->price && a->qty == b->qty);
}

class Checker : public itch::NullHandler {
public:
    Checker(MarketBooks& books, BookFuzzTally& tally) : books_(books), tally_(tally) {}

    using itch::NullHandler::on;

    std::uint64_t delivered() const noexcept { return delivered_; }

    void on(const itch::StockDirectory& m) noexcept {
        const std::optional<Locate> located_before = books_.locate(m.symbol);
        const Symbol symbol_before = books_.symbol(m.h.locate);
        const bool book_before = books_.book(m.h.locate) != nullptr;
        const Applied r = books_.on(m);
        record(r);
        switch (r) {
            case Applied::ok:
                BOOK_FUZZ_CHECK(!located_before && symbol_before.empty());
                BOOK_FUZZ_CHECK(books_.locate(m.symbol) == m.h.locate);
                BOOK_FUZZ_CHECK(books_.symbol(m.h.locate) == m.symbol);
                BOOK_FUZZ_CHECK(books_.book(m.h.locate) != nullptr);
                break;
            case Applied::ignored:
                BOOK_FUZZ_CHECK(located_before == m.h.locate);
                BOOK_FUZZ_CHECK(books_.symbol(m.h.locate) == m.symbol);
                break;
            case Applied::invalid:
            case Applied::capacity:
                // A refused entry changes no mapping and creates no book.
                BOOK_FUZZ_CHECK(books_.locate(m.symbol) == located_before);
                BOOK_FUZZ_CHECK(books_.symbol(m.h.locate) == symbol_before);
                BOOK_FUZZ_CHECK((books_.book(m.h.locate) != nullptr) == book_before);
                break;
            default: BOOK_FUZZ_CHECK(false);
        }
        touch(m.h.locate);
        verify(m.h.locate);
    }

    void on(const itch::AddOrder& m) noexcept {
        const std::size_t live_before = books_.live_orders();
        const std::optional<Info> prior = snapshot(books_, m.ref);
        const Price trade_before = books_.last_trade(m.h.locate);
        const Applied r = books_.on(m);
        record(r);
        order_flow(r);
        const std::size_t live_after = books_.live_orders();
        const Info want{m.h.locate, m.side, m.price, m.shares};
        BOOK_FUZZ_CHECK(books_.last_trade(m.h.locate) == trade_before);
        switch (r) {
            case Applied::ok:
                BOOK_FUZZ_CHECK(!prior && live_after == live_before + 1);
                BOOK_FUZZ_CHECK(same(snapshot(books_, m.ref), want));
                break;
            case Applied::capacity:
                BOOK_FUZZ_CHECK(!prior);
                if (live_after == live_before) {
                    // Refused outright: only a full table does that (the book itself is tiny).
                    BOOK_FUZZ_CHECK(live_before == kMaxOrders && !books_.order(m.ref));
                    ++tally_.table_full;
                } else {
                    // Tracked, but its level did not fit on the ladder.
                    BOOK_FUZZ_CHECK(live_after == live_before + 1 && same(snapshot(books_, m.ref), want));
                    ++tally_.stranded;
                }
                break;
            case Applied::duplicate_order:
                BOOK_FUZZ_CHECK(prior && live_after == live_before && same(snapshot(books_, m.ref), prior));
                break;
            default: BOOK_FUZZ_CHECK(false);  // the decoder never yields a zero-share or one-sided add
        }
        track(m.ref);
        verify_live_count();
        touch(m.h.locate);
        verify(m.h.locate);
    }

    // E and C share the reduction rules; C also names the execution price.
    void on(const itch::OrderExecuted& m) noexcept { reduce(m.ref, m.shares, /*trade_price=*/nullptr, true, [&] { return books_.on(m); }); }
    void on(const itch::OrderExecutedPrice& m) noexcept {
        reduce(m.ref, m.shares, &m.price, true, [&] { return books_.on(m); });
    }
    void on(const itch::OrderCancel& m) noexcept { reduce(m.ref, m.shares, nullptr, false, [&] { return books_.on(m); }); }

    void on(const itch::OrderDelete& m) noexcept {
        const std::size_t live_before = books_.live_orders();
        const std::optional<Info> prior = snapshot(books_, m.ref);
        const Applied r = books_.on(m);
        record(r);
        order_flow(r);
        if (r == Applied::ok) {
            BOOK_FUZZ_CHECK(prior && !books_.order(m.ref) && books_.live_orders() == live_before - 1);
        } else {
            BOOK_FUZZ_CHECK(r == Applied::unknown_order && !prior && books_.live_orders() == live_before);
        }
        verify_live_count();
        if (prior) {
            touch(prior->locate);
            verify(prior->locate);
        }
    }

    void on(const itch::OrderReplace& m) noexcept {
        const std::size_t live_before = books_.live_orders();
        const std::optional<Info> prior = snapshot(books_, m.old_ref);
        const std::optional<Info> collides = snapshot(books_, m.new_ref);
        const Applied r = books_.on(m);
        record(r);
        order_flow(r);
        const std::size_t live_after = books_.live_orders();
        switch (r) {
            case Applied::unknown_order:
                BOOK_FUZZ_CHECK(!prior && live_after == live_before && same(snapshot(books_, m.new_ref), collides));
                break;
            case Applied::invalid:
                BOOK_FUZZ_CHECK(prior && m.shares == 0 && live_after == live_before);
                BOOK_FUZZ_CHECK(same(snapshot(books_, m.old_ref), prior));
                break;
            case Applied::duplicate_order:
                // Nothing moves, including the order that was to be replaced.
                BOOK_FUZZ_CHECK(prior && collides && live_after == live_before);
                BOOK_FUZZ_CHECK(same(snapshot(books_, m.old_ref), prior));
                break;
            case Applied::ok:
            case Applied::capacity: {
                BOOK_FUZZ_CHECK(prior && !collides && m.old_ref != m.new_ref);
                BOOK_FUZZ_CHECK(live_after == live_before && !books_.order(m.old_ref));
                // Side and instrument are inherited from the replaced order.
                BOOK_FUZZ_CHECK(same(snapshot(books_, m.new_ref), Info{prior->locate, prior->side, m.price, m.shares}));
                if (r == Applied::capacity) ++tally_.stranded;
                break;
            }
            default: BOOK_FUZZ_CHECK(false);
        }
        track(m.new_ref);
        verify_live_count();
        if (prior) {
            touch(prior->locate);
            verify(prior->locate);
        }
    }

    void on(const itch::Trade& m) noexcept {
        const std::size_t live_before = books_.live_orders();
        const Applied r = books_.on(m);
        record(r);
        BOOK_FUZZ_CHECK(r == Applied::ignored && books_.live_orders() == live_before);
        BOOK_FUZZ_CHECK(books_.last_trade(m.h.locate) == m.price);
        touch(m.h.locate);
        verify(m.h.locate);
    }

    void on(const itch::SystemEvent& m) noexcept {
        const std::size_t live_before = books_.live_orders();
        const Applied r = books_.on(m);
        record(r);
        BOOK_FUZZ_CHECK(r == Applied::ignored && books_.live_orders() == live_before);
    }

    // Everything the whole-state checks can see, once more at the end of the input.
    void finish() noexcept {
        verify_all();
        tally_.overflows += books_.overflows();
        for (const Locate l : touched_) {
            const Symbol s = books_.symbol(l);
            if (!s.empty()) BOOK_FUZZ_CHECK(books_.locate(s) == l);
        }
    }

private:
    void record(Applied r) noexcept {
        ++tally_.by_result[static_cast<std::size_t>(r)];
        ++tally_.messages;
        ++delivered_;
    }
    void order_flow(Applied r) noexcept {
        if (r == Applied::ok) ++ok_flow_;
        BOOK_FUZZ_CHECK(books_.applied() == ok_flow_);
    }
    void track(OrderRef ref) {
        if (std::find(seen_.begin(), seen_.end(), ref) == seen_.end()) seen_.push_back(ref);
    }
    void touch(Locate l) {
        if (std::find(touched_.begin(), touched_.end(), l) != touched_.end()) return;
        touched_.push_back(l);
        last_depth_.push_back({0, 0});
    }

    template <class Apply>
    void reduce(OrderRef ref, Qty shares, const Price* trade_price, bool execution, Apply&& apply) noexcept {
        const std::size_t live_before = books_.live_orders();
        const std::optional<Info> prior = snapshot(books_, ref);
        const Price last_before = prior ? books_.last_trade(prior->locate) : 0;
        const Applied r = apply();
        record(r);
        order_flow(r);
        const std::size_t live_after = books_.live_orders();
        switch (r) {
            case Applied::unknown_order:
                BOOK_FUZZ_CHECK(!prior && live_after == live_before);
                break;
            case Applied::invalid:
                BOOK_FUZZ_CHECK(prior);
                if (shares == 0) {
                    // A zero-size message says nothing about the order.
                    BOOK_FUZZ_CHECK(live_after == live_before && same(snapshot(books_, ref), prior));
                } else {
                    // More than remained: the order is dropped, not half trusted.
                    BOOK_FUZZ_CHECK(shares > prior->qty && live_after == live_before - 1 && !books_.order(ref));
                }
                BOOK_FUZZ_CHECK(books_.last_trade(prior->locate) == last_before);
                break;
            case Applied::ok: {
                BOOK_FUZZ_CHECK(prior && shares != 0 && shares <= prior->qty);
                if (shares == prior->qty) {
                    BOOK_FUZZ_CHECK(!books_.order(ref) && live_after == live_before - 1);
                } else {
                    BOOK_FUZZ_CHECK(live_after == live_before);
                    BOOK_FUZZ_CHECK(same(snapshot(books_, ref), Info{prior->locate, prior->side, prior->price, prior->qty - shares}));
                }
                const Price expect = !execution ? last_before : trade_price != nullptr ? *trade_price : prior->price;
                BOOK_FUZZ_CHECK(books_.last_trade(prior->locate) == expect);
                break;
            }
            default: BOOK_FUZZ_CHECK(false);
        }
        verify_live_count();
        if (prior) {
            touch(prior->locate);
            verify(prior->locate);
        }
    }

    // No phantom or lost orders: the table holds exactly the references that answer.
    void verify_live_count() noexcept {
        std::size_t live = 0;
        for (const OrderRef r : seen_) live += books_.order(r) != nullptr ? 1 : 0;
        BOOK_FUZZ_CHECK(live == books_.live_orders());
    }

    void verify_all() noexcept {
        verify_live_count();
        for (const Locate l : touched_) verify(l);
    }

    void verify(Locate l) noexcept {
        const OrderBook* b = books_.book(l);
        if (b == nullptr) return;
        for (const Side side : {Side::buy, Side::sell}) {
            const std::size_t depth = b->depth(side);
            BOOK_FUZZ_CHECK(depth <= kMaxLevels);
            std::uint64_t shown = 0;
            std::uint64_t shown_orders = 0;
            for (std::size_t i = 0; i < depth; ++i) {
                const book::Level lv = b->level(side, i);
                BOOK_FUZZ_CHECK(lv.qty > 0 && lv.orders >= 1);
                if (i > 0) {  // best first, strictly: bids fall, asks rise
                    const Price prev = b->level(side, i - 1).price;
                    BOOK_FUZZ_CHECK(side == Side::buy ? prev > lv.price : prev < lv.price);
                }
                BOOK_FUZZ_CHECK(b->qty_at(side, lv.price) == lv.qty);
                shown += lv.qty;
                shown_orders += lv.orders;
            }
            const std::optional<book::Level> best = b->best(side);
            BOOK_FUZZ_CHECK(best.has_value() == (depth != 0));
            if (best) BOOK_FUZZ_CHECK(*best == b->level(side, 0));
            BOOK_FUZZ_CHECK(b->total_qty(side, kMaxLevels) ==
                            static_cast<Qty>(std::min<std::uint64_t>(shown, std::numeric_limits<Qty>::max())));

            std::uint64_t live_qty = 0;
            std::uint64_t live_count = 0;
            for (const OrderRef r : seen_) {
                const Info* o = books_.order(r);
                if (o != nullptr && o->locate == l && o->side == side) {
                    live_qty += o->qty;
                    ++live_count;
                }
            }
            BOOK_FUZZ_CHECK(shown <= live_qty);
            BOOK_FUZZ_CHECK(shown_orders <= live_count);

            std::size_t& before = last_depth_[position(l)][side == Side::buy ? 0 : 1];
            if (depth == 0 && before != 0) ++tally_.empty_ladders_after_fill;
            before = depth;
        }
        const auto bb = b->best(Side::buy);
        const auto aa = b->best(Side::sell);
        BOOK_FUZZ_CHECK(b->crossed() == (bb && aa && bb->price >= aa->price));
    }

    // Index of an instrument in touched_; verify() is only called for touched ones.
    std::size_t position(Locate l) const {
        const auto it = std::find(touched_.begin(), touched_.end(), l);
        BOOK_FUZZ_CHECK(it != touched_.end());
        return static_cast<std::size_t>(it - touched_.begin());
    }

    MarketBooks& books_;
    BookFuzzTally& tally_;
    std::vector<OrderRef> seen_;       // every reference an add or replace could have created
    std::vector<Locate> touched_;      // instruments seen so far
    std::vector<std::array<std::size_t, 2>> last_depth_;  // per touched instrument: bid, ask depth after the last check
    std::uint64_t ok_flow_{0};
    std::uint64_t delivered_{0};
};

inline void run_stream(Bytes stream, BookFuzzTally& tally) {
    MarketBooks books(small_config());
    Checker checker(books, tally);
    const itch::StreamResult r = itch::decode_stream(stream, checker);
    BOOK_FUZZ_CHECK(r.consumed <= stream.size());
    BOOK_FUZZ_CHECK(checker.delivered() == r.messages);
    checker.finish();
}

// ---- structured mode -------------------------------------------------------

inline constexpr std::size_t kOpSize = 7;

inline Qty op_qty(std::uint8_t b) noexcept {
    if (b == 0) return 0;
    if (b == 255) return 0xFFFFFFF0u;  // near the top of Qty: level saturation
    return b;
}
inline Price op_price(std::uint8_t b) noexcept {
    if (b == 255) return 0xFFFFFFFFll;  // wire maximum
    if (b == 254) return 0;
    return 100 + (b % 6);
}

// Appends the framed ITCH message for one 7-byte operation:
//   [0] kind  [1] locate  [2] reference  [3] second reference (replace)
//   [4] shares  [5] price  [6] side / symbol / attribution / system-event flags
inline void append_op(std::vector<std::byte>& out, const std::uint8_t* op) {
    std::array<std::byte, 64> buf{};
    const itch::Header h{static_cast<Locate>(op[1] & 0x07u), 0, 0};
    const OrderRef ref_a = 1 + op[2] % 12u;
    const OrderRef ref_b = 1 + op[3] % 12u;
    const Side side = (op[6] & 1u) != 0 ? Side::sell : Side::buy;
    const Symbol symbol("SYM" + std::to_string((op[6] >> 2) & 3u));
    std::size_t n = 0;
    switch (op[0] % 11u) {
        case 0:
        case 1:
        case 2: {
            itch::AddOrder m;
            m.h = h;
            m.ref = ref_a;
            m.side = side;
            m.shares = op_qty(op[4]);
            m.symbol = symbol;
            m.price = op_price(op[5]);
            m.has_attribution = (op[6] & 0x20u) != 0;
            n = itch::encode_framed(m, buf);
            break;
        }
        case 3: n = itch::encode_framed(itch::OrderExecuted{h, ref_a, op_qty(op[4]), 1}, buf); break;
        case 4: n = itch::encode_framed(itch::OrderExecutedPrice{h, ref_a, op_qty(op[4]), 1, true, op_price(op[5])}, buf); break;
        case 5: n = itch::encode_framed(itch::OrderCancel{h, ref_a, op_qty(op[4])}, buf); break;
        case 6: n = itch::encode_framed(itch::OrderDelete{h, ref_a}, buf); break;
        case 7:
        case 8: n = itch::encode_framed(itch::OrderReplace{h, ref_a, ref_b, op_qty(op[4]), op_price(op[5])}, buf); break;
        case 9: {
            itch::Trade t;
            t.h = h;
            t.side = side;
            t.shares = 1;
            t.symbol = symbol;
            t.price = op_price(op[5]);
            n = itch::encode_framed(t, buf);
            break;
        }
        default: {
            if ((op[6] & 0x10u) != 0) {
                n = itch::encode_framed(itch::SystemEvent{h, 'O'}, buf);
            } else {
                itch::StockDirectory d;
                d.h = h;
                d.symbol = symbol;
                n = itch::encode_framed(d, buf);
            }
            break;
        }
    }
    out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
}

}  // namespace book_impl

// Statistics-collecting form; the smoke test uses it to check coverage.
inline void book_one_tally(const std::uint8_t* data, std::size_t size, BookFuzzTally& tally) {
    static_assert(sizeof(std::uint8_t) == sizeof(std::byte));

    // Raw: the input as a BinaryFILE stream.
    book_impl::run_stream(book_impl::Bytes(reinterpret_cast<const std::byte*>(data), size), tally);

    // Structured: the input as a list of operations, re-encoded as a stream.
    static thread_local std::vector<std::byte> stream;
    stream.clear();
    for (std::size_t off = 0; off + book_impl::kOpSize <= size; off += book_impl::kOpSize) {
        book_impl::append_op(stream, data + off);
    }
    book_impl::run_stream(book_impl::Bytes(stream), tally);
}

inline void book_one(const std::uint8_t* data, std::size_t size) {
    BookFuzzTally tally;
    book_one_tally(data, size, tally);
}

}  // namespace optitrade::fuzz

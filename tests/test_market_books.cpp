// MarketBooks: one behaviour per test, with every expected ladder worked out by
// hand. The randomized comparison against a reference model lives in
// test_book_differential.cpp.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "check.hpp"
#include "optitrade/book/market_books.hpp"
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

MarketBooks::Config small(std::size_t orders = 64, std::size_t levels = 16) { return {orders, levels}; }

itch::Header hdr(Locate l) { return itch::Header{l, 0, 0}; }

itch::AddOrder add(Locate l, OrderRef ref, Side side, Qty qty, Price px) {
    itch::AddOrder m;
    m.h = hdr(l);
    m.ref = ref;
    m.side = side;
    m.shares = qty;
    m.symbol = Symbol("TEST");
    m.price = px;
    return m;
}
itch::OrderExecuted exec(OrderRef ref, Qty qty, Locate header_locate = 0) {
    return itch::OrderExecuted{hdr(header_locate), ref, qty, 1};
}
itch::OrderExecutedPrice exec_px(OrderRef ref, Qty qty, Price px, bool printable = true) {
    return itch::OrderExecutedPrice{hdr(0), ref, qty, 1, printable, px};
}
itch::OrderCancel cancel(OrderRef ref, Qty qty, Locate header_locate = 0) {
    return itch::OrderCancel{hdr(header_locate), ref, qty};
}
itch::OrderDelete del(OrderRef ref, Locate header_locate = 0) { return itch::OrderDelete{hdr(header_locate), ref}; }
itch::OrderReplace repl(OrderRef old_ref, OrderRef new_ref, Qty qty, Price px) {
    return itch::OrderReplace{hdr(0), old_ref, new_ref, qty, px};
}
itch::StockDirectory directory(Locate l, const char* sym) {
    itch::StockDirectory m;
    m.h = hdr(l);
    m.symbol = Symbol(sym);
    return m;
}

// The whole ladder of one side, best first.
std::vector<Level> ladder(const MarketBooks& mb, Locate l, Side s) {
    std::vector<Level> out;
    const OrderBook* b = mb.book(l);
    if (b == nullptr) return out;
    for (std::size_t i = 0; i < b->depth(s); ++i) out.push_back(b->level(s, i));
    return out;
}

using Ladder = std::vector<Level>;

}  // namespace

OT_TEST(directory_registers_the_symbol_and_creates_the_book) {
    MarketBooks mb(small());
    OT_CHECK(mb.book(7) == nullptr);
    OT_CHECK_EQ(mb.on(directory(7, "AAPL")), Applied::ok);
    OT_CHECK(mb.book(7) != nullptr);
    OT_CHECK_EQ(mb.book(7)->depth(B), std::size_t{0});
    OT_CHECK(mb.locate(Symbol("AAPL")).has_value());
    OT_CHECK(mb.locate(Symbol("AAPL")) == Locate{7});
    OT_CHECK(mb.symbol(7) == Symbol("AAPL"));
    OT_CHECK(!mb.locate(Symbol("MSFT")).has_value());
    OT_CHECK(mb.book(8) == nullptr);
    OT_CHECK(mb.symbol(8).empty());
}

OT_TEST(directory_conflicts_are_rejected_and_change_nothing) {
    MarketBooks mb(small());
    OT_CHECK_EQ(mb.on(directory(7, "AAPL")), Applied::ok);
    OT_CHECK_EQ(mb.on(directory(7, "AAPL")), Applied::ignored);   // a repeat is harmless
    OT_CHECK_EQ(mb.on(directory(9, "AAPL")), Applied::invalid);   // symbol already has a locate
    OT_CHECK_EQ(mb.on(directory(7, "MSFT")), Applied::invalid);   // locate already has a symbol
    OT_CHECK_EQ(mb.on(directory(5, "")), Applied::invalid);       // blank symbol
    OT_CHECK_EQ(mb.on(directory(5, "        ")), Applied::invalid);
    OT_CHECK(mb.locate(Symbol("AAPL")) == Locate{7});
    OT_CHECK(mb.symbol(7) == Symbol("AAPL"));
    OT_CHECK(!mb.locate(Symbol("MSFT")).has_value());
    OT_CHECK(mb.book(9) == nullptr);
    OT_CHECK(mb.book(5) == nullptr);
}

OT_TEST(the_symbol_map_has_a_limit) {
    MarketBooks mb(MarketBooks::Config{16, 4, /*max_symbols=*/2});
    OT_CHECK_EQ(mb.on(directory(1, "AAA")), Applied::ok);
    OT_CHECK_EQ(mb.on(directory(2, "BBB")), Applied::ok);
    OT_CHECK_EQ(mb.on(directory(3, "CCC")), Applied::capacity);
    OT_CHECK(!mb.locate(Symbol("CCC")).has_value());
    OT_CHECK(mb.book(3) == nullptr);                       // a refused entry leaves no book behind
    OT_CHECK_EQ(mb.on(directory(1, "AAA")), Applied::ignored);   // known entries are still recognised
    OT_CHECK_EQ(mb.on(directory(3, "AAA")), Applied::invalid);
    OT_CHECK_EQ(mb.on(add(3, 1, B, 5, 100)), Applied::ok);       // trading does not need a directory entry
}

OT_TEST(instruments_in_different_pages_of_the_locate_table_are_independent) {
    MarketBooks mb(small());
    for (const Locate l : {Locate{0}, Locate{255}, Locate{256}, Locate{4660}, Locate{40000}, Locate{65535}}) {
        OT_CHECK(mb.book(l) == nullptr);
        OT_CHECK_EQ(mb.last_trade(l), Price{0});
        OT_CHECK_EQ(mb.on(add(l, l + 1u, B, 10, 100 + l)), Applied::ok);
    }
    for (const Locate l : {Locate{0}, Locate{255}, Locate{256}, Locate{4660}, Locate{40000}, Locate{65535}}) {
        OT_CHECK(ladder(mb, l, B) == (Ladder{{100 + static_cast<Price>(l), 10, 1}}));
        OT_CHECK(mb.book(l) != nullptr);
    }
    OT_CHECK(mb.book(1) == nullptr);      // same page as locate 0, never used
    OT_CHECK(mb.book(257) == nullptr);
    OT_CHECK(mb.symbol(1).empty());
    OT_CHECK_EQ(mb.last_trade(1), Price{0});
}

OT_TEST(locate_zero_and_the_last_locate_are_ordinary) {
    MarketBooks mb(small());
    OT_CHECK_EQ(mb.on(directory(0, "ZERO")), Applied::ok);
    OT_CHECK_EQ(mb.on(directory(0xFFFF, "LAST")), Applied::ok);
    OT_CHECK_EQ(mb.on(add(0xFFFF, 1, B, 5, 100)), Applied::ok);
    OT_CHECK_EQ(mb.book(0xFFFF)->depth(B), std::size_t{1});
    OT_CHECK(mb.locate(Symbol("ZERO")) == Locate{0});
}

OT_TEST(add_creates_the_book_lazily_and_a_later_directory_adopts_it) {
    MarketBooks mb(small());
    OT_CHECK_EQ(mb.on(add(3, 1, B, 100, 5000)), Applied::ok);
    OT_CHECK(mb.book(3) != nullptr);
    OT_CHECK(mb.symbol(3).empty());  // no directory yet: the Add's stock field is not registered
    OT_CHECK_EQ(mb.on(directory(3, "IBM")), Applied::ok);
    OT_CHECK(mb.locate(Symbol("IBM")) == Locate{3});
    OT_CHECK(ladder(mb, 3, B) == (Ladder{{5000, 100, 1}}));  // the order survived
    OT_CHECK_EQ(mb.live_orders(), std::size_t{1});
}

OT_TEST(adds_build_sorted_ladders_with_counts) {
    MarketBooks mb(small());
    OT_CHECK_EQ(mb.on(add(1, 1, B, 100, 5000)), Applied::ok);
    OT_CHECK_EQ(mb.on(add(1, 2, B, 50, 5010)), Applied::ok);
    OT_CHECK_EQ(mb.on(add(1, 3, B, 25, 5000)), Applied::ok);
    OT_CHECK_EQ(mb.on(add(1, 4, S, 70, 5030)), Applied::ok);
    OT_CHECK_EQ(mb.on(add(1, 5, S, 10, 5020)), Applied::ok);
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{5010, 50, 1}, {5000, 125, 2}}));
    OT_CHECK(ladder(mb, 1, S) == (Ladder{{5020, 10, 1}, {5030, 70, 1}}));
    OT_CHECK_EQ(mb.live_orders(), std::size_t{5});
    OT_CHECK_EQ(mb.applied(), std::uint64_t{5});
    const MarketBooks::OrderInfo* o = mb.order(3);
    OT_CHECK(o != nullptr);
    OT_CHECK_EQ(o->locate, Locate{1});
    OT_CHECK(o->side == B);
    OT_CHECK_EQ(o->price, Price{5000});
    OT_CHECK_EQ(o->qty, Qty{25});
    OT_CHECK(mb.order(99) == nullptr);
}

OT_TEST(add_with_attribution_is_the_same_as_without) {
    MarketBooks mb(small());
    itch::AddOrder a = add(1, 1, S, 10, 100);
    a.has_attribution = true;
    a.mpid[0] = 'G';
    OT_CHECK_EQ(mb.on(a), Applied::ok);
    OT_CHECK(ladder(mb, 1, S) == (Ladder{{100, 10, 1}}));
}

OT_TEST(duplicate_ref_is_rejected_and_the_original_is_untouched) {
    MarketBooks mb(small());
    OT_CHECK_EQ(mb.on(add(1, 1, B, 100, 5000)), Applied::ok);
    OT_CHECK_EQ(mb.on(add(1, 1, S, 7, 6000)), Applied::duplicate_order);
    OT_CHECK_EQ(mb.on(add(2, 1, B, 7, 5000)), Applied::duplicate_order);  // refs are global, not per locate
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{5000, 100, 1}}));
    OT_CHECK_EQ(mb.book(1)->depth(S), std::size_t{0});
    OT_CHECK(mb.book(2) == nullptr);
    OT_CHECK_EQ(mb.live_orders(), std::size_t{1});
    OT_CHECK_EQ(mb.applied(), std::uint64_t{1});
}

OT_TEST(a_ref_can_be_reused_after_the_order_is_gone) {
    MarketBooks mb(small());
    mb.on(add(1, 1, B, 10, 100));
    OT_CHECK_EQ(mb.on(del(1)), Applied::ok);
    OT_CHECK_EQ(mb.on(add(1, 1, S, 20, 200)), Applied::ok);
    OT_CHECK(ladder(mb, 1, S) == (Ladder{{200, 20, 1}}));
    OT_CHECK_EQ(mb.book(1)->depth(B), std::size_t{0});
}

OT_TEST(invalid_adds_change_nothing) {
    MarketBooks mb(small());
    OT_CHECK_EQ(mb.on(add(1, 1, B, 0, 100)), Applied::invalid);
    OT_CHECK_EQ(mb.on(add(1, 2, static_cast<Side>(2), 5, 100)), Applied::invalid);
    OT_CHECK_EQ(mb.live_orders(), std::size_t{0});
    OT_CHECK(mb.book(1) == nullptr);  // not even the book is created
    OT_CHECK_EQ(mb.applied(), std::uint64_t{0});
}

OT_TEST(partial_and_full_execution) {
    MarketBooks mb(small());
    mb.on(add(1, 1, B, 100, 5000));
    mb.on(add(1, 2, B, 40, 5000));
    OT_CHECK_EQ(mb.on(exec(1, 30)), Applied::ok);
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{5000, 110, 2}}));  // partial: both orders still rest
    OT_CHECK_EQ(mb.order(1)->qty, Qty{70});
    OT_CHECK_EQ(mb.on(exec(1, 70)), Applied::ok);
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{5000, 40, 1}}));
    OT_CHECK(mb.order(1) == nullptr);
    OT_CHECK_EQ(mb.on(exec(1, 1)), Applied::unknown_order);  // gone
    OT_CHECK_EQ(mb.on(exec(2, 40)), Applied::ok);
    OT_CHECK_EQ(mb.book(1)->depth(B), std::size_t{0});
    OT_CHECK_EQ(mb.live_orders(), std::size_t{0});
}

OT_TEST(executions_use_the_stored_order_not_the_header) {
    MarketBooks mb(small());
    mb.on(add(1, 1, S, 100, 5000));
    mb.on(add(2, 2, S, 100, 5000));
    OT_CHECK_EQ(mb.on(exec(1, 60, /*header_locate=*/2)), Applied::ok);  // header lies about the instrument
    OT_CHECK(ladder(mb, 1, S) == (Ladder{{5000, 40, 1}}));
    OT_CHECK(ladder(mb, 2, S) == (Ladder{{5000, 100, 1}}));
    OT_CHECK_EQ(mb.on(cancel(2, 10, /*header_locate=*/1)), Applied::ok);
    OT_CHECK(ladder(mb, 2, S) == (Ladder{{5000, 90, 1}}));
    OT_CHECK_EQ(mb.on(del(1, /*header_locate=*/9)), Applied::ok);
    OT_CHECK(mb.book(9) == nullptr);
    OT_CHECK_EQ(mb.book(1)->depth(S), std::size_t{0});
}

OT_TEST(over_execution_is_invalid_and_drops_the_order) {
    MarketBooks mb(small());
    mb.on(add(1, 1, B, 100, 5000));
    mb.on(add(1, 2, B, 30, 5000));
    OT_CHECK_EQ(mb.on(exec(1, 101)), Applied::invalid);
    OT_CHECK(mb.order(1) == nullptr);                           // removed, not left half-trusted
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{5000, 30, 1}}));      // level total consistent
    OT_CHECK_EQ(mb.last_trade(1), Price{0});                    // an invalid execution is not a price
    OT_CHECK_EQ(mb.live_orders(), std::size_t{1});
    OT_CHECK_EQ(mb.applied(), std::uint64_t{2});                // only the two adds
}

OT_TEST(zero_share_execution_and_cancel_are_invalid_and_harmless) {
    MarketBooks mb(small());
    mb.on(add(1, 1, B, 100, 5000));
    OT_CHECK_EQ(mb.on(exec(1, 0)), Applied::invalid);
    OT_CHECK_EQ(mb.on(exec_px(1, 0, 5000)), Applied::invalid);
    OT_CHECK_EQ(mb.on(cancel(1, 0)), Applied::invalid);
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{5000, 100, 1}}));
    OT_CHECK_EQ(mb.order(1)->qty, Qty{100});
}

OT_TEST(unknown_references_are_reported) {
    MarketBooks mb(small());
    OT_CHECK_EQ(mb.on(exec(5, 1)), Applied::unknown_order);
    OT_CHECK_EQ(mb.on(exec_px(5, 1, 100)), Applied::unknown_order);
    OT_CHECK_EQ(mb.on(cancel(5, 1)), Applied::unknown_order);
    OT_CHECK_EQ(mb.on(del(5)), Applied::unknown_order);
    OT_CHECK_EQ(mb.on(repl(5, 6, 1, 100)), Applied::unknown_order);
    OT_CHECK_EQ(mb.applied(), std::uint64_t{0});
    OT_CHECK(mb.book(0) == nullptr);
}

OT_TEST(execution_with_price_consumes_the_display_level_and_records_its_own_price) {
    MarketBooks mb(small());
    mb.on(add(1, 1, S, 100, 5000));
    OT_CHECK_EQ(mb.last_trade(1), Price{0});
    // Executed at 4990 (price improvement), but the displayed order sat at 5000.
    OT_CHECK_EQ(mb.on(exec_px(1, 40, 4990)), Applied::ok);
    OT_CHECK(ladder(mb, 1, S) == (Ladder{{5000, 60, 1}}));
    OT_CHECK_EQ(mb.last_trade(1), Price{4990});
    // A non-printable execution still reduces the order; last_trade is "last seen".
    OT_CHECK_EQ(mb.on(exec_px(1, 60, 4995, /*printable=*/false)), Applied::ok);
    OT_CHECK_EQ(mb.book(1)->depth(S), std::size_t{0});
    OT_CHECK_EQ(mb.last_trade(1), Price{4995});
    mb.on(add(1, 2, S, 10, 5001));
    OT_CHECK_EQ(mb.on(exec_px(2, 11, 5001)), Applied::invalid);
    OT_CHECK_EQ(mb.last_trade(1), Price{4995});
}

OT_TEST(plain_execution_records_the_orders_price) {
    MarketBooks mb(small());
    mb.on(add(1, 1, B, 100, 4321));
    mb.on(add(2, 2, B, 100, 8765));
    mb.on(exec(1, 10));
    OT_CHECK_EQ(mb.last_trade(1), Price{4321});
    OT_CHECK_EQ(mb.last_trade(2), Price{0});  // per instrument
    mb.on(exec(2, 100));
    OT_CHECK_EQ(mb.last_trade(2), Price{8765});
    OT_CHECK_EQ(mb.last_trade(1), Price{4321});
}

OT_TEST(cancel_reduces_partially_and_removes_at_zero) {
    MarketBooks mb(small());
    mb.on(add(1, 1, B, 100, 5000));
    OT_CHECK_EQ(mb.on(cancel(1, 40)), Applied::ok);
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{5000, 60, 1}}));
    OT_CHECK_EQ(mb.order(1)->qty, Qty{60});
    OT_CHECK_EQ(mb.last_trade(1), Price{0});  // a cancel is not a trade
    OT_CHECK_EQ(mb.on(cancel(1, 60)), Applied::ok);
    OT_CHECK(mb.order(1) == nullptr);
    OT_CHECK_EQ(mb.book(1)->depth(B), std::size_t{0});
}

OT_TEST(over_cancel_is_invalid_and_drops_the_order) {
    MarketBooks mb(small());
    mb.on(add(1, 1, B, 100, 5000));
    mb.on(add(1, 2, B, 5, 5001));
    OT_CHECK_EQ(mb.on(cancel(1, 101)), Applied::invalid);
    OT_CHECK(mb.order(1) == nullptr);
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{5001, 5, 1}}));
    OT_CHECK_EQ(mb.live_orders(), std::size_t{1});
}

OT_TEST(delete_removes_the_remainder_and_only_once) {
    MarketBooks mb(small());
    mb.on(add(1, 1, S, 100, 5000));
    mb.on(add(1, 2, S, 50, 5000));
    mb.on(exec(1, 25));
    OT_CHECK_EQ(mb.on(del(1)), Applied::ok);  // removes the 75 that remained
    OT_CHECK(ladder(mb, 1, S) == (Ladder{{5000, 50, 1}}));
    OT_CHECK_EQ(mb.on(del(1)), Applied::unknown_order);
    OT_CHECK(ladder(mb, 1, S) == (Ladder{{5000, 50, 1}}));
}

OT_TEST(replace_moves_the_order_keeping_side_and_instrument) {
    MarketBooks mb(small());
    mb.on(add(4, 10, B, 100, 5000));
    mb.on(add(4, 11, B, 40, 5000));
    OT_CHECK_EQ(mb.on(repl(10, 12, 70, 5010)), Applied::ok);
    OT_CHECK(ladder(mb, 4, B) == (Ladder{{5010, 70, 1}, {5000, 40, 1}}));
    OT_CHECK(mb.order(10) == nullptr);
    const MarketBooks::OrderInfo* n = mb.order(12);
    OT_CHECK(n != nullptr);
    OT_CHECK_EQ(n->locate, Locate{4});
    OT_CHECK(n->side == B);
    OT_CHECK_EQ(n->price, Price{5010});
    OT_CHECK_EQ(n->qty, Qty{70});
    OT_CHECK_EQ(mb.live_orders(), std::size_t{2});

    // Same price, new size: the level total is corrected, not doubled.
    OT_CHECK_EQ(mb.on(repl(11, 13, 15, 5000)), Applied::ok);
    OT_CHECK(ladder(mb, 4, B) == (Ladder{{5010, 70, 1}, {5000, 15, 1}}));

    // The replacement is a normal order afterwards, the old ref is dead.
    OT_CHECK_EQ(mb.on(exec(13, 15)), Applied::ok);
    OT_CHECK_EQ(mb.on(exec(11, 1)), Applied::unknown_order);
    OT_CHECK(ladder(mb, 4, B) == (Ladder{{5010, 70, 1}}));
    OT_CHECK_EQ(mb.book(4)->depth(S), std::size_t{0});
}

OT_TEST(replace_after_a_partial_fill_uses_the_new_size_not_the_remainder) {
    MarketBooks mb(small());
    mb.on(add(1, 1, S, 100, 5000));
    mb.on(exec(1, 90));
    OT_CHECK_EQ(mb.on(repl(1, 2, 300, 5000)), Applied::ok);
    OT_CHECK(ladder(mb, 1, S) == (Ladder{{5000, 300, 1}}));
}

OT_TEST(replace_rejections_leave_everything_as_it_was) {
    MarketBooks mb(small());
    mb.on(add(1, 1, B, 100, 5000));
    mb.on(add(1, 2, B, 10, 4990));
    const Ladder before = ladder(mb, 1, B);
    OT_CHECK_EQ(mb.on(repl(9, 3, 5, 5000)), Applied::unknown_order);
    OT_CHECK_EQ(mb.on(repl(1, 2, 5, 5000)), Applied::duplicate_order);  // new ref is another live order
    OT_CHECK_EQ(mb.on(repl(1, 1, 5, 5000)), Applied::duplicate_order);  // and may not equal the old one
    OT_CHECK_EQ(mb.on(repl(1, 3, 0, 5000)), Applied::invalid);          // zero shares
    OT_CHECK(ladder(mb, 1, B) == before);
    OT_CHECK_EQ(mb.live_orders(), std::size_t{2});
    OT_CHECK_EQ(mb.order(1)->qty, Qty{100});
    OT_CHECK(mb.order(3) == nullptr);
    OT_CHECK_EQ(mb.applied(), std::uint64_t{2});
}

OT_TEST(trades_and_system_events_are_ignored_but_trades_set_the_last_price) {
    MarketBooks mb(small());
    mb.on(add(1, 1, B, 10, 100));
    itch::Trade t;
    t.h = hdr(1);
    t.side = S;
    t.shares = 500;
    t.price = 101;
    OT_CHECK_EQ(mb.on(t), Applied::ignored);
    OT_CHECK_EQ(mb.last_trade(1), Price{101});
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{100, 10, 1}}));  // hidden liquidity: display untouched
    t.h = hdr(30);
    t.price = 77;
    OT_CHECK_EQ(mb.on(t), Applied::ignored);
    OT_CHECK_EQ(mb.last_trade(30), Price{77});
    OT_CHECK(mb.book(30) == nullptr);  // a trade does not allocate a book
    OT_CHECK_EQ(mb.on(itch::SystemEvent{hdr(0), 'O'}), Applied::ignored);
    OT_CHECK_EQ(mb.applied(), std::uint64_t{1});
}

OT_TEST(a_full_order_table_refuses_adds_until_room_appears) {
    MarketBooks mb(small(/*orders=*/3));
    OT_CHECK_EQ(mb.on(add(1, 1, B, 1, 100)), Applied::ok);
    OT_CHECK_EQ(mb.on(add(1, 2, B, 1, 101)), Applied::ok);
    OT_CHECK_EQ(mb.on(add(1, 3, B, 1, 102)), Applied::ok);
    OT_CHECK_EQ(mb.on(add(1, 4, B, 1, 103)), Applied::capacity);
    OT_CHECK_EQ(mb.live_orders(), std::size_t{3});
    OT_CHECK_EQ(mb.book(1)->depth(B), std::size_t{3});  // the refused order is nowhere
    OT_CHECK(mb.order(4) == nullptr);
    OT_CHECK_EQ(mb.on(add(1, 3, B, 1, 102)), Applied::duplicate_order);  // duplicate is reported first
    // A replace never needs a free slot: it releases one first.
    OT_CHECK_EQ(mb.on(repl(1, 9, 5, 100)), Applied::ok);
    OT_CHECK_EQ(mb.live_orders(), std::size_t{3});
    OT_CHECK_EQ(mb.on(del(2)), Applied::ok);
    OT_CHECK_EQ(mb.on(add(1, 4, B, 1, 103)), Applied::ok);
}

OT_TEST(level_overflow_keeps_orders_tracked_and_totals_consistent) {
    MarketBooks mb(small(64, /*levels=*/2));
    OT_CHECK_EQ(mb.on(add(1, 1, B, 10, 20)), Applied::ok);
    OT_CHECK_EQ(mb.on(add(1, 2, B, 20, 30)), Applied::ok);
    // Worse than the worst tracked level: not on the ladder, but the order is live.
    OT_CHECK_EQ(mb.on(add(1, 3, B, 99, 10)), Applied::capacity);
    OT_CHECK_EQ(mb.live_orders(), std::size_t{3});
    OT_CHECK(mb.order(3) != nullptr);
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{30, 20, 1}, {20, 10, 1}}));
    OT_CHECK_EQ(mb.overflows(), std::uint64_t{1});

    // Its execution and delete are recognised and must not disturb the ladder.
    OT_CHECK_EQ(mb.on(exec(3, 9)), Applied::ok);
    OT_CHECK_EQ(mb.order(3)->qty, Qty{90});
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{30, 20, 1}, {20, 10, 1}}));
    OT_CHECK_EQ(mb.on(del(3)), Applied::ok);
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{30, 20, 1}, {20, 10, 1}}));

    // A better price evicts the worst level (20); order 1 lives on, off the ladder.
    OT_CHECK_EQ(mb.on(add(1, 4, B, 5, 40)), Applied::ok);
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{40, 5, 1}, {30, 20, 1}}));
    OT_CHECK(mb.order(1) != nullptr);
    OT_CHECK_EQ(mb.overflows(), std::uint64_t{2});  // the refusal of order 3 and the eviction by order 4
}

OT_TEST(an_evicted_levels_orders_cannot_corrupt_a_recreated_level) {
    MarketBooks mb(small(64, /*levels=*/2));
    mb.on(add(1, 1, B, 10, 20));   // will be evicted
    mb.on(add(1, 2, B, 20, 30));
    mb.on(add(1, 3, B, 5, 40));    // evicts the level at 20
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{40, 5, 1}, {30, 20, 1}}));
    OT_CHECK_EQ(mb.on(del(3)), Applied::ok);   // free a slot on the ladder
    OT_CHECK_EQ(mb.on(del(2)), Applied::ok);
    OT_CHECK_EQ(mb.book(1)->depth(B), std::size_t{0});

    // Price 20 comes back as a brand new level while order 1 still exists.
    OT_CHECK_EQ(mb.on(add(1, 4, B, 7, 20)), Applied::ok);
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{20, 7, 1}}));

    // The ghost is reduced, cancelled and deleted; the new level must not feel it.
    OT_CHECK_EQ(mb.on(exec(1, 4)), Applied::ok);
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{20, 7, 1}}));
    OT_CHECK_EQ(mb.on(cancel(1, 2)), Applied::ok);
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{20, 7, 1}}));
    OT_CHECK_EQ(mb.order(1)->qty, Qty{4});
    OT_CHECK_EQ(mb.on(del(1)), Applied::ok);
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{20, 7, 1}}));

    // And the real occupant still behaves.
    OT_CHECK_EQ(mb.on(exec(4, 7)), Applied::ok);
    OT_CHECK_EQ(mb.book(1)->depth(B), std::size_t{0});
    OT_CHECK_EQ(mb.live_orders(), std::size_t{0});
}

OT_TEST(replacing_an_off_ladder_order_can_bring_it_back_on) {
    MarketBooks mb(small(64, /*levels=*/1));
    mb.on(add(1, 1, S, 10, 50));                                   // the only tracked ask level
    OT_CHECK_EQ(mb.on(add(1, 2, S, 10, 60)), Applied::capacity);   // worse: off the ladder
    OT_CHECK_EQ(mb.on(repl(2, 3, 8, 40)), Applied::ok);            // better than 50: evicts it
    OT_CHECK(ladder(mb, 1, S) == (Ladder{{40, 8, 1}}));
    OT_CHECK_EQ(mb.on(repl(1, 4, 6, 70)), Applied::capacity);      // order 1 was evicted; 70 is worse than 40
    OT_CHECK(ladder(mb, 1, S) == (Ladder{{40, 8, 1}}));
    OT_CHECK(mb.order(4) != nullptr);
    OT_CHECK(mb.order(1) == nullptr);
}

OT_TEST(a_level_that_would_overflow_its_share_count_refuses_the_order) {
    MarketBooks mb(small());
    OT_CHECK_EQ(mb.on(add(1, 1, B, 0xFFFFFFF0u, 100)), Applied::ok);
    OT_CHECK_EQ(mb.on(add(1, 2, B, 0x20, 100)), Applied::capacity);
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{100, 0xFFFFFFF0u, 1}}));
    OT_CHECK_EQ(mb.on(exec(2, 0x20)), Applied::ok);  // the tracked-but-off-ladder order trades away
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{100, 0xFFFFFFF0u, 1}}));
    OT_CHECK_EQ(mb.live_orders(), std::size_t{1});
}

OT_TEST(instruments_are_isolated) {
    MarketBooks mb(small());
    mb.on(add(1, 1, B, 10, 100));
    mb.on(add(2, 2, B, 20, 100));
    mb.on(add(3, 3, S, 30, 100));
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{100, 10, 1}}));
    OT_CHECK(ladder(mb, 2, B) == (Ladder{{100, 20, 1}}));
    OT_CHECK(ladder(mb, 3, S) == (Ladder{{100, 30, 1}}));
    OT_CHECK(!mb.book(1)->crossed());
    mb.on(del(2));
    OT_CHECK_EQ(mb.book(2)->depth(B), std::size_t{0});
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{100, 10, 1}}));
}

OT_TEST(book_allocation_failure_is_reported_not_thrown) {
    // A book this large cannot exist; the fixed tables of MarketBooks itself are small.
    MarketBooks mb(MarketBooks::Config{16, std::numeric_limits<std::size_t>::max() / 2});
    OT_CHECK_EQ(mb.on(add(1, 1, B, 10, 100)), Applied::capacity);
    OT_CHECK_EQ(mb.live_orders(), std::size_t{0});
    OT_CHECK(mb.book(1) == nullptr);
    OT_CHECK_EQ(mb.on(directory(1, "AAPL")), Applied::capacity);
    OT_CHECK(!mb.locate(Symbol("AAPL")).has_value());
    OT_CHECK(mb.symbol(1).empty());
}

OT_TEST(zero_level_capacity_tracks_orders_but_shows_no_depth) {
    MarketBooks mb(small(16, 0));
    OT_CHECK_EQ(mb.on(add(1, 1, B, 10, 100)), Applied::capacity);
    OT_CHECK_EQ(mb.live_orders(), std::size_t{1});
    OT_CHECK_EQ(mb.book(1)->depth(B), std::size_t{0});
    OT_CHECK_EQ(mb.on(exec(1, 10)), Applied::ok);
    OT_CHECK_EQ(mb.live_orders(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// Wire bytes straight into the books. The vectors are written out by hand from
// the ITCH 5.0 field tables, not produced by the encoder.
// ---------------------------------------------------------------------------
namespace {

struct Feed : itch::NullHandler {
    using itch::NullHandler::on;
    explicit Feed(MarketBooks& b) : books(b) {}
    MarketBooks& books;
    std::vector<Applied> results;
    template <class M>
    void apply(const M& m) noexcept {
        results.push_back(books.on(m));
    }
    void on(const itch::StockDirectory& m) noexcept { apply(m); }
    void on(const itch::AddOrder& m) noexcept { apply(m); }
    void on(const itch::OrderExecuted& m) noexcept { apply(m); }
    void on(const itch::OrderExecutedPrice& m) noexcept { apply(m); }
    void on(const itch::OrderCancel& m) noexcept { apply(m); }
    void on(const itch::OrderDelete& m) noexcept { apply(m); }
    void on(const itch::OrderReplace& m) noexcept { apply(m); }
    void on(const itch::Trade& m) noexcept { apply(m); }
    void on(const itch::SystemEvent& m) noexcept { apply(m); }
};

std::vector<std::byte> bytes(std::initializer_list<int> v) {
    std::vector<std::byte> out;
    for (int x : v) out.push_back(static_cast<std::byte>(x));
    return out;
}

void append(std::vector<std::byte>& stream, const std::vector<std::byte>& msg) {
    stream.push_back(static_cast<std::byte>(msg.size() >> 8));
    stream.push_back(static_cast<std::byte>(msg.size() & 0xFF));
    stream.insert(stream.end(), msg.begin(), msg.end());
}

}  // namespace

OT_TEST(wire_bytes_drive_the_books) {
    std::vector<std::byte> stream;
    // A: locate 1, tracking 0, ts 0x000000001234, ref 0x101, 'B', 100 shares, "AAPL    ", 123.4500 (0x0012D644)
    append(stream, bytes({'A', 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x34,
                          0, 0, 0, 0, 0, 0, 0x01, 0x01, 'B', 0, 0, 0, 100,
                          'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ', 0x00, 0x12, 0xD6, 0x44}));
    // A: ref 0x102, 'S', 50 shares at 123.4600 (0x0012D6A8)
    append(stream, bytes({'A', 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x35,
                          0, 0, 0, 0, 0, 0, 0x01, 0x02, 'S', 0, 0, 0, 50,
                          'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ', 0x00, 0x12, 0xD6, 0xA8}));
    // E: ref 0x101, 40 shares, match 9
    append(stream, bytes({'E', 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x36,
                          0, 0, 0, 0, 0, 0, 0x01, 0x01, 0, 0, 0, 40,
                          0, 0, 0, 0, 0, 0, 0, 9}));
    // Unsupported type 'H' (trading action) is skipped by the stream decoder.
    append(stream, bytes({'H', 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ', 'T', ' ', ' ', ' ', ' ', ' '}));
    // X: ref 0x101, cancel 10 shares
    append(stream, bytes({'X', 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x37,
                          0, 0, 0, 0, 0, 0, 0x01, 0x01, 0, 0, 0, 10}));
    // U: replace ref 0x102 by ref 0x103, 70 shares at 123.4700 (0x0012D70C)
    append(stream, bytes({'U', 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x38,
                          0, 0, 0, 0, 0, 0, 0x01, 0x02, 0, 0, 0, 0, 0, 0, 0x01, 0x03,
                          0, 0, 0, 70, 0x00, 0x12, 0xD7, 0x0C}));
    // A with an illegal side byte: dropped by the decoder, never reaches the books.
    append(stream, bytes({'A', 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0x39,
                          0, 0, 0, 0, 0, 0, 0x01, 0x04, 'X', 0, 0, 0, 100,
                          'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ', 0x00, 0x12, 0xD6, 0x44}));

    MarketBooks mb(small());
    Feed feed(mb);
    const itch::StreamResult r = itch::decode_stream(std::span<const std::byte>(stream), feed);
    OT_CHECK_EQ(r.consumed, stream.size());
    OT_CHECK_EQ(r.messages, std::size_t{5});
    OT_CHECK_EQ(r.skipped, std::size_t{2});
    OT_CHECK_EQ(feed.results.size(), std::size_t{5});
    for (const Applied a : feed.results) OT_CHECK_EQ(a, Applied::ok);

    // Bid: 100 - 40 - 10 = 50 left at 123.4500. Ask 0x102 was replaced: 70 at 123.4700.
    OT_CHECK(ladder(mb, 1, B) == (Ladder{{1'234'500, 50, 1}}));
    OT_CHECK(ladder(mb, 1, S) == (Ladder{{1'234'700, 70, 1}}));
    OT_CHECK_EQ(mb.order(0x101)->qty, Qty{50});
    OT_CHECK(mb.order(0x102) == nullptr);
    OT_CHECK_EQ(mb.order(0x103)->price, Price{1'234'700});
    OT_CHECK_EQ(mb.last_trade(1), Price{1'234'500});
}

OT_TEST(counters_track_ok_results_only) {
    MarketBooks mb(small());
    mb.on(directory(1, "AAA"));                // ok, but not order flow
    mb.on(add(1, 1, B, 10, 100));              // 1
    mb.on(add(1, 1, B, 10, 100));              // duplicate
    mb.on(exec(1, 4));                         // 2
    mb.on(exec(1, 40));                        // invalid (drops the order)
    mb.on(add(1, 2, B, 10, 100));              // 3
    mb.on(cancel(2, 1));                       // 4
    mb.on(del(2));                             // 5
    mb.on(del(2));                             // unknown
    OT_CHECK_EQ(mb.applied(), std::uint64_t{5});
    OT_CHECK_EQ(mb.live_orders(), std::size_t{0});
    OT_CHECK_EQ(mb.overflows(), std::uint64_t{0});
}

OT_TEST_MAIN()

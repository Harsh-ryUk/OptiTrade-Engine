#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>

#include "optitrade/book/order_book.hpp"
#include "optitrade/core/flat_hash_map.hpp"
#include "optitrade/core/types.hpp"
#include "optitrade/itch/messages.hpp"

// Full-depth books for every instrument on an ITCH feed, driven message by message.
//
// State. One FlatHashMap holds every live order (reference -> locate, side,
// price, remaining quantity). Per-instrument state (the OrderBook, the symbol, the
// last execution price) lives in a table indexed directly by the 16-bit locate,
// organised as 256 pages of 256 locates so that an idle feed costs nothing.
// Executions, cancels, deletes and replaces name only an order reference, so they
// act on the STORED order: the locate in their header is never consulted, and a
// corrupt one cannot move quantity between instruments.
//
// Memory. The order table (max_orders entries), the symbol map (max_symbols
// entries) and the 256-entry page directory are allocated in the constructor.
// A page (6 KiB) is allocated the first time one of its locates is seen, and an
// instrument's OrderBook (2 x max_levels_per_side levels, 24 bytes each) the first
// time its locate gets a Stock Directory entry or an Add. On a real feed the
// directory for every instrument precedes the first order, so both are allocated
// before trading and never on the order path. Nothing grows afterwards. Worst case,
// under a hostile feed that touches every locate:
// 65536 x (2 x max_levels_per_side x 24 + 24) bytes; size max_levels_per_side for the
// deployment. If a page or book cannot be allocated the message is refused with
// Applied::capacity rather than terminating.
//
// Result codes and their precedence (the first rule that applies wins). A message
// that returns unknown_order, duplicate_order or invalid changes nothing, with two
// deliberate exceptions noted below: an over-execution or over-cancel drops the
// order, and a Trade (ignored) still records its price.
//   Stock Directory  blank symbol -> invalid; symbol already bound: same locate ->
//                    ignored, other locate -> invalid; locate already bound to
//                    another symbol -> invalid; symbol map full, or page / book could
//                    not be allocated -> capacity; else register + create the book -> ok.
//   Add (A, F)       bad side or 0 shares -> invalid; ref already live ->
//                    duplicate_order; order table full -> capacity; book could
//                    not be allocated -> capacity; the order's level was refused (the
//                    order is tracked but not on the ladder) -> capacity; else ok.
//                    An add that evicts a worse level is ok: the message was applied in
//                    full, and overflows() records that depth was lost.
//                    The stock field is informational: the locate identifies the book.
//   Executed (E, C)  ref unknown -> unknown_order; 0 shares -> invalid; more than
//                    remains -> invalid AND the order is dropped (its state can
//                    no longer be trusted); else ok.
//   Cancel (X)       same rules as an execution, without a price.
//   Delete (D)       ref unknown -> unknown_order; else ok.
//   Replace (U)      old ref unknown -> unknown_order; 0 shares -> invalid; new ref
//                    already live (or equal to the old one) -> duplicate_order;
//                    else atomically drop the old order and add the new one on
//                    the same locate and side -> ok, or capacity if the new level
//                    overflowed. The first three outcomes leave state untouched.
//   Trade (P)        records the price for last_trade(), -> ignored (the price is
//                    dropped only if a page cannot be allocated).
//   System Event     -> ignored.
// `unknown_symbol` is part of the shared result vocabulary and is not produced here:
// the locate is the key and an Add on a locate with no directory entry creates its
// book on the spot.
//
// Overflow. A side holds at most max_levels_per_side levels (policy in
// order_book.hpp). An order whose level cannot be stored stays in the order table
// so that its later executions and deletes are recognised, but it contributes to no
// level; a level that was evicted takes its orders' contribution with it. Levels
// are matched to orders by OrderBook::LevelId, so quantity of a forgotten level is
// never subtracted from a newer level that reuses the price: for every tracked
// level, qty is exactly the sum of the remaining quantity of the live orders that
// were added to that incarnation of the level. Consequently the ladder never shows
// more than the live orders hold.
//
// Not thread safe.
namespace optitrade::book {

class MarketBooks {
public:
    struct Config {
        std::size_t max_orders{1u << 20};
        std::size_t max_levels_per_side{256};
        std::size_t max_symbols{1u << 16};  // directory entries; capped at one per locate
    };

    struct OrderInfo {
        Locate locate{};
        Side side{};
        Price price{};
        Qty qty{};  // remaining displayed quantity
    };

    // Throws std::bad_alloc / std::length_error when the fixed tables cannot be allocated.
    explicit MarketBooks(const Config& config)
        : config_(config),
          orders_(config.max_orders),
          by_symbol_(std::min(config.max_symbols, kLocates)) {}

    Applied on(const itch::StockDirectory& m) noexcept {
        const Locate loc = m.h.locate;
        if (m.symbol.empty()) return Applied::invalid;
        if (const Locate* bound = by_symbol_.find(m.symbol)) {
            return *bound == loc ? Applied::ignored : Applied::invalid;
        }
        const Slot* existing = find_slot(loc);
        if (existing != nullptr && !existing->symbol.empty()) return Applied::invalid;
        if (by_symbol_.size() >= by_symbol_.max_size()) return Applied::capacity;
        Slot* slot = slot_for(loc);
        if (slot == nullptr || ensure_book(*slot) == nullptr) return Applied::capacity;
        by_symbol_.insert(m.symbol, loc);
        slot->symbol = m.symbol;
        return Applied::ok;
    }

    Applied on(const itch::AddOrder& m) noexcept {
        if (m.shares == 0 || (m.side != Side::buy && m.side != Side::sell)) return Applied::invalid;
        if (orders_.find(m.ref) != nullptr) return Applied::duplicate_order;
        if (orders_.size() >= orders_.max_size()) return Applied::capacity;
        if (!insert_order(m.ref, m.h.locate, m.side, m.price, m.shares)) return Applied::capacity;
        ++applied_;
        return Applied::ok;
    }

    Applied on(const itch::OrderExecuted& m) noexcept { return reduce(m.ref, m.shares, nullptr); }

    // The order is consumed at its displayed price level; the execution price
    // only feeds last_trade(). `printable` is not consulted: last_trade() is the
    // last execution price seen, not the tape.
    Applied on(const itch::OrderExecutedPrice& m) noexcept { return reduce(m.ref, m.shares, &m.price); }

    Applied on(const itch::OrderCancel& m) noexcept { return reduce(m.ref, m.shares, nullptr, false); }

    Applied on(const itch::OrderDelete& m) noexcept {
        const OrderRecord* rec = orders_.find(m.ref);
        if (rec == nullptr) return Applied::unknown_order;
        drop(m.ref);
        ++applied_;
        return Applied::ok;
    }

    Applied on(const itch::OrderReplace& m) noexcept {
        const OrderRecord* old = orders_.find(m.old_ref);
        if (old == nullptr) return Applied::unknown_order;
        if (m.shares == 0) return Applied::invalid;
        if (orders_.find(m.new_ref) != nullptr) return Applied::duplicate_order;
        const OrderInfo prev = old->info;  // drop() relocates table entries
        drop(m.old_ref);
        // The slot just freed guarantees the insert below finds room.
        const bool on_ladder = insert_order(m.new_ref, prev.locate, prev.side, m.price, m.shares);
        if (!on_ladder) return Applied::capacity;
        ++applied_;
        return Applied::ok;
    }

    // A trade against hidden liquidity leaves the displayed book alone but is
    // still the most recent price for the instrument.
    Applied on(const itch::Trade& m) noexcept {
        if (Slot* slot = slot_for(m.h.locate)) slot->last_trade = m.price;
        return Applied::ignored;
    }

    Applied on(const itch::SystemEvent&) noexcept { return Applied::ignored; }

    const OrderBook* book(Locate l) const noexcept {
        const Slot* slot = find_slot(l);
        return slot == nullptr ? nullptr : slot->book.get();
    }

    std::optional<Locate> locate(const Symbol& s) const noexcept {
        const Locate* l = by_symbol_.find(s);
        if (l == nullptr) return std::nullopt;
        return *l;
    }

    // Blank for a locate without a directory entry.
    Symbol symbol(Locate l) const noexcept {
        const Slot* slot = find_slot(l);
        return slot == nullptr ? Symbol() : slot->symbol;
    }

    // The pointer is valid until the next call that modifies the books.
    const OrderInfo* order(OrderRef ref) const noexcept {
        const OrderRecord* rec = orders_.find(ref);
        return rec == nullptr ? nullptr : &rec->info;
    }

    std::size_t live_orders() const noexcept { return orders_.size(); }

    // Order-flow messages (A, F, E, C, X, D, U) that returned Applied::ok.
    std::uint64_t applied() const noexcept { return applied_; }

    // Adds, across all books, that hit a level limit (see OrderBook::overflows()).
    // A non-zero value means some depth is missing from the ladders.
    std::uint64_t overflows() const noexcept { return overflows_; }

    // Last execution price seen for the locate (E at the order's price, C, P); 0 if none.
    Price last_trade(Locate l) const noexcept {
        const Slot* slot = find_slot(l);
        return slot == nullptr ? 0 : slot->last_trade;
    }

private:
    static constexpr std::size_t kLocates = std::size_t{std::numeric_limits<Locate>::max()} + 1;
    static constexpr std::size_t kPageBits = 8;
    static constexpr std::size_t kPageSize = std::size_t{1} << kPageBits;
    static constexpr std::size_t kPages = kLocates >> kPageBits;
    static_assert(sizeof(Locate) == 2, "the page split assumes a 16-bit locate");

    struct Slot {
        std::unique_ptr<OrderBook> book;  // null until the locate is first used
        Symbol symbol;                    // blank until the directory names it
        Price last_trade{0};
    };
    struct Page {
        std::array<Slot, kPageSize> slots;
    };

    struct OrderRecord {
        OrderInfo info;
        OrderBook::LevelId level{OrderBook::kNoLevel};  // kNoLevel: not on the ladder
    };

    const Slot* find_slot(Locate l) const noexcept {
        const Page* page = pages_[l >> kPageBits].get();
        return page == nullptr ? nullptr : &page->slots[l & (kPageSize - 1)];
    }

    // The slot for `l`, allocating its page on first use; null if that fails.
    Slot* slot_for(Locate l) noexcept {
        std::unique_ptr<Page>& page = pages_[l >> kPageBits];
        if (page == nullptr) {
            // One of the two places that allocate after construction. Failure is
            // reported through the result code, not by unwinding the message path.
            try {
                page = std::make_unique<Page>();
            } catch (const std::exception&) {
                return nullptr;
            }
        }
        return &page->slots[l & (kPageSize - 1)];
    }

    OrderBook* ensure_book(Slot& slot) noexcept {
        if (slot.book == nullptr) {
            try {
                slot.book = std::make_unique<OrderBook>(config_.max_levels_per_side);
            } catch (const std::exception&) {
                return nullptr;
            }
        }
        return slot.book.get();
    }

    // The slot of a locate that has live orders: an order is recorded only after
    // its page and book exist, and neither is ever released.
    Slot& slot_of(Locate l) noexcept { return pages_[l >> kPageBits]->slots[l & (kPageSize - 1)]; }

    // Records a new order and posts it to the ladder. Requires the ref to be free
    // and the table to have room. False if the ladder could not take it (the
    // order is still recorded) or the book could not be created (nothing is).
    bool insert_order(OrderRef ref, Locate loc, Side side, Price price, Qty qty) noexcept {
        Slot* slot = slot_for(loc);
        if (slot == nullptr) return false;
        OrderBook* bk = ensure_book(*slot);
        if (bk == nullptr) return false;
        const std::uint64_t before = bk->overflows();
        const OrderBook::LevelId id = bk->add_order(side, price, qty);
        overflows_ += bk->overflows() - before;
        orders_.insert(ref, OrderRecord{OrderInfo{loc, side, price, qty}, id});
        return id != OrderBook::kNoLevel;
    }

    // Removes an existing order from the table and, if it is on the ladder, from its level.
    void drop(OrderRef ref) noexcept {
        const OrderRecord rec = *orders_.find(ref);
        if (rec.level != OrderBook::kNoLevel) {
            // A false result means the level was evicted since; nothing to undo.
            slot_of(rec.info.locate).book->remove_order(rec.info.side, rec.info.price, rec.info.qty, rec.level);
        }
        orders_.erase(ref);
    }

    // Shared by E, C and X: takes `shares` off the order. `trade_price` is the
    // execution price of a C; an E passes nullptr and trades at the order's price.
    Applied reduce(OrderRef ref, Qty shares, const Price* trade_price, bool execution = true) noexcept {
        OrderRecord* rec = orders_.find(ref);
        if (rec == nullptr) return Applied::unknown_order;
        if (shares == 0) return Applied::invalid;
        if (shares > rec->info.qty) {
            drop(ref);
            return Applied::invalid;
        }
        const Locate loc = rec->info.locate;
        const Price traded = trade_price != nullptr ? *trade_price : rec->info.price;
        if (shares == rec->info.qty) {
            drop(ref);
        } else {
            if (rec->level != OrderBook::kNoLevel) {
                slot_of(loc).book->reduce_order(rec->info.side, rec->info.price, shares, rec->level);
            }
            rec->info.qty -= shares;
        }
        if (execution) slot_of(loc).last_trade = traded;
        ++applied_;
        return Applied::ok;
    }

    Config config_;
    FlatHashMap<OrderRef, OrderRecord> orders_;
    FlatHashMap<Symbol, Locate> by_symbol_;
    std::array<std::unique_ptr<Page>, kPages> pages_;  // null until a locate of the page is seen
    std::uint64_t applied_{0};
    std::uint64_t overflows_{0};
};

}  // namespace optitrade::book

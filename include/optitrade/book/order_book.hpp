#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "optitrade/core/types.hpp"

// Aggregated price levels of one instrument.
//
// Layout. Each side is one contiguous array of Level ordered from the WORST to
// the BEST price, so the best price is the last element. Market data is
// concentrated at the touch: an add or a removal there moves at most a few
// neighbours, and the deep end of the ladder (index 0) is only disturbed by the
// rare overflow eviction. Lookup is a binary search over the array; the
// bid side is ascending by price, the ask side descending. `level(side, 0)` is
// always the best level whatever the side.
//
// Overflow policy. Storage is fixed at construction (`max_levels_per_side`
// levels per side) and never grows. When a side is full and a price arrives that
// has no level yet:
//   * a price WORSE than the worst tracked level is not stored (`add` returns
//     false);
//   * any other price evicts the worst tracked level, which is forgotten
//     together with its quantity, and the new level takes its place.
// Either way the book keeps the best `max_levels_per_side` prices seen, which
// is the part of the ladder that matters. Orders on a forgotten level are the
// caller's business (MarketBooks keeps them in its order table and stops
// touching the book on their behalf, see LevelId). Joining an existing level
// never overflows, except that a level whose quantity would exceed 2^32-1 shares
// (the width of Qty), or whose order count would exceed 2^32-1, refuses the order
// instead of wrapping.
// `overflows()` counts every add that hit one of these limits.
//
// Invariants, at all times and for any call sequence: levels are strictly
// ordered, every stored level has qty > 0 and orders >= 1, and the quantity of a
// level is exactly the sum of what was added to it minus what was removed from it.
//
// Not thread safe. Allocation happens only in the constructor.
namespace optitrade::book {

struct Level {
    Price price{};
    Qty qty{};                  // total displayed shares at this price
    std::uint32_t orders{};     // resting orders at this price

    friend constexpr bool operator==(const Level&, const Level&) = default;
};

// Result of applying one market data message to the books. The rules per message
// type are in market_books.hpp; the enum lives here so that both headers of the
// module share it.
enum class Applied : std::uint8_t {
    ok,
    ignored,
    unknown_order,
    duplicate_order,
    unknown_symbol,
    capacity,
    invalid
};

constexpr const char* to_string(Applied a) noexcept {
    switch (a) {
        case Applied::ok: return "ok";
        case Applied::ignored: return "ignored";
        case Applied::unknown_order: return "unknown_order";
        case Applied::duplicate_order: return "duplicate_order";
        case Applied::unknown_symbol: return "unknown_symbol";
        case Applied::capacity: return "capacity";
        case Applied::invalid: return "invalid";
    }
    return "?";
}

class OrderBook {
public:
    // Identifies one incarnation of a price level. A level that is evicted and
    // later recreated at the same price gets a new id, which lets a caller that
    // tracks individual orders tell "my order is on this level" from "my order
    // was on a level that has since been forgotten". Ids are unique within a book.
    using LevelId = std::uint64_t;
    static constexpr LevelId kNoLevel = 0;  // "not stored" (result) / "any level" (argument)

    // Allocates both sides up front; throws std::bad_alloc / std::length_error
    // when the storage cannot be obtained.
    explicit OrderBook(std::size_t max_levels_per_side)
        : sides_{SideBook(max_levels_per_side), SideBook(max_levels_per_side)} {}

    std::size_t max_levels_per_side() const noexcept { return sides_[0].levels.size(); }

    // Adds one order of `qty` shares at `price`. Returns false, and changes
    // nothing, when qty is 0 or the order could not be stored (see the overflow
    // policy). Returns true when it joined an existing level, created a level, or
    // created a level by evicting the worst one.
    bool add(Side side, Price price, Qty qty) noexcept { return add_order(side, price, qty) != kNoLevel; }

    // Removes `qty` shares from the level at `price` as a whole order leaving the
    // level: quantity falls by `qty` and the order count by one. The level is
    // dropped when its quantity reaches zero. Returns false, and changes nothing,
    // when qty is 0, no level is tracked at `price`, or the level holds fewer
    // than `qty` shares.
    bool remove(Side side, Price price, Qty qty) noexcept {
        return take(side, price, qty, kNoLevel, /*whole_order=*/true);
    }

    // Like remove(), but the order stays on the level with fewer shares (a
    // partial execution or cancel), so the order count is unchanged.
    bool reduce(Side side, Price price, Qty qty) noexcept {
        return take(side, price, qty, kNoLevel, /*whole_order=*/false);
    }

    // Id-checked variants for callers that track individual orders. add_order
    // returns the id of the level now holding the order, or kNoLevel when it was
    // not stored. remove_order / reduce_order act only if the level at `price` is
    // still the incarnation `id`; a stale id (level evicted or recreated) is a
    // no-op that returns false, which is how an order on a forgotten level is
    // kept from eating into a newer level at the same price.
    LevelId add_order(Side side, Price price, Qty qty) noexcept {
        if (qty == 0) return kNoLevel;
        SideBook& sb = side_book(side);
        const std::size_t pos = find_pos(sb, side, price);

        if (pos < sb.size && sb.levels[pos].price == price) {
            Level& lv = sb.levels[pos];
            if (lv.qty > kQtyMax - qty || lv.orders == kOrdersMax) {
                ++overflows_;
                return kNoLevel;
            }
            lv.qty += qty;
            ++lv.orders;
            ++updates_;
            return sb.ids[pos];
        }

        std::size_t at = pos;
        if (sb.size == sb.levels.size()) {
            ++overflows_;
            // pos == 0: every tracked level is better than the new price (also the
            // zero-capacity case). Otherwise drop the worst level, slot 0, by
            // sliding the worse-than-new prefix down over it.
            if (pos == 0) return kNoLevel;
            shift_down(sb, 1, pos);
            at = pos - 1;
        } else {
            shift_up(sb, pos);
            ++sb.size;
        }
        sb.levels[at] = Level{price, qty, 1};
        sb.ids[at] = next_id_++;
        ++updates_;
        return sb.ids[at];
    }

    bool remove_order(Side side, Price price, Qty qty, LevelId id) noexcept {
        return id != kNoLevel && take(side, price, qty, id, true);
    }
    bool reduce_order(Side side, Price price, Qty qty, LevelId id) noexcept {
        return id != kNoLevel && take(side, price, qty, id, false);
    }

    std::size_t depth(Side side) const noexcept { return side_book(side).size; }

    // i = 0 is the best price. Requires i < depth(side); an out-of-range index
    // yields an all-zero level instead of reading outside the array.
    const Level& level(Side side, std::size_t i) const noexcept {
        const SideBook& sb = side_book(side);
        return i < sb.size ? sb.levels[sb.size - 1 - i] : kEmptyLevel;
    }

    std::optional<Level> best(Side side) const noexcept {
        const SideBook& sb = side_book(side);
        if (sb.size == 0) return std::nullopt;
        return sb.levels[sb.size - 1];
    }

    Qty qty_at(Side side, Price price) const noexcept {
        const SideBook& sb = side_book(side);
        const std::size_t pos = find_pos(sb, side, price);
        return pos < sb.size && sb.levels[pos].price == price ? sb.levels[pos].qty : 0;
    }

    // Shares on the best `top_n` levels, saturating at the largest Qty.
    Qty total_qty(Side side, std::size_t top_n) const noexcept {
        const SideBook& sb = side_book(side);
        const std::size_t n = std::min(top_n, sb.size);
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < n; ++i) {
            sum += sb.levels[sb.size - 1 - i].qty;
            if (sum >= kQtyMax) return kQtyMax;
        }
        return static_cast<Qty>(sum);
    }

    // Locked (bid == ask) counts as crossed: the book should have matched.
    bool crossed() const noexcept {
        const SideBook& bids = sides_[index(Side::buy)];
        const SideBook& asks = sides_[index(Side::sell)];
        return bids.size != 0 && asks.size != 0 &&
               bids.levels[bids.size - 1].price >= asks.levels[asks.size - 1].price;
    }

    // Successful add / remove / reduce calls (an add that joined a level counts).
    std::uint64_t updates() const noexcept { return updates_; }

    // Adds that could not be stored as asked: refused or evicting, see the policy.
    std::uint64_t overflows() const noexcept { return overflows_; }

private:
    static constexpr Qty kQtyMax = std::numeric_limits<Qty>::max();
    static constexpr std::uint32_t kOrdersMax = std::numeric_limits<std::uint32_t>::max();
    static constexpr Level kEmptyLevel{};

    struct SideBook {
        explicit SideBook(std::size_t capacity) : levels(capacity), ids(capacity) {}
        std::vector<Level> levels;   // [0, size) in use, worst price first
        std::vector<LevelId> ids;    // parallel to `levels`
        std::size_t size{0};
    };

    SideBook& side_book(Side s) noexcept { return sides_[s == Side::buy ? 0 : 1]; }
    const SideBook& side_book(Side s) const noexcept { return sides_[s == Side::buy ? 0 : 1]; }

    static constexpr bool better(Side s, Price a, Price b) noexcept {
        return s == Side::buy ? a > b : a < b;
    }

    // First index whose price is equal to or better than `price`: the position of
    // the level if it exists, else the insertion point that keeps the ordering.
    // Only comparisons are used, so no price (INT64_MIN included) can overflow.
    static std::size_t find_pos(const SideBook& sb, Side s, Price price) noexcept {
        std::size_t lo = 0;
        std::size_t hi = sb.size;
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (better(s, price, sb.levels[mid].price)) {
                lo = mid + 1;  // strictly worse than `price`
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    // Opens a hole at `pos` by moving the better levels one slot up. Requires size < capacity.
    static void shift_up(SideBook& sb, std::size_t pos) noexcept {
        std::copy_backward(sb.levels.begin() + static_cast<std::ptrdiff_t>(pos),
                           sb.levels.begin() + static_cast<std::ptrdiff_t>(sb.size),
                           sb.levels.begin() + static_cast<std::ptrdiff_t>(sb.size + 1));
        std::copy_backward(sb.ids.begin() + static_cast<std::ptrdiff_t>(pos),
                           sb.ids.begin() + static_cast<std::ptrdiff_t>(sb.size),
                           sb.ids.begin() + static_cast<std::ptrdiff_t>(sb.size + 1));
    }

    // Moves [from, to) down by one slot, overwriting slot from-1 (overlap-safe).
    static void shift_down(SideBook& sb, std::size_t from, std::size_t to) noexcept {
        std::copy(sb.levels.begin() + static_cast<std::ptrdiff_t>(from), sb.levels.begin() + static_cast<std::ptrdiff_t>(to),
                  sb.levels.begin() + static_cast<std::ptrdiff_t>(from - 1));
        std::copy(sb.ids.begin() + static_cast<std::ptrdiff_t>(from), sb.ids.begin() + static_cast<std::ptrdiff_t>(to),
                  sb.ids.begin() + static_cast<std::ptrdiff_t>(from - 1));
    }

    bool take(Side side, Price price, Qty qty, LevelId id, bool whole_order) noexcept {
        if (qty == 0) return false;
        SideBook& sb = side_book(side);
        const std::size_t pos = find_pos(sb, side, price);
        if (pos >= sb.size || sb.levels[pos].price != price) return false;
        if (id != kNoLevel && sb.ids[pos] != id) return false;
        Level& lv = sb.levels[pos];
        if (qty > lv.qty) return false;  // would drive the total negative: refuse, keep the book intact

        lv.qty -= qty;
        if (lv.qty == 0) {
            // Erase: the better levels above slide down. Erasing the best level moves nothing.
            shift_down(sb, pos + 1, sb.size);
            --sb.size;
        } else if (whole_order && lv.orders > 1) {
            --lv.orders;  // a surviving level always keeps at least one order
        }
        ++updates_;
        return true;
    }

    SideBook sides_[2];
    LevelId next_id_{1};
    std::uint64_t updates_{0};
    std::uint64_t overflows_{0};
};

}  // namespace optitrade::book

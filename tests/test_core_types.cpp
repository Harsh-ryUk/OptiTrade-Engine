// Fundamental types (Side, Symbol, numeric aliases and limits) and DecodeStatus.

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>

#include "check.hpp"
#include "optitrade/core/decode_status.hpp"
#include "optitrade/core/types.hpp"

using namespace optitrade;

namespace {

// view() points into its Symbol; copying the text out lets a test inspect a
// temporary Symbol without leaving a dangling view behind.
std::string text_of(const Symbol& s) { return std::string(s.view()); }

}  // namespace

// ---------------------------------------------------------------------------
// Numeric aliases and limits
// ---------------------------------------------------------------------------

OT_TEST(numeric_aliases_match_the_wire_formats) {
    static_assert(std::is_same_v<Locate, std::uint16_t>, "ITCH stock locate is two bytes");
    static_assert(std::is_same_v<Qty, std::uint32_t>, "ITCH/OUCH shares are four bytes");
    static_assert(std::is_same_v<OrderRef, std::uint64_t>, "ITCH order reference is eight bytes");
    static_assert(std::is_same_v<Nanos, std::uint64_t>);
    static_assert(std::is_signed_v<Price> && sizeof(Price) == 8, "signed 64 bit: PnL arithmetic goes negative");
    OT_CHECK(true);
}

OT_TEST(price_scale_and_quantity_limit) {
    OT_CHECK_EQ(kPriceScale, Price{10'000});
    OT_CHECK_EQ(kMaxOrderQty, Qty{999'999});
    OT_CHECK(kMaxOrderQty < 1'000'000u);  // OUCH: strictly below one million
    OT_CHECK(kMaxOrderQty + 1 == 1'000'000u);
}

// The header promises that price * qty cannot overflow for any legal ITCH price.
OT_TEST(notional_arithmetic_has_headroom) {
    constexpr Price kMaxWirePrice = std::numeric_limits<std::uint32_t>::max();  // 429'496.7295
    constexpr Price kMaxNotional = kMaxWirePrice * static_cast<Price>(kMaxOrderQty);
    static_assert(kMaxNotional > 0);
    static_assert(kMaxNotional < std::numeric_limits<Price>::max() / 1000, "sums of thousands of orders still fit");
    OT_CHECK_EQ(kMaxNotional, Price{4'294'967'295LL * 999'999LL});
    // The nominal ITCH ceiling, $200,000.0000, in price units.
    OT_CHECK_EQ(Price{200'000} * kPriceScale, Price{2'000'000'000});
}

// ---------------------------------------------------------------------------
// Side
// ---------------------------------------------------------------------------

OT_TEST(side_opposite_and_index) {
    OT_CHECK(opposite(Side::buy) == Side::sell);
    OT_CHECK(opposite(Side::sell) == Side::buy);
    OT_CHECK(opposite(opposite(Side::buy)) == Side::buy);
    OT_CHECK(opposite(opposite(Side::sell)) == Side::sell);
    OT_CHECK_EQ(index(Side::buy), std::size_t{0});
    OT_CHECK_EQ(index(Side::sell), std::size_t{1});
    OT_CHECK(index(Side::buy) != index(opposite(Side::buy)));
    static_assert(opposite(Side::buy) == Side::sell && index(Side::sell) == 1, "usable at compile time");
    static_assert(noexcept(opposite(Side::buy)) && noexcept(index(Side::buy)));
}

// Book code indexes two-element arrays by side.
OT_TEST(side_index_addresses_a_two_element_array) {
    std::array<int, 2> per_side{};
    per_side[index(Side::buy)] = 10;
    per_side[index(Side::sell)] = 20;
    OT_CHECK_EQ(per_side[0], 10);
    OT_CHECK_EQ(per_side[1], 20);
    OT_CHECK_EQ(per_side[index(opposite(Side::buy))], 20);
    static_assert(sizeof(Side) == 1);
    static_assert(static_cast<int>(Side::buy) == 0 && static_cast<int>(Side::sell) == 1);
}

// ---------------------------------------------------------------------------
// Symbol
// ---------------------------------------------------------------------------

OT_TEST(symbol_is_a_small_trivially_copyable_value) {
    static_assert(Symbol::kSize == 8);
    static_assert(sizeof(Symbol) == 8, "exactly the wire field, so it can be hashed as one word");
    static_assert(std::is_trivially_copyable_v<Symbol>);
    static_assert(std::is_nothrow_default_constructible_v<Symbol>);
    static_assert(!std::is_convertible_v<std::string_view, Symbol>, "construction from text is explicit");
    static_assert(!std::is_convertible_v<const char*, Symbol>);
    OT_CHECK(true);
}

OT_TEST(default_symbol_is_empty_and_space_filled) {
    const Symbol s;
    OT_CHECK(s.empty());
    OT_CHECK_EQ(s.view().size(), std::size_t{0});
    for (char c : s.raw()) OT_CHECK_EQ(c, ' ');
    OT_CHECK(s == Symbol(""));
    OT_CHECK(s == Symbol("        "));
}

OT_TEST(symbol_pads_short_text_with_spaces) {
    const Symbol s("AAPL");
    const std::array<char, 8> expected{'A', 'A', 'P', 'L', ' ', ' ', ' ', ' '};
    OT_CHECK(s.raw() == expected);
    OT_CHECK_EQ(s.view(), std::string_view("AAPL"));
    OT_CHECK_EQ(s.view().size(), std::size_t{4});
    OT_CHECK(!s.empty());

    const Symbol one("X");
    OT_CHECK_EQ(one.view(), std::string_view("X"));
    OT_CHECK_EQ(one.raw()[1], ' ');
    OT_CHECK_EQ(one.raw()[7], ' ');
}

OT_TEST(symbol_truncates_text_longer_than_eight_characters) {
    const Symbol full("ABCDEFGH");
    OT_CHECK_EQ(full.view(), std::string_view("ABCDEFGH"));
    OT_CHECK_EQ(full.view().size(), std::size_t{8});
    const Symbol longer("ABCDEFGHIJKL");
    OT_CHECK(longer == full);
    OT_CHECK_EQ(longer.view(), std::string_view("ABCDEFGH"));
}

OT_TEST(symbol_view_trims_only_trailing_padding) {
    OT_CHECK_EQ(text_of(Symbol("A   ")), std::string("A"));
    OT_CHECK_EQ(text_of(Symbol("  A")), std::string("  A"));  // leading spaces are data
    OT_CHECK_EQ(text_of(Symbol("A B")), std::string("A B"));  // so are interior ones
    OT_CHECK_EQ(text_of(Symbol("BRK.B")), std::string("BRK.B"));
    OT_CHECK_EQ(text_of(Symbol("a")), std::string("a"));  // case is preserved
    OT_CHECK(Symbol("   ").empty());
    OT_CHECK(!Symbol(" A").empty());
    OT_CHECK(!Symbol(".").empty());
}

OT_TEST(symbol_equality_is_case_and_position_sensitive) {
    OT_CHECK(Symbol("AAPL") == Symbol("AAPL"));
    OT_CHECK(Symbol("AAPL") == Symbol("AAPL    "));  // padding is implied
    OT_CHECK(!(Symbol("AAPL") != Symbol("AAPL")));
    OT_CHECK(Symbol("AAPL") != Symbol("AAPM"));
    OT_CHECK(Symbol("AAPL") != Symbol("aapl"));
    OT_CHECK(Symbol("AAPL") != Symbol("AAPL.B"));
    OT_CHECK(Symbol("AAPL") != Symbol(" AAPL"));
    OT_CHECK(Symbol("A") != Symbol());
    OT_CHECK(Symbol("ABCDEFGH") != Symbol("ABCDEFGX"));
    static_assert(Symbol("XYZ") == Symbol("XYZ  "), "equality is constexpr");
}

OT_TEST(symbol_from_wire_copies_exactly_eight_bytes) {
    // A heap block of exactly 8 bytes without a terminator: AddressSanitizer reports
    // any read past it.
    auto wire = std::make_unique<char[]>(8);
    std::memcpy(wire.get(), "MSFT    ", 8);
    const Symbol s = Symbol::from_wire(wire.get());
    OT_CHECK(s == Symbol("MSFT"));
    OT_CHECK_EQ(s.view(), std::string_view("MSFT"));

    std::memcpy(wire.get(), "ABCDEFGH", 8);
    const Symbol full = Symbol::from_wire(wire.get());
    OT_CHECK_EQ(full.view(), std::string_view("ABCDEFGH"));
    OT_CHECK(std::memcmp(full.raw().data(), "ABCDEFGH", 8) == 0);
}

// The eight bytes are kept exactly as received (they are echoed back in order
// entry); only view() interprets NUL as padding, and equality stays byte-wise.
OT_TEST(symbol_from_wire_preserves_raw_bytes) {
    const unsigned char nul_padded[8] = {'I', 'B', 'M', 0, 0, 0, 0, 0};
    const Symbol n = Symbol::from_wire(nul_padded);
    OT_CHECK_EQ(n.view(), std::string_view("IBM"));
    OT_CHECK(std::memcmp(n.raw().data(), nul_padded, 8) == 0);
    OT_CHECK(n != Symbol("IBM"));  // byte-wise: NUL padding is not space padding

    const unsigned char all_nul[8] = {};
    OT_CHECK(Symbol::from_wire(all_nul).empty());
    OT_CHECK(Symbol::from_wire(all_nul) != Symbol());

    const unsigned char interior[8] = {'A', 0, 'B', ' ', ' ', ' ', ' ', ' '};
    OT_CHECK_EQ(text_of(Symbol::from_wire(interior)), std::string("A\0B", 3));

    const unsigned char high[8] = {0xFF, 0x80, 'Z', ' ', ' ', ' ', ' ', ' '};
    const Symbol h = Symbol::from_wire(high);
    OT_CHECK(std::memcmp(h.raw().data(), high, 8) == 0);
    OT_CHECK_EQ(h.view().size(), std::size_t{3});
}

OT_TEST(symbol_copies_are_independent_values) {
    Symbol a("AAA"), b("BBB");
    OT_CHECK(a != b);
    Symbol c = a;
    OT_CHECK(c == a);
    c = b;
    OT_CHECK(c == b);
    OT_CHECK(a == Symbol("AAA"));  // the source of a copy is untouched
    std::array<Symbol, 4> table{};
    for (const Symbol& s : table) OT_CHECK(s.empty());
    table[2] = a;
    OT_CHECK(table[2] == a && table[1].empty());
}

OT_TEST(constexpr_symbol_construction) {
    constexpr Symbol s("SPY");
    static_assert(s.view() == "SPY");
    static_assert(!s.empty());
    static_assert(Symbol().empty());
    static_assert(s.raw()[3] == ' ');
    OT_CHECK(true);
}

// ---------------------------------------------------------------------------
// DecodeStatus
// ---------------------------------------------------------------------------

OT_TEST(decode_status_names) {
    OT_CHECK_EQ(std::string_view(to_string(DecodeStatus::ok)), std::string_view("ok"));
    OT_CHECK_EQ(std::string_view(to_string(DecodeStatus::truncated)), std::string_view("truncated"));
    OT_CHECK_EQ(std::string_view(to_string(DecodeStatus::unknown_type)), std::string_view("unknown_type"));
    OT_CHECK_EQ(std::string_view(to_string(DecodeStatus::bad_length)), std::string_view("bad_length"));
    OT_CHECK_EQ(std::string_view(to_string(DecodeStatus::bad_field)), std::string_view("bad_field"));
}

OT_TEST(decode_status_names_are_distinct_and_unknown_values_are_marked) {
    const DecodeStatus all[] = {DecodeStatus::ok, DecodeStatus::truncated, DecodeStatus::unknown_type,
                                DecodeStatus::bad_length, DecodeStatus::bad_field};
    std::set<std::string_view> names;
    for (DecodeStatus s : all) names.insert(to_string(s));
    OT_CHECK_EQ(names.size(), std::size_t{5});
    OT_CHECK_EQ(std::string_view(to_string(static_cast<DecodeStatus>(5))), std::string_view("?"));
    OT_CHECK_EQ(std::string_view(to_string(static_cast<DecodeStatus>(255))), std::string_view("?"));
}

// Success must be the zero value so that `if (status != DecodeStatus::ok)` and
// value-initialised results both mean what they say.
OT_TEST(decode_status_ok_is_the_zero_value) {
    OT_CHECK_EQ(static_cast<int>(DecodeStatus::ok), 0);
    OT_CHECK(DecodeStatus{} == DecodeStatus::ok);
    static_assert(sizeof(DecodeStatus) == 1);
    static_assert(std::string_view(to_string(DecodeStatus::bad_field)) == "bad_field", "usable at compile time");
    static_assert(noexcept(to_string(DecodeStatus::ok)));
}

OT_TEST_MAIN()

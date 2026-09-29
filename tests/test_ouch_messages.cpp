// OUCH 4.2 message codec: hand-written wire vectors, field boundaries, error
// paths, and a randomized differential against a naive offset-table writer.
//
// The vectors below are typed out byte by byte from the offset tables in the
// OUCH 4.2 specification. They are deliberately not produced by the encoder under
// test, and every field holds a value that differs from its neighbours so a
// swapped or shifted field cannot go unnoticed.

#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

#include "check.hpp"
#include "optitrade/core/rng.hpp"
#include "optitrade/ouch/codec.hpp"

using namespace optitrade;
using namespace optitrade::ouch;

namespace {

using Bytes = std::vector<std::byte>;
constexpr std::size_t kSame = static_cast<std::size_t>(-1);

// ---- wire vectors -----------------------------------------------------------

constexpr std::uint8_t kEnter[] = {
    'O',                                                                   // 0   type
    'T', 'O', 'K', 'E', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '1',  // 1   token
    'B',                                                                   // 15  buy/sell
    0x00, 0x00, 0x00, 0x64,                                                // 16  shares 100
    'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',                                // 20  stock
    0x00, 0x12, 0xD6, 0x44,                                                // 28  price 123.4500
    0x00, 0x01, 0x86, 0x9F,                                                // 32  time in force 99999
    'A', 'B', 'C', 'D',                                                    // 36  firm
    'Y',                                                                   // 40  display
    'P',                                                                   // 41  capacity
    'N',                                                                   // 42  intermarket sweep
    0x00, 0x00, 0x00, 0x0A,                                                // 43  minimum quantity 10
    'N',                                                                   // 47  cross type
    'R',                                                                   // 48  customer type
};
static_assert(sizeof(kEnter) == 49);

constexpr std::uint8_t kReplace[] = {
    'U',                                                                   // 0   type
    'T', 'O', 'K', 'E', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '1',  // 1   existing token
    'T', 'O', 'K', 'E', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '2',  // 15  replacement token
    0x00, 0x00, 0x00, 0xFA,                                                // 29  shares 250
    0x00, 0x12, 0xD6, 0xA8,                                                // 33  price 123.4600
    0x00, 0x00, 0x0E, 0x10,                                                // 37  time in force 3600
    'A',                                                                   // 41  display
    'y',                                                                   // 42  intermarket sweep
    0x00, 0x00, 0x00, 0x19,                                                // 43  minimum quantity 25
};
static_assert(sizeof(kReplace) == 47);

constexpr std::uint8_t kCancel[] = {
    'X',                                                                   // 0   type
    'T', 'O', 'K', 'E', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '1',  // 1   token
    0x00, 0x00, 0x00, 0x3C,                                                // 15  intended size 60
};
static_assert(sizeof(kCancel) == 19);

constexpr std::uint8_t kAccepted[] = {
    'A',                                                                   // 0   type
    0x00, 0x00, 0x1F, 0x1E, 0x2C, 0x3D, 0x4E, 0x5F,                        // 1   timestamp
    'T', 'O', 'K', 'E', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '1',  // 9   token
    'S',                                                                   // 23  buy/sell
    0x00, 0x00, 0x00, 0x64,                                                // 24  shares 100
    'M', 'S', 'F', 'T', ' ', ' ', ' ', ' ',                                // 28  stock
    0x00, 0x2D, 0xC6, 0xC0,                                                // 36  price 300.0000
    0x00, 0x01, 0x86, 0x9E,                                                // 40  time in force 99998
    'N', 'S', 'D', 'Q',                                                    // 44  firm
    'Y',                                                                   // 48  display
    0x00, 0x00, 0x00, 0x00, 0x00, 0xBC, 0x61, 0x4E,                        // 49  reference 12345678
    'A',                                                                   // 57  capacity
    'Y',                                                                   // 58  intermarket sweep
    0x00, 0x00, 0x00, 0x05,                                                // 59  minimum quantity 5
    'O',                                                                   // 63  cross type
    'L',                                                                   // 64  order state
    '1',                                                                   // 65  BBO weight
};
static_assert(sizeof(kAccepted) == 66);

constexpr std::uint8_t kReplaced[] = {
    'U',                                                                   // 0   type
    0x00, 0x00, 0x1F, 0x1E, 0x2C, 0x3D, 0x4E, 0x5F,                        // 1   timestamp
    'T', 'O', 'K', 'E', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '2',  // 9   replacement token
    'B',                                                                   // 23  buy/sell
    0x00, 0x00, 0x01, 0x90,                                                // 24  shares 400
    'M', 'S', 'F', 'T', ' ', ' ', ' ', ' ',                                // 28  stock
    0x00, 0x2D, 0xC7, 0x08,                                                // 36  price 300.0072
    0x00, 0x00, 0x0E, 0x10,                                                // 40  time in force 3600
    'N', 'S', 'D', 'Q',                                                    // 44  firm
    'N',                                                                   // 48  display
    0x00, 0x00, 0x00, 0x00, 0x00, 0xBC, 0x61, 0x4F,                        // 49  reference 12345679
    'P',                                                                   // 57  capacity
    'N',                                                                   // 58  intermarket sweep
    0x00, 0x00, 0x00, 0x0A,                                                // 59  minimum quantity 10
    'C',                                                                   // 63  cross type
    'D',                                                                   // 64  order state
    'T', 'O', 'K', 'E', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '1',  // 65  previous token
    'S',                                                                   // 79  BBO weight
};
static_assert(sizeof(kReplaced) == 80);

constexpr std::uint8_t kCanceled[] = {
    'C',                                                                   // 0   type
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,                        // 1   timestamp
    'T', 'O', 'K', 'E', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '1',  // 9   token
    0x00, 0x00, 0x03, 0xE8,                                                // 23  decrement 1000
    'I',                                                                   // 27  reason
};
static_assert(sizeof(kCanceled) == 28);

constexpr std::uint8_t kExecuted[] = {
    'E',                                                                   // 0   type
    0x00, 0x00, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,                        // 1   timestamp
    'T', 'O', 'K', 'E', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '1',  // 9   token
    0x00, 0x00, 0x00, 0x32,                                                // 23  executed shares 50
    0x00, 0x12, 0xD6, 0x44,                                                // 27  price 123.4500
    'R',                                                                   // 31  liquidity flag
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,                        // 32  match number
};
static_assert(sizeof(kExecuted) == 40);

constexpr std::uint8_t kRejected[] = {
    'J',                                                                   // 0   type
    0x00, 0x00, 0x00, 0x00, 0x3B, 0x9A, 0xCA, 0x00,                        // 1   timestamp 1e9
    'T', 'O', 'K', 'E', 'N', '0', '0', '0', '0', '0', '0', '0', '0', '1',  // 9   token
    'S',                                                                   // 23  reason
};
static_assert(sizeof(kRejected) == 24);

// ---- helpers ----------------------------------------------------------------

template <std::size_t N>
Bytes to_bytes(const std::uint8_t (&a)[N]) {
    Bytes v(N);
    for (std::size_t i = 0; i < N; ++i) v[i] = static_cast<std::byte>(a[i]);
    return v;
}

// Exactly-sized copy so AddressSanitizer flags any read past the message.
Bytes prefix(const Bytes& v, std::size_t n) { return Bytes(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n)); }

std::span<const std::byte> view(const Bytes& v) { return {v.data(), v.size()}; }

std::size_t first_diff(const Bytes& a, const Bytes& b) {
    if (a.size() != b.size()) return 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) return i;
    }
    return kSame;
}

Bytes encoded(const auto& m) {
    Bytes buf(kMaxMessageLength);
    const std::size_t n = encode(m, std::span<std::byte>(buf));
    buf.resize(n);
    return buf;
}

void set_be(Bytes& v, std::size_t off, std::size_t len, std::uint64_t value) {
    for (std::size_t i = 0; i < len; ++i) {
        v[off + i] = static_cast<std::byte>(value >> (8 * (len - 1 - i)));
    }
}

struct Rec : NullHandler {
    using NullHandler::on;
    int calls = 0;
    EnterOrder enter;
    ReplaceOrder replace;
    CancelOrder cancel;
    Accepted accepted;
    Replaced replaced;
    Canceled canceled;
    Executed executed;
    Rejected rejected;
    void on(const EnterOrder& m) noexcept { enter = m; ++calls; }
    void on(const ReplaceOrder& m) noexcept { replace = m; ++calls; }
    void on(const CancelOrder& m) noexcept { cancel = m; ++calls; }
    void on(const Accepted& m) noexcept { accepted = m; ++calls; }
    void on(const Replaced& m) noexcept { replaced = m; ++calls; }
    void on(const Canceled& m) noexcept { canceled = m; ++calls; }
    void on(const Executed& m) noexcept { executed = m; ++calls; }
    void on(const Rejected& m) noexcept { rejected = m; ++calls; }
};

DecodeStatus dec_in(const Bytes& v, Rec& r) { return decode_inbound(view(v), r); }
DecodeStatus dec_out(const Bytes& v, Rec& r) { return decode_outbound(view(v), r); }

bool firm_is(const char (&f)[4], const char* text) {
    return f[0] == text[0] && f[1] == text[1] && f[2] == text[2] && f[3] == text[3];
}

}  // namespace

// ---- Token ------------------------------------------------------------------

OT_TEST(token_from_id_is_fourteen_digit_zero_padded) {
    OT_CHECK(Token::from_id(0).view() == "00000000000000");
    OT_CHECK(Token::from_id(1).view() == "00000000000001");
    OT_CHECK(Token::from_id(42).view() == "00000000000042");
    OT_CHECK(Token::from_id(1234567890123).view() == "01234567890123");
    OT_CHECK_EQ(Token::from_id(7).raw().size(), std::size_t{14});
}

OT_TEST(token_max_id_fits_in_fourteen_digits) {
    OT_CHECK_EQ(Token::kMaxId, std::uint64_t{99'999'999'999'999ULL});
    const Token t = Token::from_id(Token::kMaxId);
    OT_CHECK(t.view() == "99999999999999");
    OT_CHECK(t.to_id().has_value());
    OT_CHECK_EQ(t.to_id().value_or(0), Token::kMaxId);
}

OT_TEST(token_id_beyond_fourteen_digits_is_rejected) {
    for (std::uint64_t id : {Token::kMaxId + 1, std::uint64_t{100'000'000'000'000ULL},
                             std::uint64_t{1} << 63, ~std::uint64_t{0}}) {
        const Token t = Token::from_id(id);
        OT_CHECK(t == Token::invalid());
        OT_CHECK(!t.to_id().has_value());
    }
    // The invalid token is distinct from every legal one.
    OT_CHECK(!(Token::invalid() == Token::from_id(0)));
}

OT_TEST(token_to_id_round_trips) {
    for (std::uint64_t id : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{9}, std::uint64_t{10},
                             std::uint64_t{99'999'999'999'998ULL}, Token::kMaxId}) {
        const auto back = Token::from_id(id).to_id();
        OT_CHECK(back.has_value());
        if (back) OT_CHECK_EQ(*back, id);
    }
}

OT_TEST(token_to_id_parses_digits_with_trailing_spaces_only) {
    OT_CHECK_EQ(Token::from_text("42").to_id().value_or(0), std::uint64_t{42});
    OT_CHECK_EQ(Token::from_text("00000000000042").to_id().value_or(0), std::uint64_t{42});
    OT_CHECK_EQ(Token::from_text("7 ").to_id().value_or(0), std::uint64_t{7});
    OT_CHECK_EQ(Token::from_text("12345678901234").to_id().value_or(0), std::uint64_t{12345678901234ULL});

    OT_CHECK(!Token().to_id().has_value());                        // all spaces
    OT_CHECK(!Token::from_text(" 42").to_id().has_value());        // leading space
    OT_CHECK(!Token::from_text("4 2").to_id().has_value());        // embedded space
    OT_CHECK(!Token::from_text("42x").to_id().has_value());        // trailing junk
    OT_CHECK(!Token::from_text("x42").to_id().has_value());
    OT_CHECK(!Token::from_text("TOKEN000000001").to_id().has_value());
    OT_CHECK(!Token::from_text("-0000000000001").to_id().has_value());
    OT_CHECK(!Token::from_text("+0000000000001").to_id().has_value());
    OT_CHECK(!Token::from_text("1.5").to_id().has_value());
    // Text longer than the field is cut at 14 characters before parsing.
    OT_CHECK(Token::from_text("12345678901234x").to_id().has_value());

    // Bytes that are not ASCII digits, including NUL and bytes above 0x7F whose
    // low seven bits happen to spell a digit (0xB0 is '0' with the top bit set).
    const char with_nul[kTokenSize] = {'1', '2', '\0', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    OT_CHECK(!Token::from_wire(with_nul).to_id().has_value());
    for (int high_byte : {0xB0, 0xB2, 0x80, 0xFF}) {
        char raw[kTokenSize];
        for (char& c : raw) c = ' ';
        raw[0] = '1';
        raw[1] = static_cast<char>(high_byte);
        OT_CHECK(!Token::from_wire(raw).to_id().has_value());
    }
}

OT_TEST(token_default_is_blank_and_text_is_padded_and_truncated) {
    OT_CHECK(Token().view().empty());
    const Token blank;  // raw() refers into the token, so it must outlive the loop
    for (char c : blank.raw()) OT_CHECK_EQ(c, ' ');
    OT_CHECK(Token::from_text("AB").view() == "AB");
    OT_CHECK(Token::from_text("AB").raw()[2] == ' ');
    OT_CHECK(Token::from_text("ABCDEFGHIJKLMNOPQRST").view() == "ABCDEFGHIJKLMN");
    OT_CHECK(Token::from_text("AB") == Token::from_text("AB  "));
    OT_CHECK(!(Token::from_text("AB") == Token::from_text("AC")));
}

// ---- length tables ----------------------------------------------------------

OT_TEST(length_tables_match_the_spec) {
    OT_CHECK_EQ(inbound_length('O'), std::size_t{49});
    OT_CHECK_EQ(inbound_length('U'), std::size_t{47});
    OT_CHECK_EQ(inbound_length('X'), std::size_t{19});
    OT_CHECK_EQ(outbound_length('A'), std::size_t{66});
    OT_CHECK_EQ(outbound_length('U'), std::size_t{80});
    OT_CHECK_EQ(outbound_length('C'), std::size_t{28});
    OT_CHECK_EQ(outbound_length('E'), std::size_t{40});
    OT_CHECK_EQ(outbound_length('J'), std::size_t{24});
    OT_CHECK_EQ(kMaxMessageLength, std::size_t{80});

    for (int c = 0; c < 256; ++c) {
        const char ch = static_cast<char>(c);
        const bool in_known = ch == 'O' || ch == 'U' || ch == 'X';
        const bool out_known = ch == 'A' || ch == 'U' || ch == 'C' || ch == 'E' || ch == 'J';
        OT_CHECK_EQ(inbound_length(ch) != 0, in_known);
        OT_CHECK_EQ(outbound_length(ch) != 0, out_known);
    }
}

// ---- known-answer decode and encode -----------------------------------------

OT_TEST(enter_order_known_answer) {
    const Bytes wire = to_bytes(kEnter);
    Rec r;
    OT_CHECK_EQ(dec_in(wire, r), DecodeStatus::ok);
    OT_CHECK_EQ(r.calls, 1);
    const EnterOrder& m = r.enter;
    OT_CHECK(m.token == Token::from_text("TOKEN000000001"));
    OT_CHECK(m.side == Side::buy);
    OT_CHECK(!m.short_sell);
    OT_CHECK_EQ(m.shares, Qty{100});
    OT_CHECK(m.stock == Symbol("AAPL"));
    OT_CHECK_EQ(m.price, Price{1'234'500});
    OT_CHECK_EQ(m.time_in_force, kTifSystemHours);
    OT_CHECK(firm_is(m.firm, "ABCD"));
    OT_CHECK_EQ(m.display, 'Y');
    OT_CHECK_EQ(m.capacity, 'P');
    OT_CHECK_EQ(m.intermarket_sweep, 'N');
    OT_CHECK_EQ(m.min_qty, Qty{10});
    OT_CHECK_EQ(m.cross_type, 'N');
    OT_CHECK_EQ(m.customer_type, 'R');

    EnterOrder e;
    e.token = Token::from_text("TOKEN000000001");
    e.side = Side::buy;
    e.shares = 100;
    e.stock = Symbol("AAPL");
    e.price = 1'234'500;
    e.time_in_force = kTifSystemHours;
    std::memcpy(e.firm, "ABCD", 4);
    e.display = 'Y';
    e.capacity = 'P';
    e.intermarket_sweep = 'N';
    e.min_qty = 10;
    e.cross_type = 'N';
    e.customer_type = 'R';
    OT_CHECK_EQ(first_diff(encoded(e), wire), kSame);
    OT_CHECK(m == e);
}

OT_TEST(replace_order_known_answer) {
    const Bytes wire = to_bytes(kReplace);
    Rec r;
    OT_CHECK_EQ(dec_in(wire, r), DecodeStatus::ok);
    OT_CHECK_EQ(r.calls, 1);
    const ReplaceOrder& m = r.replace;
    OT_CHECK(m.existing == Token::from_text("TOKEN000000001"));
    OT_CHECK(m.replacement == Token::from_text("TOKEN000000002"));
    OT_CHECK_EQ(m.shares, Qty{250});
    OT_CHECK_EQ(m.price, Price{1'234'600});
    OT_CHECK_EQ(m.time_in_force, std::uint32_t{3600});
    OT_CHECK_EQ(m.display, 'A');
    OT_CHECK_EQ(m.intermarket_sweep, 'y');
    OT_CHECK_EQ(m.min_qty, Qty{25});

    ReplaceOrder e;
    e.existing = Token::from_text("TOKEN000000001");
    e.replacement = Token::from_text("TOKEN000000002");
    e.shares = 250;
    e.price = 1'234'600;
    e.time_in_force = 3600;
    e.display = 'A';
    e.intermarket_sweep = 'y';
    e.min_qty = 25;
    OT_CHECK_EQ(first_diff(encoded(e), wire), kSame);
}

OT_TEST(cancel_order_known_answer) {
    const Bytes wire = to_bytes(kCancel);
    Rec r;
    OT_CHECK_EQ(dec_in(wire, r), DecodeStatus::ok);
    OT_CHECK(r.cancel.token == Token::from_text("TOKEN000000001"));
    OT_CHECK_EQ(r.cancel.shares, Qty{60});
    OT_CHECK_EQ(first_diff(encoded(CancelOrder{Token::from_text("TOKEN000000001"), 60}), wire), kSame);

    // Zero means cancel everything and is a legal size on the wire.
    Bytes all = wire;
    set_be(all, 15, 4, 0);
    Rec r0;
    OT_CHECK_EQ(dec_in(all, r0), DecodeStatus::ok);
    OT_CHECK_EQ(r0.cancel.shares, Qty{0});
}

OT_TEST(accepted_known_answer) {
    const Bytes wire = to_bytes(kAccepted);
    Rec r;
    OT_CHECK_EQ(dec_out(wire, r), DecodeStatus::ok);
    OT_CHECK_EQ(r.calls, 1);
    const Accepted& m = r.accepted;
    OT_CHECK_EQ(m.ts, Nanos{0x1F1E2C3D4E5FULL});
    OT_CHECK(m.token == Token::from_text("TOKEN000000001"));
    OT_CHECK(m.side == Side::sell);
    OT_CHECK_EQ(m.shares, Qty{100});
    OT_CHECK(m.stock == Symbol("MSFT"));
    OT_CHECK_EQ(m.price, Price{3'000'000});
    OT_CHECK_EQ(m.time_in_force, kTifMarketHours);
    OT_CHECK(firm_is(m.firm, "NSDQ"));
    OT_CHECK_EQ(m.display, 'Y');
    OT_CHECK_EQ(m.ref, OrderRef{12'345'678});
    OT_CHECK_EQ(m.capacity, 'A');
    OT_CHECK_EQ(m.intermarket_sweep, 'Y');
    OT_CHECK_EQ(m.min_qty, Qty{5});
    OT_CHECK_EQ(m.cross_type, 'O');
    OT_CHECK_EQ(m.order_state, 'L');
    OT_CHECK_EQ(m.bbo_weight, '1');
    OT_CHECK_EQ(first_diff(encoded(m), wire), kSame);
}

OT_TEST(replaced_known_answer_puts_previous_token_and_bbo_after_order_state) {
    const Bytes wire = to_bytes(kReplaced);
    Rec r;
    OT_CHECK_EQ(dec_out(wire, r), DecodeStatus::ok);
    OT_CHECK_EQ(r.calls, 1);
    const Replaced& m = r.replaced;
    OT_CHECK_EQ(m.a.ts, Nanos{0x1F1E2C3D4E5FULL});
    OT_CHECK(m.a.token == Token::from_text("TOKEN000000002"));  // the replacement token
    OT_CHECK(m.previous == Token::from_text("TOKEN000000001"));
    OT_CHECK(m.a.side == Side::buy);
    OT_CHECK_EQ(m.a.shares, Qty{400});
    OT_CHECK(m.a.stock == Symbol("MSFT"));
    OT_CHECK_EQ(m.a.price, Price{3'000'072});
    OT_CHECK_EQ(m.a.time_in_force, std::uint32_t{3600});
    OT_CHECK(firm_is(m.a.firm, "NSDQ"));
    OT_CHECK_EQ(m.a.display, 'N');
    OT_CHECK_EQ(m.a.ref, OrderRef{12'345'679});
    OT_CHECK_EQ(m.a.capacity, 'P');
    OT_CHECK_EQ(m.a.intermarket_sweep, 'N');
    OT_CHECK_EQ(m.a.min_qty, Qty{10});
    OT_CHECK_EQ(m.a.cross_type, 'C');
    OT_CHECK_EQ(m.a.order_state, 'D');
    OT_CHECK_EQ(m.a.bbo_weight, 'S');  // byte 79, not byte 65
    OT_CHECK_EQ(first_diff(encoded(m), wire), kSame);
}

OT_TEST(canceled_known_answer) {
    const Bytes wire = to_bytes(kCanceled);
    Rec r;
    OT_CHECK_EQ(dec_out(wire, r), DecodeStatus::ok);
    OT_CHECK_EQ(r.canceled.ts, Nanos{0x0102030405060708ULL});
    OT_CHECK(r.canceled.token == Token::from_text("TOKEN000000001"));
    OT_CHECK_EQ(r.canceled.decrement, Qty{1000});
    OT_CHECK_EQ(r.canceled.reason, 'I');
    OT_CHECK_EQ(first_diff(encoded(r.canceled), wire), kSame);
}

OT_TEST(executed_known_answer) {
    const Bytes wire = to_bytes(kExecuted);
    Rec r;
    OT_CHECK_EQ(dec_out(wire, r), DecodeStatus::ok);
    OT_CHECK_EQ(r.executed.ts, Nanos{0x0A0B0C0D0E0FULL});
    OT_CHECK(r.executed.token == Token::from_text("TOKEN000000001"));
    OT_CHECK_EQ(r.executed.shares, Qty{50});
    OT_CHECK_EQ(r.executed.price, Price{1'234'500});
    OT_CHECK_EQ(r.executed.liquidity, 'R');
    OT_CHECK_EQ(r.executed.match, std::uint64_t{0x1112131415161718ULL});
    OT_CHECK_EQ(first_diff(encoded(r.executed), wire), kSame);
}

OT_TEST(rejected_known_answer) {
    const Bytes wire = to_bytes(kRejected);
    Rec r;
    OT_CHECK_EQ(dec_out(wire, r), DecodeStatus::ok);
    OT_CHECK_EQ(r.rejected.ts, Nanos{1'000'000'000});
    OT_CHECK(r.rejected.token == Token::from_text("TOKEN000000001"));
    OT_CHECK_EQ(r.rejected.reason, 'S');
    OT_CHECK_EQ(first_diff(encoded(r.rejected), wire), kSame);
}

// ---- sides ------------------------------------------------------------------

OT_TEST(short_sell_sides_decode_as_sell_with_flag) {
    for (char code : {'T', 'E'}) {
        Bytes wire = to_bytes(kEnter);
        wire[15] = static_cast<std::byte>(code);
        Rec r;
        OT_CHECK_EQ(dec_in(wire, r), DecodeStatus::ok);
        OT_CHECK(r.enter.side == Side::sell);
        OT_CHECK(r.enter.short_sell);
    }
    Bytes plain_sell = to_bytes(kEnter);
    plain_sell[15] = static_cast<std::byte>('S');
    Rec r;
    OT_CHECK_EQ(dec_in(plain_sell, r), DecodeStatus::ok);
    OT_CHECK(r.enter.side == Side::sell);
    OT_CHECK(!r.enter.short_sell);
}

OT_TEST(encoder_writes_the_side_byte_for_each_combination) {
    EnterOrder e;
    e.price = 1;
    e.shares = 1;
    e.side = Side::buy;
    OT_CHECK_EQ(static_cast<char>(encoded(e)[15]), 'B');
    e.short_sell = true;  // meaningless on a buy, ignored
    OT_CHECK_EQ(static_cast<char>(encoded(e)[15]), 'B');
    e.side = Side::sell;
    OT_CHECK_EQ(static_cast<char>(encoded(e)[15]), 'T');
    e.short_sell = false;
    OT_CHECK_EQ(static_cast<char>(encoded(e)[15]), 'S');
}

OT_TEST(outbound_short_sides_read_as_sell_and_write_back_as_s) {
    for (char code : {'T', 'E', 'S'}) {
        Bytes wire = to_bytes(kAccepted);
        wire[23] = static_cast<std::byte>(code);
        Rec r;
        OT_CHECK_EQ(dec_out(wire, r), DecodeStatus::ok);
        OT_CHECK(r.accepted.side == Side::sell);
        OT_CHECK_EQ(static_cast<char>(encoded(r.accepted)[23]), 'S');
    }
    Bytes wire = to_bytes(kReplaced);
    wire[23] = static_cast<std::byte>('T');
    Rec r;
    OT_CHECK_EQ(dec_out(wire, r), DecodeStatus::ok);
    OT_CHECK(r.replaced.a.side == Side::sell);
}

// ---- value extremes ---------------------------------------------------------

OT_TEST(enter_order_extreme_values) {
    Bytes wire = to_bytes(kEnter);
    set_be(wire, 16, 4, 999'999);         // largest legal size
    set_be(wire, 28, 4, 0xFFFFFFFFu);     // largest wire price
    set_be(wire, 32, 4, 0xFFFFFFFFu);     // time in force is opaque
    set_be(wire, 43, 4, 0xFFFFFFFFu);
    Rec r;
    OT_CHECK_EQ(dec_in(wire, r), DecodeStatus::ok);
    OT_CHECK_EQ(r.enter.shares, Qty{999'999});
    OT_CHECK_EQ(r.enter.price, Price{4'294'967'295LL});
    OT_CHECK_EQ(r.enter.time_in_force, std::uint32_t{0xFFFFFFFFu});
    OT_CHECK_EQ(r.enter.min_qty, Qty{0xFFFFFFFFu});
    OT_CHECK_EQ(first_diff(encoded(r.enter), wire), kSame);

    set_be(wire, 16, 4, 1);  // smallest legal size
    set_be(wire, 28, 4, 0);  // price zero is not the decoder's business
    Rec r2;
    OT_CHECK_EQ(dec_in(wire, r2), DecodeStatus::ok);
    OT_CHECK_EQ(r2.enter.shares, Qty{1});
    OT_CHECK_EQ(r2.enter.price, Price{0});
    OT_CHECK_EQ(first_diff(encoded(r2.enter), wire), kSame);

    set_be(wire, 28, 4, 0x7FFFFFFFu);  // market-order price is passed through
    Rec r3;
    OT_CHECK_EQ(dec_in(wire, r3), DecodeStatus::ok);
    OT_CHECK_EQ(r3.enter.price, Price{0x7FFFFFFF});
}

OT_TEST(outbound_extreme_values) {
    Bytes wire = to_bytes(kAccepted);
    set_be(wire, 1, 8, ~std::uint64_t{0});
    set_be(wire, 49, 8, ~std::uint64_t{0});
    set_be(wire, 36, 4, 0xFFFFFFFFu);
    Rec r;
    OT_CHECK_EQ(dec_out(wire, r), DecodeStatus::ok);
    OT_CHECK_EQ(r.accepted.ts, ~std::uint64_t{0});      // full 64 bits, not 48
    OT_CHECK_EQ(r.accepted.ref, ~std::uint64_t{0});
    OT_CHECK_EQ(r.accepted.price, Price{4'294'967'295LL});
    OT_CHECK_EQ(first_diff(encoded(r.accepted), wire), kSame);

    Bytes ex = to_bytes(kExecuted);
    set_be(ex, 1, 8, ~std::uint64_t{0});
    set_be(ex, 23, 4, 0xFFFFFFFFu);
    set_be(ex, 27, 4, 0xFFFFFFFFu);
    set_be(ex, 32, 8, ~std::uint64_t{0});
    Rec r2;
    OT_CHECK_EQ(dec_out(ex, r2), DecodeStatus::ok);
    OT_CHECK_EQ(r2.executed.shares, Qty{0xFFFFFFFFu});
    OT_CHECK_EQ(r2.executed.match, ~std::uint64_t{0});
    OT_CHECK_EQ(first_diff(encoded(r2.executed), ex), kSame);

    // Outbound share counts are not range checked: the exchange is the authority.
    Bytes zero = to_bytes(kAccepted);
    set_be(zero, 24, 4, 0);
    Rec r3;
    OT_CHECK_EQ(dec_out(zero, r3), DecodeStatus::ok);
    OT_CHECK_EQ(r3.accepted.shares, Qty{0});
}

// ---- bad_field --------------------------------------------------------------

OT_TEST(enter_and_replace_reject_out_of_range_shares) {
    const struct { const std::uint8_t* wire; std::size_t size; std::size_t shares_off; } cases[] = {
        {kEnter, sizeof kEnter, 16}, {kReplace, sizeof kReplace, 29}};
    for (const auto& c : cases) {
        Bytes base(c.size);
        for (std::size_t i = 0; i < c.size; ++i) base[i] = static_cast<std::byte>(c.wire[i]);
        for (std::uint32_t bad : {0u, 1'000'000u, 1'000'001u, 0x80000000u, 0xFFFFFFFFu}) {
            Bytes wire = base;
            set_be(wire, c.shares_off, 4, bad);
            Rec r;
            OT_CHECK_EQ(dec_in(wire, r), DecodeStatus::bad_field);
            OT_CHECK_EQ(r.calls, 0);
        }
        for (std::uint32_t good : {1u, 999'999u}) {
            Bytes wire = base;
            set_be(wire, c.shares_off, 4, good);
            Rec r;
            OT_CHECK_EQ(dec_in(wire, r), DecodeStatus::ok);
        }
    }
}

OT_TEST(cancel_accepts_every_size) {
    for (std::uint32_t size : {0u, 1u, 999'999u, 1'000'000u, 0xFFFFFFFFu}) {
        Bytes wire = to_bytes(kCancel);
        set_be(wire, 15, 4, size);
        Rec r;
        OT_CHECK_EQ(dec_in(wire, r), DecodeStatus::ok);
        OT_CHECK_EQ(r.cancel.shares, size);
    }
}

OT_TEST(unknown_buy_sell_indicator_is_bad_field) {
    for (int c = 0; c < 256; ++c) {
        const bool legal = c == 'B' || c == 'S' || c == 'T' || c == 'E';
        Bytes enter = to_bytes(kEnter);
        enter[15] = static_cast<std::byte>(c);
        Rec r1;
        OT_CHECK_EQ(dec_in(enter, r1), legal ? DecodeStatus::ok : DecodeStatus::bad_field);
        OT_CHECK_EQ(r1.calls, legal ? 1 : 0);

        Bytes accepted = to_bytes(kAccepted);
        accepted[23] = static_cast<std::byte>(c);
        Rec r2;
        OT_CHECK_EQ(dec_out(accepted, r2), legal ? DecodeStatus::ok : DecodeStatus::bad_field);
        OT_CHECK_EQ(r2.calls, legal ? 1 : 0);

        Bytes replaced = to_bytes(kReplaced);
        replaced[23] = static_cast<std::byte>(c);
        Rec r3;
        OT_CHECK_EQ(dec_out(replaced, r3), legal ? DecodeStatus::ok : DecodeStatus::bad_field);
        OT_CHECK_EQ(r3.calls, legal ? 1 : 0);
    }
}

OT_TEST(order_state_must_be_live_or_dead) {
    for (int c = 0; c < 256; ++c) {
        const bool legal = c == 'L' || c == 'D';
        Bytes accepted = to_bytes(kAccepted);
        accepted[64] = static_cast<std::byte>(c);
        Rec r1;
        OT_CHECK_EQ(dec_out(accepted, r1), legal ? DecodeStatus::ok : DecodeStatus::bad_field);
        OT_CHECK_EQ(r1.calls, legal ? 1 : 0);

        Bytes replaced = to_bytes(kReplaced);
        replaced[64] = static_cast<std::byte>(c);
        Rec r2;
        OT_CHECK_EQ(dec_out(replaced, r2), legal ? DecodeStatus::ok : DecodeStatus::bad_field);
        OT_CHECK_EQ(r2.calls, legal ? 1 : 0);
    }
    // Byte 65 of a Replaced message is the first byte of the previous token, not a state.
    Bytes replaced = to_bytes(kReplaced);
    replaced[65] = static_cast<std::byte>('?');
    Rec r;
    OT_CHECK_EQ(dec_out(replaced, r), DecodeStatus::ok);
}

OT_TEST(free_form_alpha_fields_pass_through) {
    Bytes c = to_bytes(kCanceled);
    c[27] = static_cast<std::byte>('q');  // codes outside the published list are still delivered
    Rec r1;
    OT_CHECK_EQ(dec_out(c, r1), DecodeStatus::ok);
    OT_CHECK_EQ(r1.canceled.reason, 'q');

    Bytes j = to_bytes(kRejected);
    j[23] = static_cast<std::byte>(0xE9);
    Rec r2;
    OT_CHECK_EQ(dec_out(j, r2), DecodeStatus::ok);
    OT_CHECK_EQ(static_cast<unsigned char>(r2.rejected.reason), 0xE9u);
}

// ---- framing of a single message: length, type, direction --------------------

OT_TEST(every_proper_prefix_is_truncated_and_every_extension_is_bad_length) {
    const std::vector<Bytes> inbound = {to_bytes(kEnter), to_bytes(kReplace), to_bytes(kCancel)};
    const std::vector<Bytes> outbound = {to_bytes(kAccepted), to_bytes(kReplaced), to_bytes(kCanceled),
                                         to_bytes(kExecuted), to_bytes(kRejected)};
    for (const Bytes& full : inbound) {
        for (std::size_t n = 0; n < full.size(); ++n) {
            Rec r;
            OT_CHECK_EQ(dec_in(prefix(full, n), r), DecodeStatus::truncated);
            OT_CHECK_EQ(r.calls, 0);
        }
        for (std::size_t extra = 1; extra <= 3; ++extra) {
            Bytes longer = full;
            longer.resize(full.size() + extra, std::byte{0});
            Rec r;
            OT_CHECK_EQ(dec_in(longer, r), DecodeStatus::bad_length);
            OT_CHECK_EQ(r.calls, 0);
        }
        Rec ok;
        OT_CHECK_EQ(dec_in(prefix(full, full.size()), ok), DecodeStatus::ok);
    }
    for (const Bytes& full : outbound) {
        for (std::size_t n = 0; n < full.size(); ++n) {
            Rec r;
            OT_CHECK_EQ(dec_out(prefix(full, n), r), DecodeStatus::truncated);
            OT_CHECK_EQ(r.calls, 0);
        }
        for (std::size_t extra = 1; extra <= 3; ++extra) {
            Bytes longer = full;
            longer.resize(full.size() + extra, std::byte{0});
            Rec r;
            OT_CHECK_EQ(dec_out(longer, r), DecodeStatus::bad_length);
            OT_CHECK_EQ(r.calls, 0);
        }
        Rec ok;
        OT_CHECK_EQ(dec_out(prefix(full, full.size()), ok), DecodeStatus::ok);
    }
}

OT_TEST(unsupported_outbound_types_are_unknown_type) {
    // Sizes are those of the spec tables: System Event 10, AIQ Canceled 38, Broken
    // Trade 32, Executed with Reference Price 45, Cancel Pending 23, Cancel Reject
    // 23, Order Priority Update 36, Order Modified 28. Whatever the size, the type
    // alone decides.
    const struct { char type; std::size_t size; } cases[] = {
        {'S', 10}, {'D', 38}, {'B', 32}, {'G', 45}, {'P', 23}, {'I', 23}, {'T', 36}, {'M', 28}};
    for (const auto& c : cases) {
        for (std::size_t size : {std::size_t{1}, c.size, c.size + 1, std::size_t{80}, std::size_t{200}}) {
            Bytes wire(size, std::byte{0});
            wire[0] = static_cast<std::byte>(c.type);
            Rec r;
            OT_CHECK_EQ(dec_out(wire, r), DecodeStatus::unknown_type);
            OT_CHECK_EQ(r.calls, 0);
        }
    }
}

OT_TEST(unsupported_inbound_types_are_unknown_type) {
    for (char type : {'M', 'A', 'C', 'E', 'J', 'S', ' ', '\0'}) {
        Bytes wire(60, std::byte{0});
        wire[0] = static_cast<std::byte>(type);
        Rec r;
        OT_CHECK_EQ(dec_in(wire, r), DecodeStatus::unknown_type);
    }
}

OT_TEST(empty_input_is_truncated_in_both_directions) {
    Rec r;
    OT_CHECK_EQ(decode_inbound(std::span<const std::byte>{}, r), DecodeStatus::truncated);
    OT_CHECK_EQ(decode_outbound(std::span<const std::byte>{}, r), DecodeStatus::truncated);
    OT_CHECK_EQ(r.calls, 0);
}

OT_TEST(direction_matters_for_the_shared_type_byte_u) {
    // 'U' is a 47-byte Replace inbound and an 80-byte Replaced outbound.
    Rec r;
    OT_CHECK_EQ(dec_out(to_bytes(kReplace), r), DecodeStatus::truncated);
    OT_CHECK_EQ(dec_in(to_bytes(kReplaced), r), DecodeStatus::bad_length);
    OT_CHECK_EQ(dec_in(to_bytes(kAccepted), r), DecodeStatus::unknown_type);
    OT_CHECK_EQ(dec_out(to_bytes(kEnter), r), DecodeStatus::unknown_type);
    OT_CHECK_EQ(r.calls, 0);
}

// ---- encoder buffer discipline ------------------------------------------------

namespace {

template <class M>
void check_encoder_bounds(const M& m, std::size_t length) {
    // Too small: nothing is written, whatever the shortfall.
    for (std::size_t n = 0; n < length; ++n) {
        Bytes buf(n, std::byte{0xEE});
        OT_CHECK_EQ(encode(m, std::span<std::byte>(buf)), std::size_t{0});
        for (std::byte b : buf) OT_CHECK(b == std::byte{0xEE});
    }
    // Exact size works, and a larger buffer is only touched up to `length`.
    Bytes exact(length);
    OT_CHECK_EQ(encode(m, std::span<std::byte>(exact)), length);
    Bytes big(length + 5, std::byte{0xEE});
    OT_CHECK_EQ(encode(m, std::span<std::byte>(big)), length);
    for (std::size_t i = length; i < big.size(); ++i) OT_CHECK(big[i] == std::byte{0xEE});
    OT_CHECK_EQ(first_diff(prefix(big, length), exact), kSame);
}

}  // namespace

OT_TEST(encoders_never_write_past_or_short_of_the_message) {
    EnterOrder e;
    e.price = 5;
    e.shares = 5;
    ReplaceOrder u;
    u.price = 5;
    u.shares = 5;
    Accepted a;
    Replaced p;
    check_encoder_bounds(e, 49);
    check_encoder_bounds(u, 47);
    check_encoder_bounds(CancelOrder{}, 19);
    check_encoder_bounds(a, 66);
    check_encoder_bounds(p, 80);
    check_encoder_bounds(Canceled{}, 28);
    check_encoder_bounds(Executed{}, 40);
    check_encoder_bounds(Rejected{}, 24);
}

OT_TEST(encoders_refuse_a_price_the_wire_cannot_hold) {
    for (Price bad : {Price{-1}, Price{4'294'967'296LL}, Price{INT64_MIN}, Price{INT64_MAX}}) {
        Bytes buf(kMaxMessageLength, std::byte{0xEE});
        const std::span<std::byte> s(buf);
        EnterOrder e;
        e.price = bad;
        OT_CHECK_EQ(encode(e, s), std::size_t{0});
        ReplaceOrder u;
        u.price = bad;
        OT_CHECK_EQ(encode(u, s), std::size_t{0});
        Accepted a;
        a.price = bad;
        OT_CHECK_EQ(encode(a, s), std::size_t{0});
        Replaced p;
        p.a.price = bad;
        OT_CHECK_EQ(encode(p, s), std::size_t{0});
        Executed x;
        x.price = bad;
        OT_CHECK_EQ(encode(x, s), std::size_t{0});
        for (std::byte b : buf) OT_CHECK(b == std::byte{0xEE});
    }
    for (Price good : {Price{0}, Price{4'294'967'295LL}}) {
        EnterOrder e;
        e.price = good;
        OT_CHECK_EQ(encoded(e).size(), std::size_t{49});
    }
}

// ---- randomized differential against a naive offset-table writer -------------

namespace {

// Writes fields at offsets typed out again from the spec tables, sharing no code
// with the library's encoder.
struct Naive {
    Bytes b;
    Naive(std::size_t size, char type) : b(size) { b[0] = static_cast<std::byte>(type); }
    Naive& u(std::size_t off, std::size_t len, std::uint64_t v) {
        for (std::size_t i = 0; i < len; ++i) b[off + i] = static_cast<std::byte>(v >> (8 * (len - 1 - i)));
        return *this;
    }
    Naive& raw(std::size_t off, const char* p, std::size_t len) {
        for (std::size_t i = 0; i < len; ++i) b[off + i] = static_cast<std::byte>(static_cast<unsigned char>(p[i]));
        return *this;
    }
    Naive& c(std::size_t off, char ch) { return raw(off, &ch, 1); }
};

char side_byte(Side s, bool short_sell) { return s == Side::buy ? 'B' : (short_sell ? 'T' : 'S'); }

Bytes naive(const EnterOrder& m) {
    Naive n(49, 'O');
    n.raw(1, m.token.raw().data(), 14).c(15, side_byte(m.side, m.short_sell)).u(16, 4, m.shares);
    n.raw(20, m.stock.raw().data(), 8).u(28, 4, static_cast<std::uint64_t>(m.price)).u(32, 4, m.time_in_force);
    n.raw(36, m.firm, 4).c(40, m.display).c(41, m.capacity).c(42, m.intermarket_sweep);
    n.u(43, 4, m.min_qty).c(47, m.cross_type).c(48, m.customer_type);
    return n.b;
}
Bytes naive(const ReplaceOrder& m) {
    Naive n(47, 'U');
    n.raw(1, m.existing.raw().data(), 14).raw(15, m.replacement.raw().data(), 14).u(29, 4, m.shares);
    n.u(33, 4, static_cast<std::uint64_t>(m.price)).u(37, 4, m.time_in_force).c(41, m.display);
    n.c(42, m.intermarket_sweep).u(43, 4, m.min_qty);
    return n.b;
}
Bytes naive(const CancelOrder& m) {
    Naive n(19, 'X');
    n.raw(1, m.token.raw().data(), 14).u(15, 4, m.shares);
    return n.b;
}
Naive naive_accepted_head(char type, std::size_t size, const Accepted& m) {
    Naive n(size, type);
    n.u(1, 8, m.ts).raw(9, m.token.raw().data(), 14).c(23, side_byte(m.side, false)).u(24, 4, m.shares);
    n.raw(28, m.stock.raw().data(), 8).u(36, 4, static_cast<std::uint64_t>(m.price)).u(40, 4, m.time_in_force);
    n.raw(44, m.firm, 4).c(48, m.display).u(49, 8, m.ref).c(57, m.capacity).c(58, m.intermarket_sweep);
    n.u(59, 4, m.min_qty).c(63, m.cross_type).c(64, m.order_state);
    return n;
}
Bytes naive(const Accepted& m) {
    Naive n = naive_accepted_head('A', 66, m);
    n.c(65, m.bbo_weight);
    return n.b;
}
Bytes naive(const Replaced& m) {
    Naive n = naive_accepted_head('U', 80, m.a);
    n.raw(65, m.previous.raw().data(), 14).c(79, m.a.bbo_weight);
    return n.b;
}
Bytes naive(const Canceled& m) {
    Naive n(28, 'C');
    n.u(1, 8, m.ts).raw(9, m.token.raw().data(), 14).u(23, 4, m.decrement).c(27, m.reason);
    return n.b;
}
Bytes naive(const Executed& m) {
    Naive n(40, 'E');
    n.u(1, 8, m.ts).raw(9, m.token.raw().data(), 14).u(23, 4, m.shares);
    n.u(27, 4, static_cast<std::uint64_t>(m.price)).c(31, m.liquidity).u(32, 8, m.match);
    return n.b;
}
Bytes naive(const Rejected& m) {
    Naive n(24, 'J');
    n.u(1, 8, m.ts).raw(9, m.token.raw().data(), 14).c(23, m.reason);
    return n.b;
}

// Random values with the edges over-represented.
std::uint32_t r32(Rng& g) {
    switch (g.bounded(8)) {
        case 0: return 0;
        case 1: return 0xFFFFFFFFu;
        case 2: return 0x7FFFFFFFu;
        default: return static_cast<std::uint32_t>(g.next());
    }
}
std::uint64_t r64(Rng& g) { return g.chance(1, 8) ? ~std::uint64_t{0} : g.next(); }
Qty order_shares(Rng& g) { return g.chance(1, 6) ? (g.chance(1, 2) ? 1 : 999'999) : static_cast<Qty>(g.range(1, 999'999)); }
Price wire_price(Rng& g) { return static_cast<Price>(r32(g)); }
char rchar(Rng& g) { return static_cast<char>(g.bounded(256)); }
Token rtoken(Rng& g) {
    char raw[kTokenSize];
    for (char& c : raw) c = rchar(g);
    return Token::from_wire(raw);
}
Symbol rsymbol(Rng& g) {
    char raw[Symbol::kSize];
    for (char& c : raw) c = rchar(g);
    return Symbol::from_wire(raw);
}
void rfirm(Rng& g, char (&f)[4]) {
    for (char& c : f) c = rchar(g);
}

EnterOrder gen_enter(Rng& g) {
    EnterOrder m;
    m.token = rtoken(g);
    m.side = g.chance(1, 2) ? Side::buy : Side::sell;
    m.short_sell = m.side == Side::sell && g.chance(1, 3);
    m.shares = order_shares(g);
    m.stock = rsymbol(g);
    m.price = wire_price(g);
    m.time_in_force = r32(g);
    rfirm(g, m.firm);
    m.display = rchar(g);
    m.capacity = rchar(g);
    m.intermarket_sweep = rchar(g);
    m.min_qty = r32(g);
    m.cross_type = rchar(g);
    m.customer_type = rchar(g);
    return m;
}
ReplaceOrder gen_replace(Rng& g) {
    ReplaceOrder m;
    m.existing = rtoken(g);
    m.replacement = rtoken(g);
    m.shares = order_shares(g);
    m.price = wire_price(g);
    m.time_in_force = r32(g);
    m.display = rchar(g);
    m.intermarket_sweep = rchar(g);
    m.min_qty = r32(g);
    return m;
}
CancelOrder gen_cancel(Rng& g) { return CancelOrder{rtoken(g), r32(g)}; }
Accepted gen_accepted(Rng& g) {
    Accepted m;
    m.ts = r64(g);
    m.token = rtoken(g);
    m.side = g.chance(1, 2) ? Side::buy : Side::sell;
    m.shares = r32(g);
    m.stock = rsymbol(g);
    m.price = wire_price(g);
    m.time_in_force = r32(g);
    rfirm(g, m.firm);
    m.display = rchar(g);
    m.ref = r64(g);
    m.capacity = rchar(g);
    m.intermarket_sweep = rchar(g);
    m.min_qty = r32(g);
    m.cross_type = rchar(g);
    m.order_state = g.chance(1, 2) ? 'L' : 'D';
    m.bbo_weight = rchar(g);
    return m;
}
Replaced gen_replaced(Rng& g) { return Replaced{gen_accepted(g), rtoken(g)}; }
Canceled gen_canceled(Rng& g) { return Canceled{r64(g), rtoken(g), r32(g), rchar(g)}; }
Executed gen_executed(Rng& g) { return Executed{r64(g), rtoken(g), r32(g), wire_price(g), rchar(g), r64(g)}; }
Rejected gen_rejected(Rng& g) { return Rejected{r64(g), rtoken(g), rchar(g)}; }

template <class M, class Gen>
void differential(std::uint64_t seed, int rounds, bool inbound, Gen gen, M Rec::*slot) {
    Rng g(seed);
    for (int i = 0; i < rounds; ++i) {
        const M m = gen(g);
        const Bytes want = naive(m);
        OT_CHECK_EQ(first_diff(encoded(m), want), kSame);
        Rec r;
        const DecodeStatus st = inbound ? dec_in(want, r) : dec_out(want, r);
        OT_CHECK_EQ(st, DecodeStatus::ok);
        OT_CHECK(r.*slot == m);
    }
}

// What the wire rules say about a message with these bytes, decided from the raw
// bytes alone.
DecodeStatus predict(const Bytes& w) {
    const auto at = [&w](std::size_t i) { return static_cast<char>(w[i]); };
    const auto be32 = [&w](std::size_t i) {
        return (std::uint32_t{std::to_integer<std::uint8_t>(w[i])} << 24) |
               (std::uint32_t{std::to_integer<std::uint8_t>(w[i + 1])} << 16) |
               (std::uint32_t{std::to_integer<std::uint8_t>(w[i + 2])} << 8) |
               std::uint32_t{std::to_integer<std::uint8_t>(w[i + 3])};
    };
    const auto legal_side = [](char c) { return c == 'B' || c == 'S' || c == 'T' || c == 'E'; };
    const auto legal_shares = [](std::uint32_t s) { return s >= 1 && s <= 999'999; };
    const auto legal_state = [](char c) { return c == 'L' || c == 'D'; };
    const bool bad = [&] {
        switch (at(0)) {
            case 'O': return !legal_side(at(15)) || !legal_shares(be32(16));
            case 'U': return w.size() == 47 ? !legal_shares(be32(29)) : (!legal_side(at(23)) || !legal_state(at(64)));
            case 'A': return !legal_side(at(23)) || !legal_state(at(64));
            default: return false;
        }
    }();
    return bad ? DecodeStatus::bad_field : DecodeStatus::ok;
}

}  // namespace

OT_TEST(random_messages_encode_to_the_naive_layout_and_decode_back) {
    differential<EnterOrder>(101, 4000, true, gen_enter, &Rec::enter);
    differential<ReplaceOrder>(102, 4000, true, gen_replace, &Rec::replace);
    differential<CancelOrder>(103, 4000, true, gen_cancel, &Rec::cancel);
    differential<Accepted>(104, 4000, false, gen_accepted, &Rec::accepted);
    differential<Replaced>(105, 4000, false, gen_replaced, &Rec::replaced);
    differential<Canceled>(106, 4000, false, gen_canceled, &Rec::canceled);
    differential<Executed>(107, 4000, false, gen_executed, &Rec::executed);
    differential<Rejected>(108, 4000, false, gen_rejected, &Rec::rejected);
}

OT_TEST(random_byte_corruption_yields_exactly_the_predicted_status) {
    Rng g(909);
    int ok = 0, bad = 0;
    for (int i = 0; i < 40000; ++i) {
        Bytes w;
        bool inbound = false;
        switch (g.bounded(8)) {
            case 0: w = naive(gen_enter(g)); inbound = true; break;
            case 1: w = naive(gen_replace(g)); inbound = true; break;
            case 2: w = naive(gen_cancel(g)); inbound = true; break;
            case 3: w = naive(gen_accepted(g)); break;
            case 4: w = naive(gen_replaced(g)); break;
            case 5: w = naive(gen_canceled(g)); break;
            case 6: w = naive(gen_executed(g)); break;
            default: w = naive(gen_rejected(g)); break;
        }
        // Mutate anything except the type byte, preferring the bytes that matter.
        const int flips = 1 + static_cast<int>(g.bounded(3));
        for (int k = 0; k < flips; ++k) {
            const std::size_t pos = 1 + g.bounded(w.size() - 1);
            static constexpr char kInteresting[] = {'B', 'S', 'T', 'E', 'L', 'D', 'X', 0, ' '};
            w[pos] = g.chance(1, 2) ? static_cast<std::byte>(kInteresting[g.bounded(sizeof kInteresting)])
                                    : static_cast<std::byte>(g.bounded(256));
        }
        Rec r;
        const DecodeStatus got = inbound ? dec_in(w, r) : dec_out(w, r);
        const DecodeStatus want = predict(w);
        OT_CHECK_EQ(got, want);
        OT_CHECK_EQ(r.calls, want == DecodeStatus::ok ? 1 : 0);
        (want == DecodeStatus::ok ? ok : bad)++;
    }
    OT_CHECK(ok > 1000);
    OT_CHECK(bad > 1000);
}

OT_TEST_MAIN()

// Known-answer, boundary and differential tests for the ITCH 5.0 message codec.
//
// Every wire vector below is written byte by byte from the field tables of the
// TotalView-ITCH 5.0 specification (offsets are noted next to each field). None
// of them is produced by the encoder under test, so a mistake that the encoder
// and decoder share would still fail here.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "check.hpp"
#include "optitrade/core/rng.hpp"
#include "optitrade/itch/decoder.hpp"
#include "optitrade/itch/encoder.hpp"

namespace itch = optitrade::itch;
using optitrade::DecodeStatus;
using optitrade::Nanos;
using optitrade::Price;
using optitrade::Qty;
using optitrade::Side;
using optitrade::Symbol;

namespace {

template <class... T>
constexpr std::array<std::byte, sizeof...(T)> bytes(T... v) {
    return {static_cast<std::byte>(v)...};
}

// Collects the last message of type M and counts every other message.
template <class M>
struct Recorder {
    M last{};
    int matching = 0;
    int other = 0;
    void on(const M& m) noexcept {
        last = m;
        ++matching;
    }
    template <class X>
    void on(const X&) noexcept {
        ++other;
    }
};

struct Counter {
    int calls = 0;
    template <class X>
    void on(const X&) noexcept {
        ++calls;
    }
};

// ---- Hand-built wire messages ---------------------------------------------

// S: System Event, event code 'O' (start of messages).
constexpr auto kWireS = bytes(
    'S',                                      // @0  type
    0x00, 0x00,                               // @1  locate 0
    0x00, 0x07,                               // @3  tracking 7
    0x00, 0x00, 0x1A, 0x2B, 0x3C, 0x4D,       // @5  timestamp 0x1A2B3C4D
    'O');                                     // @11 event code

// R: Stock Directory for SQQQ (a 3x inverse ETP), every one-byte field distinct
// from its neighbours so that an off-by-one shows up.
constexpr auto kWireR = bytes(
    'R',                                      // @0
    0x01, 0x02,                               // @1  locate 258
    0x03, 0x04,                               // @3  tracking 772
    0x00, 0x00, 0x0B, 0xEE, 0xF0, 0x01,       // @5  timestamp 0x0BEEF001
    'S', 'Q', 'Q', 'Q', ' ', ' ', ' ', ' ',   // @11 stock "SQQQ    "
    'Q',                                      // @19 market category
    'D',                                      // @20 financial status
    0x00, 0x00, 0x00, 0x64,                   // @21 round lot size 100
    'Y',                                      // @25 round lots only
    'C',                                      // @26 issue classification
    'Z', ' ',                                 // @27 issue sub-type
    'P',                                      // @29 authenticity
    'N',                                      // @30 short sale threshold
    ' ',                                      // @31 IPO flag
    '1',                                      // @32 LULD reference price tier
    'Y',                                      // @33 ETP flag
    0x00, 0x00, 0x00, 0x03,                   // @34 ETP leverage 3
    'Y');                                     // @38 inverse indicator

// A: Add Order, buy 500 AAPL at 123.4500.
constexpr auto kWireA = bytes(
    'A',                                      // @0
    0x12, 0x34,                               // @1  locate 0x1234
    0xAB, 0xCD,                               // @3  tracking 0xABCD
    0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,       // @5  timestamp 0x0A0B0C0D0E0F
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,  // @11 reference 0x0011223344556677
    'B',                                      // @19 side
    0x00, 0x00, 0x01, 0xF4,                   // @20 shares 500
    'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ',   // @24 stock "AAPL    "
    0x00, 0x12, 0xD6, 0x44);                  // @32 price 1234500

// F: Add Order with attribution, sell 999999 MSFT at 20.0000, MPID GSCO.
constexpr auto kWireF = bytes(
    'F',                                      // @0
    0x00, 0x02,                               // @1  locate 2
    0x00, 0x09,                               // @3  tracking 9
    0x00, 0x00, 0x00, 0x00, 0x03, 0xE8,       // @5  timestamp 1000
    0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00, 0x00, 0x2A,  // @11 reference
    'S',                                      // @19 side
    0x00, 0x0F, 0x42, 0x3F,                   // @20 shares 999999
    'M', 'S', 'F', 'T', ' ', ' ', ' ', ' ',   // @24 stock "MSFT    "
    0x00, 0x03, 0x0D, 0x40,                   // @32 price 200000
    'G', 'S', 'C', 'O');                      // @36 attribution

// E: Order Executed.
constexpr auto kWireE = bytes(
    'E',                                      // @0
    0x00, 0x05,                               // @1  locate 5
    0x00, 0x06,                               // @3  tracking 6
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00,       // @5  timestamp 65536
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // @11 reference
    0x00, 0x00, 0x00, 0x64,                   // @19 executed shares 100
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88); // @23 match number

// C: Order Executed With Price, printable.
constexpr auto kWireC = bytes(
    'C',                                      // @0
    0x00, 0x0A,                               // @1  locate 10
    0x00, 0x0B,                               // @3  tracking 11
    0x00, 0x00, 0x00, 0x00, 0x00, 0x2A,       // @5  timestamp 42
    0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8,  // @11 reference
    0x00, 0x00, 0x00, 0x32,                   // @19 executed shares 50
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,  // @23 match number
    'Y',                                      // @31 printable
    0x00, 0x0F, 0x42, 0x40);                  // @32 price 1000000

// X: Order Cancel (partial).
constexpr auto kWireX = bytes(
    'X',                                      // @0
    0x00, 0x03,                               // @1  locate 3
    0x00, 0x04,                               // @3  tracking 4
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00,       // @5  timestamp 4096
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x2C,  // @11 reference 300
    0x00, 0x00, 0x00, 0x19);                  // @19 cancelled shares 25

// D: Order Delete.
constexpr auto kWireD = bytes(
    'D',                                      // @0
    0x00, 0x09,                               // @1  locate 9
    0x00, 0x08,                               // @3  tracking 8
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01,       // @5  timestamp 1
    0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10);  // @11 reference

// U: Order Replace.
constexpr auto kWireU = bytes(
    'U',                                      // @0
    0x00, 0x07,                               // @1  locate 7
    0x00, 0x00,                               // @3  tracking 0
    0x00, 0x00, 0x00, 0x00, 0x00, 0x63,       // @5  timestamp 99
    0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22,  // @11 original reference
    0x33, 0x33, 0x33, 0x33, 0x44, 0x44, 0x44, 0x44,  // @19 new reference
    0x00, 0x00, 0x02, 0x58,                   // @27 shares 600
    0x00, 0x0F, 0x42, 0x41);                  // @31 price 1000001

// P: Trade (non-cross). The reference field is zero on today's feed.
constexpr auto kWireP = bytes(
    'P',                                      // @0
    0x00, 0x0C,                               // @1  locate 12
    0x00, 0x0D,                               // @3  tracking 13
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00,       // @5  timestamp 256
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // @11 reference (zero)
    'B',                                      // @19 side
    0x00, 0x00, 0x03, 0xE8,                   // @20 shares 1000
    'T', 'S', 'L', 'A', ' ', ' ', ' ', ' ',   // @24 stock "TSLA    "
    0x00, 0x0A, 0xBC, 0xDE,                   // @32 price 703710
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02);  // @36 match number

itch::Header hdr(optitrade::Locate locate, std::uint16_t tracking, Nanos ts) { return {locate, tracking, ts}; }

itch::SystemEvent expect_s() { return {hdr(0, 7, 0x1A2B3C4D), 'O'}; }

itch::StockDirectory expect_r() {
    itch::StockDirectory e{};
    e.h = hdr(258, 772, 0x0BEEF001);
    e.symbol = Symbol("SQQQ");
    e.market_category = 'Q';
    e.financial_status = 'D';
    e.round_lot_size = 100;
    e.round_lots_only = 'Y';
    e.issue_classification = 'C';
    e.issue_subtype[0] = 'Z';
    e.issue_subtype[1] = ' ';
    e.authenticity = 'P';
    e.short_sale_threshold = 'N';
    e.ipo_flag = ' ';
    e.luld_tier = '1';
    e.etp_flag = 'Y';
    e.etp_leverage = 3;
    e.inverse = 'Y';
    return e;
}

itch::AddOrder expect_a() {
    itch::AddOrder e{};
    e.h = hdr(0x1234, 0xABCD, 0x0A0B0C0D0E0FULL);
    e.ref = 0x0011223344556677ULL;
    e.side = Side::buy;
    e.shares = 500;
    e.symbol = Symbol("AAPL");
    e.price = 1'234'500;
    return e;
}

itch::AddOrder expect_f() {
    itch::AddOrder e{};
    e.h = hdr(2, 9, 1000);
    e.ref = 0xDEADBEEF0000002AULL;
    e.side = Side::sell;
    e.shares = 999'999;
    e.symbol = Symbol("MSFT");
    e.price = 200'000;
    e.has_attribution = true;
    std::memcpy(e.mpid, "GSCO", 4);
    return e;
}

itch::OrderExecuted expect_e() {
    return {hdr(5, 6, 65536), 0x0102030405060708ULL, 100, 0x1122334455667788ULL};
}

itch::OrderExecutedPrice expect_c() {
    return {hdr(10, 11, 42), 0xA1A2A3A4A5A6A7A8ULL, 50, 0x0F0E0D0C0B0A0908ULL, true, 1'000'000};
}

itch::OrderCancel expect_x() { return {hdr(3, 4, 4096), 300, 25}; }

itch::OrderDelete expect_d() { return {hdr(9, 8, 1), 0xFEDCBA9876543210ULL}; }

itch::OrderReplace expect_u() {
    return {hdr(7, 0, 99), 0x1111111122222222ULL, 0x3333333344444444ULL, 600, 1'000'001};
}

itch::Trade expect_p() {
    itch::Trade e{};
    e.h = hdr(12, 13, 256);
    e.ref = 0;
    e.side = Side::buy;
    e.shares = 1000;
    e.symbol = Symbol("TSLA");
    e.price = 703'710;
    e.match = 0x0000000100000002ULL;
    return e;
}

std::array<std::span<const std::byte>, 10> all_wires() {
    return {std::span<const std::byte>(kWireS), std::span<const std::byte>(kWireR),
            std::span<const std::byte>(kWireA), std::span<const std::byte>(kWireF),
            std::span<const std::byte>(kWireE), std::span<const std::byte>(kWireC),
            std::span<const std::byte>(kWireX), std::span<const std::byte>(kWireD),
            std::span<const std::byte>(kWireU), std::span<const std::byte>(kWireP)};
}

// ---- Shared checks ----------------------------------------------------------

// Decode `wire`, compare with `expected`, then encode `expected` and compare to
// the same bytes. Also proves the encoder writes nothing past the message.
template <class M>
void check_known_answer(std::span<const std::byte> wire, const M& expected) {
    Recorder<M> rec;
    OT_CHECK_EQ(itch::decode(wire, rec), DecodeStatus::ok);
    OT_CHECK_EQ(rec.matching, 1);
    OT_CHECK_EQ(rec.other, 0);
    OT_CHECK(rec.last == expected);

    const std::size_t n = wire.size();
    std::vector<std::byte> out(n + 6, std::byte{0xEE});
    OT_CHECK_EQ(itch::encode(expected, out), n);
    OT_CHECK(std::equal(wire.begin(), wire.end(), out.begin()));
    for (std::size_t i = n; i < out.size(); ++i) OT_CHECK(out[i] == std::byte{0xEE});

    std::vector<std::byte> framed(n + 2 + 3, std::byte{0xEE});
    OT_CHECK_EQ(itch::encode_framed(expected, framed), n + 2);
    OT_CHECK(framed[0] == std::byte{0} && framed[1] == static_cast<std::byte>(n));
    OT_CHECK(std::equal(wire.begin(), wire.end(), framed.begin() + 2));
    for (std::size_t i = n + 2; i < framed.size(); ++i) OT_CHECK(framed[i] == std::byte{0xEE});
}

// Encoding into every too-small buffer fails and leaves the buffer alone; an
// exactly-sized heap buffer lets AddressSanitizer catch any overrun.
template <class M>
void check_encode_bounds(const M& msg, std::span<const std::byte> wire) {
    const std::size_t n = wire.size();
    for (std::size_t cap = 0; cap < n; ++cap) {
        std::vector<std::byte> buf(cap, std::byte{0xEE});
        OT_CHECK_EQ(itch::encode(msg, buf), std::size_t{0});
        for (std::byte b : buf) OT_CHECK(b == std::byte{0xEE});
    }
    std::vector<std::byte> exact(n, std::byte{0xEE});
    OT_CHECK_EQ(itch::encode(msg, exact), n);
    OT_CHECK(std::equal(wire.begin(), wire.end(), exact.begin()));

    for (std::size_t cap = 0; cap < n + 2; ++cap) {
        std::vector<std::byte> buf(cap, std::byte{0xEE});
        OT_CHECK_EQ(itch::encode_framed(msg, buf), std::size_t{0});
        for (std::byte b : buf) OT_CHECK(b == std::byte{0xEE});
    }
    std::vector<std::byte> framed(n + 2, std::byte{0xEE});
    OT_CHECK_EQ(itch::encode_framed(msg, framed), n + 2);
}

// Big-endian read of `len` bytes: the naive reference the differential tests use.
std::uint64_t rd(std::span<const std::byte> b, std::size_t off, std::size_t len) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < len; ++i) v = (v << 8) | std::to_integer<std::uint64_t>(b[off + i]);
    return v;
}

char ch_at(std::span<const std::byte> b, std::size_t off) { return static_cast<char>(rd(b, off, 1)); }

bool symbol_matches(const Symbol& s, std::span<const std::byte> b, std::size_t off) {
    return std::memcmp(s.raw().data(), b.data() + off, 8) == 0;
}

}  // namespace

// ---- Known-answer vectors ---------------------------------------------------

OT_TEST(known_answer_system_event) { check_known_answer(kWireS, expect_s()); }
OT_TEST(known_answer_stock_directory) { check_known_answer(kWireR, expect_r()); }
OT_TEST(known_answer_add_order) { check_known_answer(kWireA, expect_a()); }
OT_TEST(known_answer_add_order_mpid) { check_known_answer(kWireF, expect_f()); }
OT_TEST(known_answer_order_executed) { check_known_answer(kWireE, expect_e()); }
OT_TEST(known_answer_executed_with_price) { check_known_answer(kWireC, expect_c()); }
OT_TEST(known_answer_order_cancel) { check_known_answer(kWireX, expect_x()); }
OT_TEST(known_answer_order_delete) { check_known_answer(kWireD, expect_d()); }
OT_TEST(known_answer_order_replace) { check_known_answer(kWireU, expect_u()); }
OT_TEST(known_answer_trade) { check_known_answer(kWireP, expect_p()); }

OT_TEST(add_order_fields_spelled_out) {
    // The whole-struct comparison above is the strict check; this one documents
    // the values so a failure message points at the field.
    Recorder<itch::AddOrder> rec;
    OT_CHECK_EQ(itch::decode(kWireA, rec), DecodeStatus::ok);
    const itch::AddOrder& m = rec.last;
    OT_CHECK_EQ(std::size_t{m.h.locate}, std::size_t{4660});
    OT_CHECK_EQ(std::size_t{m.h.tracking}, std::size_t{43981});
    OT_CHECK_EQ(m.h.timestamp, std::uint64_t{11'042'563'100'175ULL});
    OT_CHECK_EQ(m.ref, std::uint64_t{4'822'678'189'205'111ULL});
    OT_CHECK(m.side == Side::buy);
    OT_CHECK_EQ(m.shares, Qty{500});
    OT_CHECK(m.symbol.view() == "AAPL");
    OT_CHECK(m.symbol.raw()[4] == ' ' && m.symbol.raw()[7] == ' ');
    OT_CHECK_EQ(m.price, Price{1'234'500});
    OT_CHECK(!m.has_attribution);
    OT_CHECK(m.mpid[0] == 0 && m.mpid[1] == 0 && m.mpid[2] == 0 && m.mpid[3] == 0);
}

OT_TEST(add_order_mpid_fields_spelled_out) {
    Recorder<itch::AddOrder> rec;
    OT_CHECK_EQ(itch::decode(kWireF, rec), DecodeStatus::ok);
    const itch::AddOrder& m = rec.last;
    OT_CHECK(m.has_attribution);
    OT_CHECK(m.side == Side::sell);
    OT_CHECK_EQ(m.shares, Qty{999'999});
    OT_CHECK_EQ(m.price, Price{200'000});
    OT_CHECK(m.symbol.view() == "MSFT");
    OT_CHECK(std::memcmp(m.mpid, "GSCO", 4) == 0);
}

OT_TEST(sell_side_add_and_buy_side_trade_flip_only_the_side) {
    auto a = kWireA;
    a[19] = std::byte{'S'};
    Recorder<itch::AddOrder> ra;
    OT_CHECK_EQ(itch::decode(a, ra), DecodeStatus::ok);
    itch::AddOrder ea = expect_a();
    ea.side = Side::sell;
    OT_CHECK(ra.last == ea);

    auto p = kWireP;
    p[19] = std::byte{'S'};
    Recorder<itch::Trade> rp;
    OT_CHECK_EQ(itch::decode(p, rp), DecodeStatus::ok);
    itch::Trade ep = expect_p();
    ep.side = Side::sell;
    OT_CHECK(rp.last == ep);
}

OT_TEST(trade_keeps_a_nonzero_reference_from_older_captures) {
    auto p = kWireP;
    for (std::size_t i = 0; i < 8; ++i) p[11 + i] = static_cast<std::byte>(0x90 + i);
    Recorder<itch::Trade> rec;
    OT_CHECK_EQ(itch::decode(p, rec), DecodeStatus::ok);
    OT_CHECK_EQ(rec.last.ref, std::uint64_t{0x9091929394959697ULL});
}

OT_TEST(executed_with_price_printable_flag) {
    auto c = kWireC;
    Recorder<itch::OrderExecutedPrice> rec;
    OT_CHECK_EQ(itch::decode(c, rec), DecodeStatus::ok);
    OT_CHECK(rec.last.printable);

    c[31] = std::byte{'N'};
    OT_CHECK_EQ(itch::decode(c, rec), DecodeStatus::ok);
    OT_CHECK(!rec.last.printable);

    // Only 'Y' counts as printable: a corrupt flag must not add volume.
    const int corrupt_flags[] = {0, 'y', 'Z', ' ', 0xFF};
    for (int v : corrupt_flags) {
        c[31] = static_cast<std::byte>(v);
        OT_CHECK_EQ(itch::decode(c, rec), DecodeStatus::ok);
        OT_CHECK(!rec.last.printable);
    }
}

OT_TEST(attribution_decides_message_type_on_encode) {
    itch::AddOrder plain = expect_a();
    std::vector<std::byte> out(64, std::byte{0});
    OT_CHECK_EQ(itch::encode(plain, out), std::size_t{36});
    OT_CHECK(out[0] == std::byte{'A'});

    // mpid bytes are ignored when has_attribution is false.
    std::memcpy(plain.mpid, "ZZZZ", 4);
    std::fill(out.begin(), out.end(), std::byte{0});
    OT_CHECK_EQ(itch::encode(plain, out), std::size_t{36});
    OT_CHECK(out[0] == std::byte{'A'});
    OT_CHECK(out[36] == std::byte{0});

    itch::AddOrder attributed = expect_f();
    OT_CHECK_EQ(itch::encode(attributed, out), std::size_t{40});
    OT_CHECK(out[0] == std::byte{'F'});
    OT_CHECK(std::memcmp(out.data() + 36, "GSCO", 4) == 0);
}

// ---- Numeric extremes -------------------------------------------------------

OT_TEST(all_ones_add_order_has_no_sign_or_width_surprises) {
    constexpr auto wire = bytes(
        'A',
        0xFF, 0xFF,                                       // locate
        0xFF, 0xFF,                                       // tracking
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,               // timestamp (48 bits)
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   // reference
        'S',
        0xFF, 0xFF, 0xFF, 0xFF,                           // shares
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',           // a symbol with no padding
        0xFF, 0xFF, 0xFF, 0xFF);                          // price
    itch::AddOrder e{};
    e.h = hdr(0xFFFF, 0xFFFF, 0xFFFF'FFFF'FFFFULL);
    e.ref = 0xFFFF'FFFF'FFFF'FFFFULL;
    e.side = Side::sell;
    e.shares = 0xFFFF'FFFFu;
    e.symbol = Symbol("ABCDEFGH");
    e.price = 4'294'967'295LL;
    check_known_answer(wire, e);
    OT_CHECK(e.price > 0);
}

OT_TEST(timestamp_is_48_bits_wide) {
    // Only the high bit of the 48-bit field set, then only the low bit.
    auto s = kWireS;
    Recorder<itch::SystemEvent> rec;
    for (std::size_t i = 0; i < 6; ++i) s[5 + i] = std::byte{0};
    s[5] = std::byte{0x80};
    OT_CHECK_EQ(itch::decode(s, rec), DecodeStatus::ok);
    OT_CHECK_EQ(rec.last.h.timestamp, std::uint64_t{0x8000'0000'0000ULL});
    s[5] = std::byte{0};
    s[10] = std::byte{0x01};
    OT_CHECK_EQ(itch::decode(s, rec), DecodeStatus::ok);
    OT_CHECK_EQ(rec.last.h.timestamp, std::uint64_t{1});
    for (std::size_t i = 0; i < 6; ++i) s[5 + i] = std::byte{0xFF};
    OT_CHECK_EQ(itch::decode(s, rec), DecodeStatus::ok);
    OT_CHECK_EQ(rec.last.h.timestamp, std::uint64_t{0xFFFF'FFFF'FFFFULL});
}

OT_TEST(encoder_truncates_timestamp_to_48_bits) {
    itch::SystemEvent m = expect_s();
    m.h.timestamp = 0xABCD'0123'4567'89ABULL;  // bits 48..63 do not exist on the wire
    std::vector<std::byte> out(12);
    OT_CHECK_EQ(itch::encode(m, out), std::size_t{12});
    OT_CHECK(out[5] == std::byte{0x01} && out[6] == std::byte{0x23} && out[7] == std::byte{0x45} &&
             out[8] == std::byte{0x67} && out[9] == std::byte{0x89} && out[10] == std::byte{0xAB});
    Recorder<itch::SystemEvent> rec;
    OT_CHECK_EQ(itch::decode(out, rec), DecodeStatus::ok);
    OT_CHECK_EQ(rec.last.h.timestamp, std::uint64_t{0x0123'4567'89ABULL});
}

OT_TEST(price_and_shares_at_u32_limits_round_trip) {
    for (Price px : {Price{0}, Price{1}, Price{2'000'000'000}, Price{4'294'967'295LL}}) {
        itch::OrderReplace m = expect_u();
        m.price = px;
        m.shares = 0xFFFF'FFFFu;
        std::vector<std::byte> out(35);
        OT_CHECK_EQ(itch::encode(m, out), std::size_t{35});
        OT_CHECK_EQ(rd(out, 31, 4), static_cast<std::uint64_t>(px));
        OT_CHECK_EQ(rd(out, 27, 4), std::uint64_t{0xFFFF'FFFFu});
        Recorder<itch::OrderReplace> rec;
        OT_CHECK_EQ(itch::decode(out, rec), DecodeStatus::ok);
        OT_CHECK(rec.last == m);
    }
}

OT_TEST(encoder_refuses_prices_the_wire_cannot_carry) {
    const Price bad[] = {-1, 4'294'967'296LL, INT64_MIN, INT64_MAX};
    for (Price px : bad) {
        std::vector<std::byte> out(64, std::byte{0xEE});
        itch::AddOrder a = expect_a();
        a.price = px;
        OT_CHECK_EQ(itch::encode(a, out), std::size_t{0});
        OT_CHECK_EQ(itch::encode_framed(a, out), std::size_t{0});
        itch::AddOrder f = expect_f();
        f.price = px;
        OT_CHECK_EQ(itch::encode(f, out), std::size_t{0});
        itch::OrderExecutedPrice c = expect_c();
        c.price = px;
        OT_CHECK_EQ(itch::encode(c, out), std::size_t{0});
        itch::OrderReplace u = expect_u();
        u.price = px;
        OT_CHECK_EQ(itch::encode(u, out), std::size_t{0});
        itch::Trade p = expect_p();
        p.price = px;
        OT_CHECK_EQ(itch::encode(p, out), std::size_t{0});
        for (std::byte b : out) OT_CHECK(b == std::byte{0xEE});  // nothing written on failure
    }
}

// ---- Error paths ------------------------------------------------------------

OT_TEST(message_length_table) {
    // Written out literally, not derived from the constants in messages.hpp.
    OT_CHECK_EQ(itch::message_length('S'), std::size_t{12});
    OT_CHECK_EQ(itch::message_length('R'), std::size_t{39});
    OT_CHECK_EQ(itch::message_length('A'), std::size_t{36});
    OT_CHECK_EQ(itch::message_length('F'), std::size_t{40});
    OT_CHECK_EQ(itch::message_length('E'), std::size_t{31});
    OT_CHECK_EQ(itch::message_length('C'), std::size_t{36});
    OT_CHECK_EQ(itch::message_length('X'), std::size_t{23});
    OT_CHECK_EQ(itch::message_length('D'), std::size_t{19});
    OT_CHECK_EQ(itch::message_length('U'), std::size_t{35});
    OT_CHECK_EQ(itch::message_length('P'), std::size_t{44});
    int supported = 0;
    for (int c = -128; c < 256; ++c) {
        if (itch::message_length(static_cast<char>(c)) != 0) ++supported;
    }
    OT_CHECK_EQ(supported, 10);
    static_assert(itch::message_length('A') == 36);
    static_assert(itch::message_length('?') == 0);
    static_assert(itch::kMaxMessageLength == 44);
}

OT_TEST(known_answer_wire_lengths_match_the_table) {
    const auto wires = all_wires();
    for (auto w : wires) OT_CHECK_EQ(itch::message_length(static_cast<char>(w[0])), w.size());
}

OT_TEST(truncation_at_every_prefix_length) {
    for (std::span<const std::byte> wire : all_wires()) {
        for (std::size_t len = 0; len < wire.size(); ++len) {
            Counter c;
            // Copy into an exactly-sized heap block so a read past `len` trips ASan.
            std::vector<std::byte> prefix(wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(len));
            OT_CHECK_EQ(itch::decode(prefix, c), DecodeStatus::truncated);
            OT_CHECK_EQ(c.calls, 0);
        }
    }
}

OT_TEST(over_long_buffers_are_bad_length) {
    for (std::span<const std::byte> wire : all_wires()) {
        for (std::size_t extra : {1u, 2u, 7u, 30u, 200u}) {
            for (std::byte fill : {std::byte{0}, std::byte{0xFF}}) {
                std::vector<std::byte> buf(wire.begin(), wire.end());
                buf.resize(wire.size() + extra, fill);
                Counter c;
                OT_CHECK_EQ(itch::decode(buf, c), DecodeStatus::bad_length);
                OT_CHECK_EQ(c.calls, 0);
            }
        }
    }
}

OT_TEST(unknown_types_are_reported_whatever_the_length) {
    // Valid ITCH types we do not model (H Y L V W K J h Q B I N O) and arbitrary bytes.
    for (int t = 0; t < 256; ++t) {
        const char type = static_cast<char>(t);
        if (itch::message_length(type) != 0) continue;
        for (std::size_t len : {1u, 2u, 12u, 25u, 36u, 50u, 64u}) {
            std::vector<std::byte> buf(len, std::byte{0x5A});
            buf[0] = static_cast<std::byte>(t);
            Counter c;
            OT_CHECK_EQ(itch::decode(buf, c), DecodeStatus::unknown_type);
            OT_CHECK_EQ(c.calls, 0);
        }
    }
}

OT_TEST(empty_input_is_truncated) {
    Counter c;
    OT_CHECK_EQ(itch::decode(std::span<const std::byte>{}, c), DecodeStatus::truncated);
    OT_CHECK_EQ(c.calls, 0);
}

OT_TEST(invalid_side_is_bad_field_for_add_order_and_trade) {
    const int bad_sides[] = {0, ' ', 'b', 's', 'X', 'T', 0xFF};
    const std::span<const std::byte> wires[] = {kWireA, kWireF, kWireP};
    for (int bad : bad_sides) {
        for (std::span<const std::byte> wire : wires) {
            std::vector<std::byte> buf(wire.begin(), wire.end());
            buf[19] = static_cast<std::byte>(bad);
            Counter c;
            OT_CHECK_EQ(itch::decode(buf, c), DecodeStatus::bad_field);
            OT_CHECK_EQ(c.calls, 0);
        }
    }
}

OT_TEST(zero_shares_is_bad_field_only_for_add_orders) {
    const std::span<const std::byte> add_orders[] = {kWireA, kWireF};
    for (std::span<const std::byte> wire : add_orders) {
        std::vector<std::byte> buf(wire.begin(), wire.end());
        for (std::size_t i = 20; i < 24; ++i) buf[i] = std::byte{0};
        Counter c;
        OT_CHECK_EQ(itch::decode(buf, c), DecodeStatus::bad_field);
        OT_CHECK_EQ(c.calls, 0);
    }
    // A trade, an execution, a cancel and a replace may carry zero shares: the
    // book layer decides what that means, the codec only polices the framing.
    struct Case { const std::byte* wire; std::size_t size; std::size_t off; };
    const Case cases[] = {{kWireP.data(), kWireP.size(), 20}, {kWireE.data(), kWireE.size(), 19},
                          {kWireC.data(), kWireC.size(), 19}, {kWireX.data(), kWireX.size(), 19},
                          {kWireU.data(), kWireU.size(), 27}};
    for (const Case& k : cases) {
        std::vector<std::byte> buf(k.wire, k.wire + k.size);
        for (std::size_t i = 0; i < 4; ++i) buf[k.off + i] = std::byte{0};
        Counter c;
        OT_CHECK_EQ(itch::decode(buf, c), DecodeStatus::ok);
        OT_CHECK_EQ(c.calls, 1);
    }
}

OT_TEST(length_is_checked_before_fields) {
    // A short A message with a bad side is truncated, not bad_field.
    std::vector<std::byte> buf(kWireA.begin(), kWireA.begin() + 20);
    buf[19] = std::byte{'X'};
    Counter c;
    OT_CHECK_EQ(itch::decode(buf, c), DecodeStatus::truncated);
}

// ---- Encoder bounds ---------------------------------------------------------

OT_TEST(encoder_respects_output_bounds_for_every_type) {
    check_encode_bounds(expect_s(), kWireS);
    check_encode_bounds(expect_r(), kWireR);
    check_encode_bounds(expect_a(), kWireA);
    check_encode_bounds(expect_f(), kWireF);
    check_encode_bounds(expect_e(), kWireE);
    check_encode_bounds(expect_c(), kWireC);
    check_encode_bounds(expect_x(), kWireX);
    check_encode_bounds(expect_d(), kWireD);
    check_encode_bounds(expect_u(), kWireU);
    check_encode_bounds(expect_p(), kWireP);
}

OT_TEST(framed_prefix_is_big_endian_length) {
    std::vector<std::byte> out(64);
    OT_CHECK_EQ(itch::encode_framed(expect_p(), out), std::size_t{46});
    OT_CHECK(out[0] == std::byte{0x00} && out[1] == std::byte{44});
    OT_CHECK_EQ(itch::encode_framed(expect_f(), out), std::size_t{42});
    OT_CHECK(out[0] == std::byte{0x00} && out[1] == std::byte{40});
}

// ---- Decoder robustness -----------------------------------------------------

OT_TEST(decode_works_at_every_alignment) {
    // Fields are assembled from byte loads; UBSan's alignment check would flag a
    // wide load through a misaligned pointer.
    for (std::span<const std::byte> wire : all_wires()) {
        for (std::size_t shift = 0; shift < 8; ++shift) {
            std::vector<std::byte> buf(shift + wire.size() + 1, std::byte{0});
            std::copy(wire.begin(), wire.end(), buf.begin() + static_cast<std::ptrdiff_t>(shift));
            Counter c;
            const std::span<const std::byte> view(buf.data() + shift, wire.size());
            OT_CHECK_EQ(itch::decode(view, c), DecodeStatus::ok);
            OT_CHECK_EQ(c.calls, 1);
        }
    }
}

OT_TEST(decode_accepts_common_containers) {
    std::vector<std::byte> v(kWireD.begin(), kWireD.end());
    std::array<std::byte, 19> a = kWireD;
    Counter c;
    OT_CHECK_EQ(itch::decode(v, c), DecodeStatus::ok);
    OT_CHECK_EQ(itch::decode(a, c), DecodeStatus::ok);
    OT_CHECK_EQ(itch::decode(std::span<std::byte>(v), c), DecodeStatus::ok);
    OT_CHECK_EQ(c.calls, 3);
}

namespace {
// A handler that opts into two message types and inherits the no-op for the rest.
struct AddsAndDeletes : itch::NullHandler {
    using NullHandler::on;
    int adds = 0;
    int deletes = 0;
    void on(const itch::AddOrder&) noexcept { ++adds; }
    void on(const itch::OrderDelete&) noexcept { ++deletes; }
};
}  // namespace

OT_TEST(null_handler_base_lets_users_opt_into_messages) {
    AddsAndDeletes h;
    for (std::span<const std::byte> wire : all_wires()) OT_CHECK_EQ(itch::decode(wire, h), DecodeStatus::ok);
    OT_CHECK_EQ(h.adds, 2);  // A and F
    OT_CHECK_EQ(h.deletes, 1);
    itch::NullHandler null;
    OT_CHECK_EQ(itch::decode(kWireA, null), DecodeStatus::ok);
}

OT_TEST(unattributed_add_order_has_zeroed_mpid) {
    // The decoder value-initialises the struct, so a field it does not fill (the
    // MPID of an 'A') reads as zero rather than stack garbage, even when the
    // wire bytes just past the message hold something else.
    std::vector<std::byte> buf(kWireA.begin(), kWireA.end());
    buf.insert(buf.end(), 4, std::byte{'Z'});
    Recorder<itch::AddOrder> rec;
    OT_CHECK_EQ(itch::decode(std::span<const std::byte>(buf.data(), kWireA.size()), rec), DecodeStatus::ok);
    OT_CHECK(rec.last.mpid[0] == 0 && rec.last.mpid[1] == 0 && rec.last.mpid[2] == 0 && rec.last.mpid[3] == 0);
    OT_CHECK(!rec.last.has_attribution);
}

// ---- Differential tests against a naive big-endian reader --------------------

namespace {

constexpr char kTypes[] = {'S', 'R', 'A', 'F', 'E', 'C', 'X', 'D', 'U', 'P'};

// Expected decode status for random bytes `b` of exact length, per the spec rules.
DecodeStatus reference_status(std::span<const std::byte> b) {
    const char t = ch_at(b, 0);
    if (t == 'A' || t == 'F' || t == 'P') {
        const char side = ch_at(b, 19);
        if (side != 'B' && side != 'S') return DecodeStatus::bad_field;
    }
    if ((t == 'A' || t == 'F') && rd(b, 20, 4) == 0) return DecodeStatus::bad_field;
    return DecodeStatus::ok;
}

void expect_header(const itch::Header& h, std::span<const std::byte> b) {
    OT_CHECK_EQ(std::uint64_t{h.locate}, rd(b, 1, 2));
    OT_CHECK_EQ(std::uint64_t{h.tracking}, rd(b, 3, 2));
    OT_CHECK_EQ(h.timestamp, rd(b, 5, 6));
}

struct FieldChecker {
    std::span<const std::byte> b;

    void on(const itch::SystemEvent& m) {
        expect_header(m.h, b);
        OT_CHECK_EQ(m.event_code, ch_at(b, 11));
    }
    void on(const itch::StockDirectory& m) {
        expect_header(m.h, b);
        OT_CHECK(symbol_matches(m.symbol, b, 11));
        OT_CHECK_EQ(m.market_category, ch_at(b, 19));
        OT_CHECK_EQ(m.financial_status, ch_at(b, 20));
        OT_CHECK_EQ(std::uint64_t{m.round_lot_size}, rd(b, 21, 4));
        OT_CHECK_EQ(m.round_lots_only, ch_at(b, 25));
        OT_CHECK_EQ(m.issue_classification, ch_at(b, 26));
        OT_CHECK_EQ(m.issue_subtype[0], ch_at(b, 27));
        OT_CHECK_EQ(m.issue_subtype[1], ch_at(b, 28));
        OT_CHECK_EQ(m.authenticity, ch_at(b, 29));
        OT_CHECK_EQ(m.short_sale_threshold, ch_at(b, 30));
        OT_CHECK_EQ(m.ipo_flag, ch_at(b, 31));
        OT_CHECK_EQ(m.luld_tier, ch_at(b, 32));
        OT_CHECK_EQ(m.etp_flag, ch_at(b, 33));
        OT_CHECK_EQ(std::uint64_t{m.etp_leverage}, rd(b, 34, 4));
        OT_CHECK_EQ(m.inverse, ch_at(b, 38));
    }
    void on(const itch::AddOrder& m) {
        expect_header(m.h, b);
        OT_CHECK_EQ(m.ref, rd(b, 11, 8));
        OT_CHECK(m.side == (ch_at(b, 19) == 'B' ? Side::buy : Side::sell));
        OT_CHECK_EQ(std::uint64_t{m.shares}, rd(b, 20, 4));
        OT_CHECK(symbol_matches(m.symbol, b, 24));
        OT_CHECK_EQ(static_cast<std::uint64_t>(m.price), rd(b, 32, 4));
        OT_CHECK_EQ(m.has_attribution, ch_at(b, 0) == 'F');
        if (m.has_attribution) {
            OT_CHECK(std::memcmp(m.mpid, b.data() + 36, 4) == 0);
        } else {
            OT_CHECK(m.mpid[0] == 0 && m.mpid[1] == 0 && m.mpid[2] == 0 && m.mpid[3] == 0);
        }
    }
    void on(const itch::OrderExecuted& m) {
        expect_header(m.h, b);
        OT_CHECK_EQ(m.ref, rd(b, 11, 8));
        OT_CHECK_EQ(std::uint64_t{m.shares}, rd(b, 19, 4));
        OT_CHECK_EQ(m.match, rd(b, 23, 8));
    }
    void on(const itch::OrderExecutedPrice& m) {
        expect_header(m.h, b);
        OT_CHECK_EQ(m.ref, rd(b, 11, 8));
        OT_CHECK_EQ(std::uint64_t{m.shares}, rd(b, 19, 4));
        OT_CHECK_EQ(m.match, rd(b, 23, 8));
        OT_CHECK_EQ(m.printable, ch_at(b, 31) == 'Y');
        OT_CHECK_EQ(static_cast<std::uint64_t>(m.price), rd(b, 32, 4));
    }
    void on(const itch::OrderCancel& m) {
        expect_header(m.h, b);
        OT_CHECK_EQ(m.ref, rd(b, 11, 8));
        OT_CHECK_EQ(std::uint64_t{m.shares}, rd(b, 19, 4));
    }
    void on(const itch::OrderDelete& m) {
        expect_header(m.h, b);
        OT_CHECK_EQ(m.ref, rd(b, 11, 8));
    }
    void on(const itch::OrderReplace& m) {
        expect_header(m.h, b);
        OT_CHECK_EQ(m.old_ref, rd(b, 11, 8));
        OT_CHECK_EQ(m.new_ref, rd(b, 19, 8));
        OT_CHECK_EQ(std::uint64_t{m.shares}, rd(b, 27, 4));
        OT_CHECK_EQ(static_cast<std::uint64_t>(m.price), rd(b, 31, 4));
    }
    void on(const itch::Trade& m) {
        expect_header(m.h, b);
        OT_CHECK_EQ(m.ref, rd(b, 11, 8));
        OT_CHECK(m.side == (ch_at(b, 19) == 'B' ? Side::buy : Side::sell));
        OT_CHECK_EQ(std::uint64_t{m.shares}, rd(b, 20, 4));
        OT_CHECK(symbol_matches(m.symbol, b, 24));
        OT_CHECK_EQ(static_cast<std::uint64_t>(m.price), rd(b, 32, 4));
        OT_CHECK_EQ(m.match, rd(b, 36, 8));
    }
};

// Counts deliveries around a FieldChecker so a missing callback fails the test.
struct CountingFieldChecker {
    FieldChecker inner;
    int calls = 0;
    template <class M>
    void on(const M& m) {
        ++calls;
        inner.on(m);
    }
};

}  // namespace

OT_TEST(random_messages_decode_like_a_naive_reader) {
    optitrade::Rng rng(0x1F2E3D4C5B6A7988ULL);
    std::size_t decoded = 0;
    std::size_t rejected = 0;
    for (int iter = 0; iter < 60'000; ++iter) {
        const char type = kTypes[rng.bounded(sizeof kTypes)];
        const std::size_t len = itch::message_length(type);
        std::vector<std::byte> b(len);
        for (auto& x : b) x = static_cast<std::byte>(rng.next());
        b[0] = static_cast<std::byte>(type);
        // Steer most inputs towards valid ones so both outcomes are exercised.
        if (type == 'A' || type == 'F' || type == 'P') {
            if (rng.chance(7, 8)) b[19] = static_cast<std::byte>(rng.chance(1, 2) ? 'B' : 'S');
        }
        if ((type == 'A' || type == 'F') && rng.chance(1, 8)) {
            for (std::size_t i = 20; i < 24; ++i) b[i] = std::byte{0};
        }
        if (type == 'C' && rng.chance(1, 2)) b[31] = static_cast<std::byte>(rng.chance(1, 2) ? 'Y' : 'N');

        CountingFieldChecker h{FieldChecker{b}};
        const DecodeStatus expected = reference_status(b);
        OT_CHECK_EQ(itch::decode(b, h), expected);
        if (expected == DecodeStatus::ok) {
            OT_CHECK_EQ(h.calls, 1);
            ++decoded;
        } else {
            OT_CHECK_EQ(h.calls, 0);
            ++rejected;
        }
    }
    OT_CHECK(decoded > 30'000);   // the property test is not vacuous...
    OT_CHECK(rejected > 1'000);   // ...and the error branches are hit
}

namespace {

// Random values biased towards the boundaries that matter.
std::uint64_t pick(optitrade::Rng& rng, std::uint64_t max) {
    switch (rng.bounded(6)) {
        case 0: return 0;
        case 1: return max;
        case 2: return 1;
        default: return rng.next() & max;
    }
}

itch::Header random_header(optitrade::Rng& rng) {
    return {static_cast<optitrade::Locate>(pick(rng, 0xFFFF)), static_cast<std::uint16_t>(pick(rng, 0xFFFF)),
            pick(rng, 0xFFFF'FFFF'FFFFULL)};
}

Symbol random_symbol(optitrade::Rng& rng) {
    std::array<char, 8> raw{};
    for (char& c : raw) c = static_cast<char>(rng.range(0x20, 0x7E));
    return Symbol::from_wire(raw.data());
}

}  // namespace

namespace {

// Encode `m`, check the bytes with the naive reader, decode them back, and
// confirm the encoder stayed inside the message.
template <class M>
void check_random_encode(const M& m, std::size_t want_len, char want_type, std::vector<std::byte>& out) {
    std::fill(out.begin(), out.end(), std::byte{0xEE});
    const std::size_t n = itch::encode(m, out);
    OT_CHECK_EQ(n, want_len);
    if (n != want_len) return;
    const std::span<const std::byte> b(out.data(), n);
    OT_CHECK_EQ(ch_at(b, 0), want_type);
    FieldChecker fc{b};
    fc.on(m);
    Recorder<M> rec;
    OT_CHECK_EQ(itch::decode(b, rec), DecodeStatus::ok);
    OT_CHECK(rec.last == m);
    for (std::size_t i = n; i < out.size(); ++i) OT_CHECK(out[i] == std::byte{0xEE});
}

char random_alpha(optitrade::Rng& rng) { return static_cast<char>(rng.range(0x20, 0x7E)); }

}  // namespace

OT_TEST(random_messages_encode_like_a_naive_writer_and_round_trip) {
    optitrade::Rng rng(0x0BADC0FFEE123456ULL);
    std::vector<std::byte> out(64);
    for (int iter = 0; iter < 30'000; ++iter) {
        switch (rng.bounded(9)) {
            case 0: {
                const itch::SystemEvent m{random_header(rng), random_alpha(rng)};
                check_random_encode(m, 12, 'S', out);
                break;
            }
            case 1: {
                itch::StockDirectory m{};
                m.h = random_header(rng);
                m.symbol = random_symbol(rng);
                m.market_category = random_alpha(rng);
                m.financial_status = random_alpha(rng);
                m.round_lot_size = static_cast<Qty>(pick(rng, 0xFFFF'FFFFu));
                m.round_lots_only = random_alpha(rng);
                m.issue_classification = random_alpha(rng);
                m.issue_subtype[0] = random_alpha(rng);
                m.issue_subtype[1] = random_alpha(rng);
                m.authenticity = random_alpha(rng);
                m.short_sale_threshold = random_alpha(rng);
                m.ipo_flag = random_alpha(rng);
                m.luld_tier = random_alpha(rng);
                m.etp_flag = random_alpha(rng);
                m.etp_leverage = static_cast<std::uint32_t>(pick(rng, 0xFFFF'FFFFu));
                m.inverse = random_alpha(rng);
                check_random_encode(m, 39, 'R', out);
                break;
            }
            case 2: {
                itch::AddOrder m{};
                m.h = random_header(rng);
                m.ref = pick(rng, ~0ULL);
                m.side = rng.chance(1, 2) ? Side::buy : Side::sell;
                m.shares = static_cast<Qty>(std::max<std::uint64_t>(1, pick(rng, 0xFFFF'FFFFu)));
                m.symbol = random_symbol(rng);
                m.price = static_cast<Price>(pick(rng, 0xFFFF'FFFFu));
                m.has_attribution = rng.chance(1, 2);
                if (m.has_attribution) {
                    for (char& c : m.mpid) c = static_cast<char>(rng.range(0x41, 0x5A));
                }
                check_random_encode(m, m.has_attribution ? 40 : 36, m.has_attribution ? 'F' : 'A', out);
                break;
            }
            case 3: {
                const itch::OrderExecuted m{random_header(rng), pick(rng, ~0ULL),
                                            static_cast<Qty>(pick(rng, 0xFFFF'FFFFu)), pick(rng, ~0ULL)};
                check_random_encode(m, 31, 'E', out);
                break;
            }
            case 4: {
                const itch::OrderExecutedPrice m{random_header(rng),
                                                 pick(rng, ~0ULL),
                                                 static_cast<Qty>(pick(rng, 0xFFFF'FFFFu)),
                                                 pick(rng, ~0ULL),
                                                 rng.chance(1, 2),
                                                 static_cast<Price>(pick(rng, 0xFFFF'FFFFu))};
                check_random_encode(m, 36, 'C', out);
                break;
            }
            case 5: {
                const itch::OrderCancel m{random_header(rng), pick(rng, ~0ULL),
                                          static_cast<Qty>(pick(rng, 0xFFFF'FFFFu))};
                check_random_encode(m, 23, 'X', out);
                break;
            }
            case 6: {
                const itch::OrderDelete m{random_header(rng), pick(rng, ~0ULL)};
                check_random_encode(m, 19, 'D', out);
                break;
            }
            case 7: {
                const itch::OrderReplace m{random_header(rng), pick(rng, ~0ULL), pick(rng, ~0ULL),
                                           static_cast<Qty>(pick(rng, 0xFFFF'FFFFu)),
                                           static_cast<Price>(pick(rng, 0xFFFF'FFFFu))};
                check_random_encode(m, 35, 'U', out);
                break;
            }
            default: {
                itch::Trade m{};
                m.h = random_header(rng);
                m.ref = pick(rng, ~0ULL);
                m.side = rng.chance(1, 2) ? Side::buy : Side::sell;
                m.shares = static_cast<Qty>(pick(rng, 0xFFFF'FFFFu));
                m.symbol = random_symbol(rng);
                m.price = static_cast<Price>(pick(rng, 0xFFFF'FFFFu));
                m.match = pick(rng, ~0ULL);
                check_random_encode(m, 44, 'P', out);
                break;
            }
        }
    }
}

OT_TEST_MAIN()

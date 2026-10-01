#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "optitrade/core/decode_status.hpp"
#include "optitrade/core/endian.hpp"
#include "optitrade/ouch/messages.hpp"

// OUCH 4.2 encoder and bounds-checked decoder.
//
// Decoding takes one unframed message and never reads outside the span it is
// given: the type byte selects the exact wire length and the span must match it.
//   empty span or fewer bytes than the type needs -> truncated
//   more bytes than the type needs                -> bad_length
//   type byte not supported in that direction     -> unknown_type
//   a field the protocol forbids                  -> bad_field
// The handler is called only when the result is ok.
//
// Field policy. Inbound Enter/Replace require 1 <= shares <= 999'999, as the
// exchange does. Outbound messages are validated only where the value selects a
// layout or a state (buy/sell indicator, order state); the client trusts nothing
// else about them and the order manager clamps quantities itself. Prices are
// taken as sent: 0x7FFFFFFF is the market-order price, not an error. Alpha
// fields (display, capacity, reason, ...) are passed through untouched, since the
// exchange adds codes over time.
//
// Encoding is a faithful serializer, not a validator: it writes exactly the
// given fields so the simulator and tests can produce invalid traffic on
// purpose. The only value it refuses is a price that does not fit the u32 wire
// field (negative or above 0xFFFFFFFF): truncating it could turn a limit order
// into a market order, so the call writes nothing and returns 0.
//
// Framing. Real OUCH is carried on SoupBinTCP. This library uses a plain 2-byte
// big-endian length prefix in front of each message instead; it keeps the
// message layer testable without a session layer and matches how the ITCH
// BinaryFILE format frames messages.
namespace optitrade::ouch {

namespace detail {

// Field offsets, transcribed from the OUCH 4.2 message tables. Every byte of
// every message belongs to exactly one field; the asserts below pin the totals.
namespace enter {
inline constexpr std::size_t token = 1, side = 15, shares = 16, stock = 20, price = 28, tif = 32,
                             firm = 36, display = 40, capacity = 41, sweep = 42, min_qty = 43,
                             cross = 47, customer = 48;
}
namespace replace {
inline constexpr std::size_t existing = 1, replacement = 15, shares = 29, price = 33, tif = 37,
                             display = 41, sweep = 42, min_qty = 43;
}
namespace cancel {
inline constexpr std::size_t token = 1, shares = 15;
}
// Accepted and Replaced share bytes 0..64; they differ in what follows.
namespace accepted {
inline constexpr std::size_t ts = 1, token = 9, side = 23, shares = 24, stock = 28, price = 36,
                             tif = 40, firm = 44, display = 48, ref = 49, capacity = 57, sweep = 58,
                             min_qty = 59, cross = 63, state = 64, bbo = 65;
}
namespace replaced {
inline constexpr std::size_t previous = 65, bbo = 79;
}
namespace canceled {
inline constexpr std::size_t token = 9, decrement = 23, reason = 27;
}
namespace executed {
inline constexpr std::size_t token = 9, shares = 23, price = 27, liquidity = 31, match = 32;
}
namespace rejected {
inline constexpr std::size_t token = 9, reason = 23;
}

static_assert(enter::customer + 1 == kEnterOrderLength);
static_assert(enter::cross == enter::min_qty + 4 && enter::min_qty == enter::sweep + 1);
static_assert(replace::min_qty + 4 == kReplaceOrderLength);
static_assert(cancel::shares + 4 == kCancelOrderLength);
static_assert(accepted::bbo + 1 == kAcceptedLength);
static_assert(accepted::state + 1 == accepted::bbo);
static_assert(replaced::previous == accepted::state + 1);
static_assert(replaced::bbo == replaced::previous + kTokenSize);
static_assert(replaced::bbo + 1 == kReplacedLength);
static_assert(canceled::reason + 1 == kCanceledLength);
static_assert(executed::match + 8 == kExecutedLength);
static_assert(rejected::reason + 1 == kRejectedLength);

inline char get_char(const std::byte* p) noexcept {
    return static_cast<char>(std::to_integer<unsigned char>(*p));
}
inline void put_char(std::byte* p, char c) noexcept {
    *p = static_cast<std::byte>(static_cast<unsigned char>(c));
}

inline bool valid_order_shares(Qty q) noexcept { return q >= 1 && q <= kMaxOrderQty; }

// The price field is a u32 on the wire; Price is wider so arithmetic cannot overflow.
inline bool fits_wire_price(Price p) noexcept { return p >= 0 && p <= 0xFFFFFFFFLL; }

inline bool parse_side(char c, Side& side, bool& short_sell) noexcept {
    switch (c) {
        case 'B': side = Side::buy; short_sell = false; return true;
        case 'S': side = Side::sell; short_sell = false; return true;
        case 'T':
        case 'E': side = Side::sell; short_sell = true; return true;
        default: return false;
    }
}
inline char side_char(Side side, bool short_sell) noexcept {
    if (side == Side::buy) return 'B';
    return short_sell ? 'T' : 'S';
}

// ---- readers: `p` points at a message whose length has already been checked ----

inline bool read(const std::byte* p, EnterOrder& m) noexcept {
    namespace f = enter;
    if (!parse_side(get_char(p + f::side), m.side, m.short_sell)) return false;
    m.shares = be::load32(p + f::shares);
    if (!valid_order_shares(m.shares)) return false;
    m.token = Token::from_wire(p + f::token);
    m.stock = Symbol::from_wire(p + f::stock);
    m.price = static_cast<Price>(be::load32(p + f::price));
    m.time_in_force = be::load32(p + f::tif);
    std::memcpy(m.firm, p + f::firm, sizeof m.firm);
    m.display = get_char(p + f::display);
    m.capacity = get_char(p + f::capacity);
    m.intermarket_sweep = get_char(p + f::sweep);
    m.min_qty = be::load32(p + f::min_qty);
    m.cross_type = get_char(p + f::cross);
    m.customer_type = get_char(p + f::customer);
    return true;
}

inline bool read(const std::byte* p, ReplaceOrder& m) noexcept {
    namespace f = replace;
    m.shares = be::load32(p + f::shares);
    if (!valid_order_shares(m.shares)) return false;
    m.existing = Token::from_wire(p + f::existing);
    m.replacement = Token::from_wire(p + f::replacement);
    m.price = static_cast<Price>(be::load32(p + f::price));
    m.time_in_force = be::load32(p + f::tif);
    m.display = get_char(p + f::display);
    m.intermarket_sweep = get_char(p + f::sweep);
    m.min_qty = be::load32(p + f::min_qty);
    return true;
}

inline bool read(const std::byte* p, CancelOrder& m) noexcept {
    m.token = Token::from_wire(p + cancel::token);
    m.shares = be::load32(p + cancel::shares);
    return true;
}

// Bytes 1..64, shared by Accepted and Replaced. Leaves bbo_weight to the caller
// because it sits at a different offset in each.
inline bool read_accepted_common(const std::byte* p, Accepted& m) noexcept {
    namespace f = accepted;
    bool ignored_short = false;
    if (!parse_side(get_char(p + f::side), m.side, ignored_short)) return false;
    m.order_state = get_char(p + f::state);
    if (m.order_state != 'L' && m.order_state != 'D') return false;
    m.ts = be::load64(p + f::ts);
    m.token = Token::from_wire(p + f::token);
    m.shares = be::load32(p + f::shares);
    m.stock = Symbol::from_wire(p + f::stock);
    m.price = static_cast<Price>(be::load32(p + f::price));
    m.time_in_force = be::load32(p + f::tif);
    std::memcpy(m.firm, p + f::firm, sizeof m.firm);
    m.display = get_char(p + f::display);
    m.ref = be::load64(p + f::ref);
    m.capacity = get_char(p + f::capacity);
    m.intermarket_sweep = get_char(p + f::sweep);
    m.min_qty = be::load32(p + f::min_qty);
    m.cross_type = get_char(p + f::cross);
    return true;
}

inline bool read(const std::byte* p, Accepted& m) noexcept {
    if (!read_accepted_common(p, m)) return false;
    m.bbo_weight = get_char(p + accepted::bbo);
    return true;
}

inline bool read(const std::byte* p, Replaced& m) noexcept {
    if (!read_accepted_common(p, m.a)) return false;
    m.previous = Token::from_wire(p + replaced::previous);
    m.a.bbo_weight = get_char(p + replaced::bbo);
    return true;
}

inline bool read(const std::byte* p, Canceled& m) noexcept {
    m.ts = be::load64(p + accepted::ts);
    m.token = Token::from_wire(p + canceled::token);
    m.decrement = be::load32(p + canceled::decrement);
    m.reason = get_char(p + canceled::reason);
    return true;
}

inline bool read(const std::byte* p, Executed& m) noexcept {
    m.ts = be::load64(p + accepted::ts);
    m.token = Token::from_wire(p + executed::token);
    m.shares = be::load32(p + executed::shares);
    m.price = static_cast<Price>(be::load32(p + executed::price));
    m.liquidity = get_char(p + executed::liquidity);
    m.match = be::load64(p + executed::match);
    return true;
}

inline bool read(const std::byte* p, Rejected& m) noexcept {
    m.ts = be::load64(p + accepted::ts);
    m.token = Token::from_wire(p + rejected::token);
    m.reason = get_char(p + rejected::reason);
    return true;
}

// ---- writers: `p` has room for the whole message; false = nothing was written ----

inline bool write(std::byte* p, const EnterOrder& m) noexcept {
    namespace f = enter;
    if (!fits_wire_price(m.price)) return false;
    put_char(p, 'O');
    std::memcpy(p + f::token, m.token.raw().data(), kTokenSize);
    put_char(p + f::side, side_char(m.side, m.short_sell));
    be::store32(p + f::shares, m.shares);
    std::memcpy(p + f::stock, m.stock.raw().data(), Symbol::kSize);
    be::store32(p + f::price, static_cast<std::uint32_t>(m.price));
    be::store32(p + f::tif, m.time_in_force);
    std::memcpy(p + f::firm, m.firm, sizeof m.firm);
    put_char(p + f::display, m.display);
    put_char(p + f::capacity, m.capacity);
    put_char(p + f::sweep, m.intermarket_sweep);
    be::store32(p + f::min_qty, m.min_qty);
    put_char(p + f::cross, m.cross_type);
    put_char(p + f::customer, m.customer_type);
    return true;
}

inline bool write(std::byte* p, const ReplaceOrder& m) noexcept {
    namespace f = replace;
    if (!fits_wire_price(m.price)) return false;
    put_char(p, 'U');
    std::memcpy(p + f::existing, m.existing.raw().data(), kTokenSize);
    std::memcpy(p + f::replacement, m.replacement.raw().data(), kTokenSize);
    be::store32(p + f::shares, m.shares);
    be::store32(p + f::price, static_cast<std::uint32_t>(m.price));
    be::store32(p + f::tif, m.time_in_force);
    put_char(p + f::display, m.display);
    put_char(p + f::sweep, m.intermarket_sweep);
    be::store32(p + f::min_qty, m.min_qty);
    return true;
}

inline bool write(std::byte* p, const CancelOrder& m) noexcept {
    put_char(p, 'X');
    std::memcpy(p + cancel::token, m.token.raw().data(), kTokenSize);
    be::store32(p + cancel::shares, m.shares);
    return true;
}

inline void write_accepted_common(std::byte* p, char type, const Accepted& m) noexcept {
    namespace f = accepted;
    put_char(p, type);
    be::store64(p + f::ts, m.ts);
    std::memcpy(p + f::token, m.token.raw().data(), kTokenSize);
    put_char(p + f::side, side_char(m.side, false));
    be::store32(p + f::shares, m.shares);
    std::memcpy(p + f::stock, m.stock.raw().data(), Symbol::kSize);
    be::store32(p + f::price, static_cast<std::uint32_t>(m.price));
    be::store32(p + f::tif, m.time_in_force);
    std::memcpy(p + f::firm, m.firm, sizeof m.firm);
    put_char(p + f::display, m.display);
    be::store64(p + f::ref, m.ref);
    put_char(p + f::capacity, m.capacity);
    put_char(p + f::sweep, m.intermarket_sweep);
    be::store32(p + f::min_qty, m.min_qty);
    put_char(p + f::cross, m.cross_type);
    put_char(p + f::state, m.order_state);
}

inline bool write(std::byte* p, const Accepted& m) noexcept {
    if (!fits_wire_price(m.price)) return false;
    write_accepted_common(p, 'A', m);
    put_char(p + accepted::bbo, m.bbo_weight);
    return true;
}

inline bool write(std::byte* p, const Replaced& m) noexcept {
    if (!fits_wire_price(m.a.price)) return false;
    write_accepted_common(p, 'U', m.a);
    std::memcpy(p + replaced::previous, m.previous.raw().data(), kTokenSize);
    put_char(p + replaced::bbo, m.a.bbo_weight);
    return true;
}

inline bool write(std::byte* p, const Canceled& m) noexcept {
    put_char(p, 'C');
    be::store64(p + accepted::ts, m.ts);
    std::memcpy(p + canceled::token, m.token.raw().data(), kTokenSize);
    be::store32(p + canceled::decrement, m.decrement);
    put_char(p + canceled::reason, m.reason);
    return true;
}

inline bool write(std::byte* p, const Executed& m) noexcept {
    if (!fits_wire_price(m.price)) return false;
    put_char(p, 'E');
    be::store64(p + accepted::ts, m.ts);
    std::memcpy(p + executed::token, m.token.raw().data(), kTokenSize);
    be::store32(p + executed::shares, m.shares);
    be::store32(p + executed::price, static_cast<std::uint32_t>(m.price));
    put_char(p + executed::liquidity, m.liquidity);
    be::store64(p + executed::match, m.match);
    return true;
}

inline bool write(std::byte* p, const Rejected& m) noexcept {
    put_char(p, 'J');
    be::store64(p + accepted::ts, m.ts);
    std::memcpy(p + rejected::token, m.token.raw().data(), kTokenSize);
    put_char(p + rejected::reason, m.reason);
    return true;
}

template <class M>
std::size_t emit(const M& m, std::span<std::byte> out, std::size_t length) noexcept {
    if (out.size() < length || !write(out.data(), m)) return 0;
    return length;
}

template <class M>
std::size_t emit_framed(const M& m, std::span<std::byte> out, std::size_t length) noexcept {
    if (out.size() < length + 2 || !write(out.data() + 2, m)) return 0;
    be::store16(out.data(), static_cast<std::uint16_t>(length));
    return length + 2;
}

// Reads one message of type M from `p` and hands it to the handler.
template <class M, class H>
DecodeStatus deliver(const std::byte* p, H& h) noexcept {
    M m;
    if (!read(p, m)) return DecodeStatus::bad_field;
    const M& delivered = m;
    h.on(delivered);
    return DecodeStatus::ok;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

// One unframed client -> exchange message. The handler receives EnterOrder,
// ReplaceOrder or CancelOrder.
template <class H>
DecodeStatus decode_inbound(std::span<const std::byte> msg, H& h) noexcept {
    if (msg.empty()) return DecodeStatus::truncated;
    const char type = detail::get_char(msg.data());
    const std::size_t length = inbound_length(type);
    if (length == 0) return DecodeStatus::unknown_type;
    if (msg.size() < length) return DecodeStatus::truncated;
    if (msg.size() > length) return DecodeStatus::bad_length;
    switch (type) {
        case 'O': return detail::deliver<EnterOrder>(msg.data(), h);
        case 'U': return detail::deliver<ReplaceOrder>(msg.data(), h);
        case 'X': return detail::deliver<CancelOrder>(msg.data(), h);
        default: return DecodeStatus::unknown_type;  // unreachable: inbound_length() is 0 for the rest
    }
}

// One unframed exchange -> client message. The handler receives Accepted,
// Replaced, Canceled, Executed or Rejected. Other outbound types (system event,
// AIQ cancel, broken trade, ...) give unknown_type.
template <class H>
DecodeStatus decode_outbound(std::span<const std::byte> msg, H& h) noexcept {
    if (msg.empty()) return DecodeStatus::truncated;
    const char type = detail::get_char(msg.data());
    const std::size_t length = outbound_length(type);
    if (length == 0) return DecodeStatus::unknown_type;
    if (msg.size() < length) return DecodeStatus::truncated;
    if (msg.size() > length) return DecodeStatus::bad_length;
    switch (type) {
        case 'A': return detail::deliver<Accepted>(msg.data(), h);
        case 'U': return detail::deliver<Replaced>(msg.data(), h);
        case 'C': return detail::deliver<Canceled>(msg.data(), h);
        case 'E': return detail::deliver<Executed>(msg.data(), h);
        case 'J': return detail::deliver<Rejected>(msg.data(), h);
        default: return DecodeStatus::unknown_type;  // unreachable: outbound_length() is 0 for the rest
    }
}

// ---------------------------------------------------------------------------
// Stream decoding: [u16 big-endian length][message] repeated
// ---------------------------------------------------------------------------

struct StreamResult {
    std::size_t consumed{};  // bytes covered by complete frames
    std::size_t messages{};  // frames decoded and delivered to the handler
    std::size_t skipped{};   // complete frames that failed to decode
    DecodeStatus last_error{DecodeStatus::ok};
};

namespace detail {

template <class Decode>
StreamResult deframe(std::span<const std::byte> buf, Decode&& decode_one) noexcept {
    StreamResult r;
    while (buf.size() - r.consumed >= 2) {
        const std::byte* p = buf.data() + r.consumed;
        const std::size_t length = be::load16(p);
        if (buf.size() - r.consumed - 2 < length) break;  // partial frame: wait for more bytes
        const DecodeStatus st = decode_one(std::span<const std::byte>(p + 2, length));
        if (st == DecodeStatus::ok) {
            ++r.messages;
        } else {
            ++r.skipped;
            r.last_error = st;
        }
        r.consumed += 2 + length;
    }
    return r;
}

}  // namespace detail

// Decodes every complete frame in `buf`. A frame that fails to decode (including
// a zero-length frame, which counts as truncated) is counted in `skipped` and
// the stream carries on, because the length prefix already says where the next
// frame starts. A partial trailing frame is left unconsumed: `consumed` points at
// its first byte so the caller can keep the tail and append more data. There is
// no resynchronisation after a corrupt length prefix; that needs the session
// layer this library replaces.
template <class H>
StreamResult decode_inbound_stream(std::span<const std::byte> buf, H& h) noexcept {
    return detail::deframe(buf, [&h](std::span<const std::byte> m) noexcept {
        return decode_inbound(m, h);
    });
}

template <class H>
StreamResult decode_outbound_stream(std::span<const std::byte> buf, H& h) noexcept {
    return detail::deframe(buf, [&h](std::span<const std::byte> m) noexcept {
        return decode_outbound(m, h);
    });
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

// Each encode() writes exactly the message's wire length and returns it, or
// returns 0 and writes nothing if `out` is too small or the price does not fit
// the wire field. encode_framed() puts the u16 length prefix in front and
// returns length + 2.

inline std::size_t encode(const EnterOrder& m, std::span<std::byte> out) noexcept {
    return detail::emit(m, out, kEnterOrderLength);
}
inline std::size_t encode(const ReplaceOrder& m, std::span<std::byte> out) noexcept {
    return detail::emit(m, out, kReplaceOrderLength);
}
inline std::size_t encode(const CancelOrder& m, std::span<std::byte> out) noexcept {
    return detail::emit(m, out, kCancelOrderLength);
}
inline std::size_t encode(const Accepted& m, std::span<std::byte> out) noexcept {
    return detail::emit(m, out, kAcceptedLength);
}
inline std::size_t encode(const Replaced& m, std::span<std::byte> out) noexcept {
    return detail::emit(m, out, kReplacedLength);
}
inline std::size_t encode(const Canceled& m, std::span<std::byte> out) noexcept {
    return detail::emit(m, out, kCanceledLength);
}
inline std::size_t encode(const Executed& m, std::span<std::byte> out) noexcept {
    return detail::emit(m, out, kExecutedLength);
}
inline std::size_t encode(const Rejected& m, std::span<std::byte> out) noexcept {
    return detail::emit(m, out, kRejectedLength);
}

inline std::size_t encode_framed(const EnterOrder& m, std::span<std::byte> out) noexcept {
    return detail::emit_framed(m, out, kEnterOrderLength);
}
inline std::size_t encode_framed(const ReplaceOrder& m, std::span<std::byte> out) noexcept {
    return detail::emit_framed(m, out, kReplaceOrderLength);
}
inline std::size_t encode_framed(const CancelOrder& m, std::span<std::byte> out) noexcept {
    return detail::emit_framed(m, out, kCancelOrderLength);
}
inline std::size_t encode_framed(const Accepted& m, std::span<std::byte> out) noexcept {
    return detail::emit_framed(m, out, kAcceptedLength);
}
inline std::size_t encode_framed(const Replaced& m, std::span<std::byte> out) noexcept {
    return detail::emit_framed(m, out, kReplacedLength);
}
inline std::size_t encode_framed(const Canceled& m, std::span<std::byte> out) noexcept {
    return detail::emit_framed(m, out, kCanceledLength);
}
inline std::size_t encode_framed(const Executed& m, std::span<std::byte> out) noexcept {
    return detail::emit_framed(m, out, kExecutedLength);
}
inline std::size_t encode_framed(const Rejected& m, std::span<std::byte> out) noexcept {
    return detail::emit_framed(m, out, kRejectedLength);
}

}  // namespace optitrade::ouch

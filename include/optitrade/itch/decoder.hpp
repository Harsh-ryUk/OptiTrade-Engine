#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "optitrade/core/decode_status.hpp"
#include "optitrade/core/endian.hpp"
#include "optitrade/core/types.hpp"
#include "optitrade/itch/messages.hpp"

// ITCH 5.0 decoding: one unframed message (`decode`) or a BinaryFILE-style
// stream of length-prefixed messages (`decode_stream`).
//
// Guarantees, all covered by the unit tests and the fuzzer:
//  * no read outside the span passed in: the wire length is checked against the
//    span before the first field is touched, and fields are assembled from
//    byte loads, so alignment never matters;
//  * a message is delivered to the handler only after every field has been
//    validated, never half-decoded;
//  * no allocation, no exceptions. A handler that throws terminates the process
//    (the decoders are noexcept on purpose: the hot path has no unwinding).
namespace optitrade::itch {

namespace detail {

inline char byte_char(const std::byte* p) noexcept {
    return static_cast<char>(std::to_integer<unsigned char>(*p));
}

inline Header read_header(const std::byte* p) noexcept {
    return Header{be::load16(p + 1), be::load16(p + 3), be::load48(p + 5)};
}

inline Symbol read_symbol(const std::byte* p) noexcept { return Symbol::from_wire(p); }

// Prices are unsigned 32-bit on the wire (max 200,000.0000 = 2'000'000'000, but
// we do not police the exchange's own range); widening to int64 cannot overflow.
inline Price read_price(const std::byte* p) noexcept { return static_cast<Price>(be::load32(p)); }

// Anything but 'B' or 'S' is rejected: guessing a side would put liquidity on
// the wrong side of the book.
inline bool read_side(const std::byte* p, Side& out) noexcept {
    switch (byte_char(p)) {
        case 'B': out = Side::buy; return true;
        case 'S': out = Side::sell; return true;
        default: return false;
    }
}

// The decode_* helpers below are only reached after the caller verified that the
// span holds exactly the wire length of the type, so every offset is in range.

template <class Handler>
DecodeStatus decode_system_event(const std::byte* p, Handler& handler) noexcept {
    SystemEvent m{};
    m.h = read_header(p);
    m.event_code = byte_char(p + 11);
    handler.on(m);
    return DecodeStatus::ok;
}

template <class Handler>
DecodeStatus decode_stock_directory(const std::byte* p, Handler& handler) noexcept {
    StockDirectory m{};
    m.h = read_header(p);
    m.symbol = read_symbol(p + 11);
    m.market_category = byte_char(p + 19);
    m.financial_status = byte_char(p + 20);
    m.round_lot_size = be::load32(p + 21);
    m.round_lots_only = byte_char(p + 25);
    m.issue_classification = byte_char(p + 26);
    m.issue_subtype[0] = byte_char(p + 27);
    m.issue_subtype[1] = byte_char(p + 28);
    m.authenticity = byte_char(p + 29);
    m.short_sale_threshold = byte_char(p + 30);
    m.ipo_flag = byte_char(p + 31);
    m.luld_tier = byte_char(p + 32);
    m.etp_flag = byte_char(p + 33);
    m.etp_leverage = be::load32(p + 34);
    m.inverse = byte_char(p + 38);
    handler.on(m);
    return DecodeStatus::ok;
}

// 'A' and 'F' share bytes 0..35; 'F' appends the 4-byte attribution.
template <class Handler>
DecodeStatus decode_add_order(const std::byte* p, bool attributed, Handler& handler) noexcept {
    AddOrder m{};
    m.h = read_header(p);
    m.ref = be::load64(p + 11);
    if (!read_side(p + 19, m.side)) return DecodeStatus::bad_field;
    m.shares = be::load32(p + 20);
    // An add of zero shares would create a phantom level entry.
    if (m.shares == 0) return DecodeStatus::bad_field;
    m.symbol = read_symbol(p + 24);
    m.price = read_price(p + 32);
    if (attributed) {
        m.has_attribution = true;
        std::memcpy(m.mpid, p + 36, sizeof m.mpid);
    }
    handler.on(m);
    return DecodeStatus::ok;
}

template <class Handler>
DecodeStatus decode_order_executed(const std::byte* p, Handler& handler) noexcept {
    OrderExecuted m{};
    m.h = read_header(p);
    m.ref = be::load64(p + 11);
    m.shares = be::load32(p + 19);
    m.match = be::load64(p + 23);
    handler.on(m);
    return DecodeStatus::ok;
}

template <class Handler>
DecodeStatus decode_order_executed_price(const std::byte* p, Handler& handler) noexcept {
    OrderExecutedPrice m{};
    m.h = read_header(p);
    m.ref = be::load64(p + 11);
    m.shares = be::load32(p + 19);
    m.match = be::load64(p + 23);
    m.printable = byte_char(p + 31) == 'Y';
    m.price = read_price(p + 32);
    handler.on(m);
    return DecodeStatus::ok;
}

template <class Handler>
DecodeStatus decode_order_cancel(const std::byte* p, Handler& handler) noexcept {
    OrderCancel m{};
    m.h = read_header(p);
    m.ref = be::load64(p + 11);
    m.shares = be::load32(p + 19);
    handler.on(m);
    return DecodeStatus::ok;
}

template <class Handler>
DecodeStatus decode_order_delete(const std::byte* p, Handler& handler) noexcept {
    OrderDelete m{};
    m.h = read_header(p);
    m.ref = be::load64(p + 11);
    handler.on(m);
    return DecodeStatus::ok;
}

template <class Handler>
DecodeStatus decode_order_replace(const std::byte* p, Handler& handler) noexcept {
    OrderReplace m{};
    m.h = read_header(p);
    m.old_ref = be::load64(p + 11);
    m.new_ref = be::load64(p + 19);
    m.shares = be::load32(p + 27);
    m.price = read_price(p + 31);
    handler.on(m);
    return DecodeStatus::ok;
}

template <class Handler>
DecodeStatus decode_trade(const std::byte* p, Handler& handler) noexcept {
    Trade m{};
    m.h = read_header(p);
    m.ref = be::load64(p + 11);  // zero on today's feed; kept for older captures
    if (!read_side(p + 19, m.side)) return DecodeStatus::bad_field;
    m.shares = be::load32(p + 20);
    m.symbol = read_symbol(p + 24);
    m.price = read_price(p + 32);
    m.match = be::load64(p + 36);
    handler.on(m);
    return DecodeStatus::ok;
}

}  // namespace detail

// Decodes exactly one unframed message and hands it to `handler.on(msg)`.
//
// Status, in the order the checks run:
//   truncated     the span is empty (no type byte) or shorter than the type requires
//   unknown_type  the type byte is not one of the ten supported types
//   bad_length    the span is longer than the type allows (trailing bytes are never ignored)
//   bad_field     side is not 'B'/'S' (A, F, P), or shares == 0 (A, F)
// The handler is called only when the result is `ok`.
//
// An unknown type is reported before any length check, so a caller can tell a
// valid-but-unmodelled ITCH type from a damaged message of a known type.
template <class Handler>
DecodeStatus decode(std::span<const std::byte> msg, Handler& handler) noexcept {
    if (msg.empty()) return DecodeStatus::truncated;
    const std::byte* p = msg.data();
    const char type = detail::byte_char(p);
    const std::size_t len = message_length(type);
    if (len == 0) return DecodeStatus::unknown_type;
    if (msg.size() < len) return DecodeStatus::truncated;
    if (msg.size() > len) return DecodeStatus::bad_length;

    switch (type) {
        case 'S': return detail::decode_system_event(p, handler);
        case 'R': return detail::decode_stock_directory(p, handler);
        case 'A': return detail::decode_add_order(p, false, handler);
        case 'F': return detail::decode_add_order(p, true, handler);
        case 'E': return detail::decode_order_executed(p, handler);
        case 'C': return detail::decode_order_executed_price(p, handler);
        case 'X': return detail::decode_order_cancel(p, handler);
        case 'D': return detail::decode_order_delete(p, handler);
        case 'U': return detail::decode_order_replace(p, handler);
        case 'P': return detail::decode_trade(p, handler);
        default: break;
    }
    return DecodeStatus::unknown_type;  // unreachable: message_length() admitted only the cases above
}

struct StreamResult {
    std::size_t consumed{};   // bytes of complete frames processed; always a frame boundary
    std::size_t messages{};   // frames decoded and delivered to the handler
    std::size_t skipped{};    // complete frames not delivered (unsupported type or malformed)
    DecodeStatus last_error{DecodeStatus::ok};  // status of the most recent skipped frame; not cleared by later successes
};

// Decodes a BinaryFILE-style stream: repeated [u16 big-endian length][message].
//
// Every *complete* frame is processed. A frame whose message has an unsupported
// type, the wrong length for its type, or an illegal field is counted in
// `skipped` and decoding continues at the next frame; the length prefix is what
// makes resynchronisation exact, so one bad message never costs its neighbours.
// A zero-length frame carries no type byte, so it is skipped with `truncated`.
//
// A partial trailing frame (fewer than two prefix bytes, or a prefix announcing
// more bytes than remain) is left unconsumed: `consumed` is its start offset, and
// the caller appends more input and calls again from there. Splitting a stream at
// any byte boundary therefore yields the same messages as decoding it in one call.
// A frame that can never complete (prefix larger than the caller's buffer) shows up
// as `consumed == 0` with a full buffer; the caller decides whether to give up.
template <class Handler>
StreamResult decode_stream(std::span<const std::byte> buf, Handler& handler) noexcept {
    StreamResult r;
    const std::size_t size = buf.size();
    while (size - r.consumed >= kFrameHeaderSize) {
        const std::size_t len = be::load16(buf.data() + r.consumed);
        const std::size_t available = size - r.consumed - kFrameHeaderSize;
        if (available < len) break;  // partial trailing frame

        const DecodeStatus st = decode(buf.subspan(r.consumed + kFrameHeaderSize, len), handler);
        if (st == DecodeStatus::ok) {
            ++r.messages;
        } else {
            ++r.skipped;
            r.last_error = st;
        }
        r.consumed += kFrameHeaderSize + len;
    }
    return r;
}

}  // namespace optitrade::itch

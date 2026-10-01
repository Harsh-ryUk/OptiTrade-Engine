#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "optitrade/core/endian.hpp"
#include "optitrade/core/types.hpp"
#include "optitrade/itch/messages.hpp"

// ITCH 5.0 encoding, the inverse of decoder.hpp. Used by the synthetic market
// generator, the exchange simulator's replays and the tests.
//
// Every encoder writes exactly the wire length of the message and returns it, or
// returns 0 and leaves the output untouched when
//  * `out` is smaller than the message, or
//  * a value has no wire representation: a price outside [0, 2^32 - 1]. The wire
//    field is an unsigned 32-bit integer, so silently wrapping a negative price
//    into a four-billion one would corrupt the feed without a trace.
// The timestamp is the one field that is truncated rather than refused: ITCH
// carries 48 bits (about 78 hours of nanoseconds), far more than a trading day.
//
// The encoders are dumb serialisers on purpose. They do not enforce what the
// decoder enforces (a zero-share add, say, encodes fine and is then rejected on
// the way back in), which lets tests build malformed traffic with the real code.
namespace optitrade::itch {

namespace detail {

constexpr bool fits_wire_price(Price px) noexcept { return px >= 0 && px <= 0xFFFF'FFFFLL; }

inline void put_char(std::byte* p, char c) noexcept {
    *p = static_cast<std::byte>(static_cast<unsigned char>(c));
}

inline void put_header(std::byte* p, char type, const Header& h) noexcept {
    put_char(p, type);
    be::store16(p + 1, h.locate);
    be::store16(p + 3, h.tracking);
    be::store48(p + 5, h.timestamp);  // store48 keeps the low 48 bits
}

inline void put_symbol(std::byte* p, const Symbol& s) noexcept {
    std::memcpy(p, s.raw().data(), Symbol::kSize);
}

// Callers validate with fits_wire_price() first so a failed encode writes nothing.
inline void put_price(std::byte* p, Price px) noexcept {
    be::store32(p, static_cast<std::uint32_t>(px));
}

constexpr char side_char(Side s) noexcept { return s == Side::sell ? 'S' : 'B'; }

}  // namespace detail

inline std::size_t encode(const SystemEvent& m, std::span<std::byte> out) noexcept {
    if (out.size() < kSystemEventLength) return 0;
    std::byte* p = out.data();
    detail::put_header(p, 'S', m.h);
    detail::put_char(p + 11, m.event_code);
    return kSystemEventLength;
}

inline std::size_t encode(const StockDirectory& m, std::span<std::byte> out) noexcept {
    if (out.size() < kStockDirectoryLength) return 0;
    std::byte* p = out.data();
    detail::put_header(p, 'R', m.h);
    detail::put_symbol(p + 11, m.symbol);
    detail::put_char(p + 19, m.market_category);
    detail::put_char(p + 20, m.financial_status);
    be::store32(p + 21, m.round_lot_size);
    detail::put_char(p + 25, m.round_lots_only);
    detail::put_char(p + 26, m.issue_classification);
    detail::put_char(p + 27, m.issue_subtype[0]);
    detail::put_char(p + 28, m.issue_subtype[1]);
    detail::put_char(p + 29, m.authenticity);
    detail::put_char(p + 30, m.short_sale_threshold);
    detail::put_char(p + 31, m.ipo_flag);
    detail::put_char(p + 32, m.luld_tier);
    detail::put_char(p + 33, m.etp_flag);
    be::store32(p + 34, m.etp_leverage);
    detail::put_char(p + 38, m.inverse);
    return kStockDirectoryLength;
}

// Writes an 'F' message when `has_attribution` is set, otherwise an 'A' message
// (and `mpid` is ignored).
inline std::size_t encode(const AddOrder& m, std::span<std::byte> out) noexcept {
    const std::size_t len = m.has_attribution ? kAddOrderMpidLength : kAddOrderLength;
    if (out.size() < len || !detail::fits_wire_price(m.price)) return 0;
    std::byte* p = out.data();
    detail::put_header(p, m.has_attribution ? 'F' : 'A', m.h);
    be::store64(p + 11, m.ref);
    detail::put_char(p + 19, detail::side_char(m.side));
    be::store32(p + 20, m.shares);
    detail::put_symbol(p + 24, m.symbol);
    detail::put_price(p + 32, m.price);
    if (m.has_attribution) std::memcpy(p + 36, m.mpid, sizeof m.mpid);
    return len;
}

inline std::size_t encode(const OrderExecuted& m, std::span<std::byte> out) noexcept {
    if (out.size() < kOrderExecutedLength) return 0;
    std::byte* p = out.data();
    detail::put_header(p, 'E', m.h);
    be::store64(p + 11, m.ref);
    be::store32(p + 19, m.shares);
    be::store64(p + 23, m.match);
    return kOrderExecutedLength;
}

inline std::size_t encode(const OrderExecutedPrice& m, std::span<std::byte> out) noexcept {
    if (out.size() < kOrderExecutedPriceLength || !detail::fits_wire_price(m.price)) return 0;
    std::byte* p = out.data();
    detail::put_header(p, 'C', m.h);
    be::store64(p + 11, m.ref);
    be::store32(p + 19, m.shares);
    be::store64(p + 23, m.match);
    detail::put_char(p + 31, m.printable ? 'Y' : 'N');
    detail::put_price(p + 32, m.price);
    return kOrderExecutedPriceLength;
}

inline std::size_t encode(const OrderCancel& m, std::span<std::byte> out) noexcept {
    if (out.size() < kOrderCancelLength) return 0;
    std::byte* p = out.data();
    detail::put_header(p, 'X', m.h);
    be::store64(p + 11, m.ref);
    be::store32(p + 19, m.shares);
    return kOrderCancelLength;
}

inline std::size_t encode(const OrderDelete& m, std::span<std::byte> out) noexcept {
    if (out.size() < kOrderDeleteLength) return 0;
    std::byte* p = out.data();
    detail::put_header(p, 'D', m.h);
    be::store64(p + 11, m.ref);
    return kOrderDeleteLength;
}

inline std::size_t encode(const OrderReplace& m, std::span<std::byte> out) noexcept {
    if (out.size() < kOrderReplaceLength || !detail::fits_wire_price(m.price)) return 0;
    std::byte* p = out.data();
    detail::put_header(p, 'U', m.h);
    be::store64(p + 11, m.old_ref);
    be::store64(p + 19, m.new_ref);
    be::store32(p + 27, m.shares);
    detail::put_price(p + 31, m.price);
    return kOrderReplaceLength;
}

inline std::size_t encode(const Trade& m, std::span<std::byte> out) noexcept {
    if (out.size() < kTradeLength || !detail::fits_wire_price(m.price)) return 0;
    std::byte* p = out.data();
    detail::put_header(p, 'P', m.h);
    be::store64(p + 11, m.ref);
    detail::put_char(p + 19, detail::side_char(m.side));
    be::store32(p + 20, m.shares);
    detail::put_symbol(p + 24, m.symbol);
    detail::put_price(p + 32, m.price);
    be::store64(p + 36, m.match);
    return kTradeLength;
}

template <class M>
concept WireMessage = std::same_as<M, SystemEvent> || std::same_as<M, StockDirectory> ||
                      std::same_as<M, AddOrder> || std::same_as<M, OrderExecuted> ||
                      std::same_as<M, OrderExecutedPrice> || std::same_as<M, OrderCancel> ||
                      std::same_as<M, OrderDelete> || std::same_as<M, OrderReplace> ||
                      std::same_as<M, Trade>;

// BinaryFILE framing: the big-endian u16 length of the message, then the message.
// Returns the total bytes written (2 + message length) or 0 on failure, in which
// case `out` is untouched.
template <WireMessage M>
std::size_t encode_framed(const M& m, std::span<std::byte> out) noexcept {
    if (out.size() < kFrameHeaderSize) return 0;
    const std::size_t len = encode(m, out.subspan(kFrameHeaderSize));
    if (len == 0) return 0;
    be::store16(out.data(), static_cast<std::uint16_t>(len));
    return kFrameHeaderSize + len;
}

}  // namespace optitrade::itch

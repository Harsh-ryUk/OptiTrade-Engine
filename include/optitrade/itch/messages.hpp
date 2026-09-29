#pragma once

#include <cstddef>
#include <cstdint>

#include "optitrade/core/types.hpp"

// Decoded Nasdaq TotalView-ITCH 5.0 messages.
//
// Only the message types that drive an order book and the strategy layer are
// modelled. Everything else on the feed (trading actions, NOII, cross trades,
// ...) is recognised by the stream decoder and skipped, never mistaken for an
// error. The structs are plain aggregates: fully value-initialised by the
// decoder, cheap to copy, and comparable, so a message can be compared as a
// whole in tests and fuzzers.
namespace optitrade::itch {

// Fields shared by every ITCH message (bytes 1..10 of the wire form).
struct Header {
    Locate locate{};             // instrument index for the trading day (0 = not instrument specific)
    std::uint16_t tracking{};    // Nasdaq internal tracking number
    Nanos timestamp{};           // nanoseconds since midnight; only the low 48 bits exist on the wire

    friend constexpr bool operator==(const Header&, const Header&) = default;
};

struct SystemEvent {
    Header h;
    char event_code{};

    friend constexpr bool operator==(const SystemEvent&, const SystemEvent&) = default;
};

struct StockDirectory {
    Header h;
    Symbol symbol;
    char market_category{};
    char financial_status{};
    Qty round_lot_size{};
    char round_lots_only{};
    char issue_classification{};
    char issue_subtype[2]{};
    char authenticity{};
    char short_sale_threshold{};
    char ipo_flag{};
    char luld_tier{};
    char etp_flag{};
    std::uint32_t etp_leverage{};
    char inverse{};

    friend constexpr bool operator==(const StockDirectory&, const StockDirectory&) = default;
};

// Add Order without ('A') and with ('F') market participant attribution. The two
// share one struct because every consumer treats them identically; `mpid` is
// meaningful only when `has_attribution` is set and is zero-filled otherwise.
struct AddOrder {
    Header h;
    OrderRef ref{};
    Side side{Side::buy};
    Qty shares{};
    Symbol symbol;
    Price price{};  // 4 implied decimals; the wire field is an unsigned 32-bit integer
    bool has_attribution{};
    char mpid[4]{};

    friend constexpr bool operator==(const AddOrder&, const AddOrder&) = default;
};

struct OrderExecuted {
    Header h;
    OrderRef ref{};
    Qty shares{};
    std::uint64_t match{};

    friend constexpr bool operator==(const OrderExecuted&, const OrderExecuted&) = default;
};

// Order Executed With Price. `printable` is true only for the wire value 'Y'; any
// other byte reads as non-printable so a corrupt flag can never inflate volume.
struct OrderExecutedPrice {
    Header h;
    OrderRef ref{};
    Qty shares{};
    std::uint64_t match{};
    bool printable{};
    Price price{};

    friend constexpr bool operator==(const OrderExecutedPrice&, const OrderExecutedPrice&) = default;
};

struct OrderCancel {
    Header h;
    OrderRef ref{};
    Qty shares{};  // shares removed from the displayed size (partial cancel)

    friend constexpr bool operator==(const OrderCancel&, const OrderCancel&) = default;
};

struct OrderDelete {
    Header h;
    OrderRef ref{};

    friend constexpr bool operator==(const OrderDelete&, const OrderDelete&) = default;
};

struct OrderReplace {
    Header h;
    OrderRef old_ref{};
    OrderRef new_ref{};
    Qty shares{};  // new total displayed quantity
    Price price{};

    friend constexpr bool operator==(const OrderReplace&, const OrderReplace&) = default;
};

// Non-cross Trade ('P'): an execution against a non-displayed order.
struct Trade {
    Header h;
    OrderRef ref{};
    Side side{Side::buy};
    Qty shares{};
    Symbol symbol;
    Price price{};
    std::uint64_t match{};

    friend constexpr bool operator==(const Trade&, const Trade&) = default;
};

// Wire length in bytes (type byte included, length prefix excluded) of every
// supported message type. Verified against the TotalView-ITCH 5.0 field tables.
inline constexpr std::size_t kSystemEventLength = 12;
inline constexpr std::size_t kStockDirectoryLength = 39;
inline constexpr std::size_t kAddOrderLength = 36;
inline constexpr std::size_t kAddOrderMpidLength = 40;
inline constexpr std::size_t kOrderExecutedLength = 31;
inline constexpr std::size_t kOrderExecutedPriceLength = 36;
inline constexpr std::size_t kOrderCancelLength = 23;
inline constexpr std::size_t kOrderDeleteLength = 19;
inline constexpr std::size_t kOrderReplaceLength = 35;
inline constexpr std::size_t kTradeLength = 44;

// Longest supported message; sizes scratch buffers.
inline constexpr std::size_t kMaxMessageLength = kTradeLength;

// Size of the big-endian length prefix used by BinaryFILE framing.
inline constexpr std::size_t kFrameHeaderSize = 2;

// Wire length of `type`, or 0 if the type is not one this library decodes. Valid
// ITCH types that we do not model (H, Y, L, Q, ...) also return 0: the stream
// decoder skips those by their frame length.
constexpr std::size_t message_length(char type) noexcept {
    switch (type) {
        case 'S': return kSystemEventLength;
        case 'R': return kStockDirectoryLength;
        case 'A': return kAddOrderLength;
        case 'F': return kAddOrderMpidLength;
        case 'E': return kOrderExecutedLength;
        case 'C': return kOrderExecutedPriceLength;
        case 'X': return kOrderCancelLength;
        case 'D': return kOrderDeleteLength;
        case 'U': return kOrderReplaceLength;
        case 'P': return kTradeLength;
        default: return 0;
    }
}

// Base for user handlers: `struct H : NullHandler { using NullHandler::on; void on(const AddOrder&); };`
// receives only the messages it declares and ignores the rest at zero cost.
struct NullHandler {
    template <class M>
    void on(const M&) noexcept {}
};

}  // namespace optitrade::itch

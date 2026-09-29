#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

#include "optitrade/core/types.hpp"

// OUCH 4.2 order-entry messages as plain value types. The byte-level layout lives
// in codec.hpp; nothing here depends on it.
//
// Wire summary (all integers big-endian, alpha fields left-justified and space
// padded, prices are u32 with four implied decimals):
//
//   inbound   'O' Enter 49   'U' Replace 47   'X' Cancel 19
//   outbound  'A' Accepted 66   'U' Replaced 80   'C' Canceled 28
//             'E' Executed 40   'J' Rejected 24
//
// Session framing is not part of these messages. Real OUCH rides on SoupBinTCP;
// this library uses the simplest possible stand-in, a 2-byte big-endian length
// prefix in front of every message (see codec.hpp).
namespace optitrade::ouch {

inline constexpr std::size_t kTokenSize = 14;

namespace detail {
constexpr std::array<char, kTokenSize> blank_token() noexcept {
    std::array<char, kTokenSize> r{};
    for (char& c : r) c = ' ';
    return r;
}
}  // namespace detail

// The 14-character order token that identifies an order for the trading day.
//
// This engine mints tokens as 14-digit zero-padded decimal counters so a report
// maps back to an order with one integer parse. Tokens received from the wire may
// hold anything (the exchange echoes them verbatim), so parsing is strict and
// reports failure instead of guessing.
class Token {
public:
    using Raw = std::array<char, kTokenSize>;

    // Largest id that fits in 14 decimal digits.
    static constexpr std::uint64_t kMaxId = 99'999'999'999'999ULL;

    constexpr Token() noexcept = default;

    // 14-digit zero-padded decimal. An id above kMaxId cannot be represented; it
    // yields invalid(), which to_id() rejects and which the exchange refuses
    // ('#' is not an alphanumeric). The alternative, wrapping modulo 10^14,
    // would silently reuse a live token.
    static constexpr Token from_id(std::uint64_t id) noexcept {
        if (id > kMaxId) return invalid();
        Token t;
        for (std::size_t i = kTokenSize; i-- > 0;) {
            t.chars_[i] = static_cast<char>('0' + static_cast<int>(id % 10));
            id /= 10;
        }
        return t;
    }

    static constexpr Token invalid() noexcept {
        Token t;
        for (char& c : t.chars_) c = '#';
        return t;
    }

    // Left-justified text, space padded; anything past 14 characters is dropped.
    static constexpr Token from_text(std::string_view text) noexcept {
        Token t;
        for (std::size_t i = 0; i < kTokenSize && i < text.size(); ++i) t.chars_[i] = text[i];
        return t;
    }

    // Copies exactly kTokenSize bytes.
    static Token from_wire(const void* fourteen_bytes) noexcept {
        Token t;
        std::memcpy(t.chars_.data(), fourteen_bytes, kTokenSize);
        return t;
    }

    // Digits followed only by spaces. Leading spaces, embedded spaces, signs, any
    // other byte and the empty (all-space) token all give nullopt. Distinct tokens
    // can share an id ("42" and "00000000000042"), which is harmless because the
    // exchange echoes our token back byte for byte.
    constexpr std::optional<std::uint64_t> to_id() const noexcept {
        std::size_t digits = 0;
        std::uint64_t value = 0;  // at most 14 digits, so no overflow
        while (digits < kTokenSize && chars_[digits] >= '0' && chars_[digits] <= '9') {
            value = value * 10 + static_cast<std::uint64_t>(chars_[digits] - '0');
            ++digits;
        }
        if (digits == 0) return std::nullopt;
        for (std::size_t i = digits; i < kTokenSize; ++i) {
            if (chars_[i] != ' ') return std::nullopt;
        }
        return value;
    }

    constexpr const Raw& raw() const noexcept { return chars_; }

    // Text without trailing padding, for logs.
    constexpr std::string_view view() const noexcept {
        std::size_t n = kTokenSize;
        while (n > 0 && chars_[n - 1] == ' ') --n;
        return {chars_.data(), n};
    }

    friend constexpr bool operator==(const Token&, const Token&) = default;

private:
    Raw chars_ = detail::blank_token();
};

// Time in force is a number of seconds with three special values.
inline constexpr std::uint32_t kTifIoc = 0;
inline constexpr std::uint32_t kTifMarketHours = 99998;
inline constexpr std::uint32_t kTifSystemHours = 99999;

// ---------------------------------------------------------------------------
// Inbound (client -> exchange)
// ---------------------------------------------------------------------------

// Buy/sell indicator: 'B', 'S', 'T' (sell short) and 'E' (sell short exempt) map
// to `side` plus `short_sell`. The contract carries a single flag, so 'E' is read
// as a short sale and written back as 'T'; the exemption cannot be expressed.
// `short_sell` is ignored on a buy.
struct EnterOrder {
    Token token;
    Side side{};
    bool short_sell{};
    Qty shares{};
    Symbol stock;
    Price price{};
    std::uint32_t time_in_force{kTifSystemHours};
    char firm[4]{' ', ' ', ' ', ' '};
    char display{'Y'};
    char capacity{'P'};
    char intermarket_sweep{'N'};
    Qty min_qty{};
    char cross_type{'N'};
    char customer_type{' '};

    friend bool operator==(const EnterOrder&, const EnterOrder&) = default;
};

struct ReplaceOrder {
    Token existing;
    Token replacement;
    Qty shares{};  // total liable for the whole chain, executions included
    Price price{};
    std::uint32_t time_in_force{kTifSystemHours};
    char display{'Y'};
    char intermarket_sweep{'N'};
    Qty min_qty{};

    friend bool operator==(const ReplaceOrder&, const ReplaceOrder&) = default;
};

struct CancelOrder {
    Token token;
    Qty shares{};  // new intended size; 0 cancels everything still open

    friend bool operator==(const CancelOrder&, const CancelOrder&) = default;
};

// ---------------------------------------------------------------------------
// Outbound (exchange -> client). `ts` is nanoseconds since midnight, a full u64
// on the wire.
// ---------------------------------------------------------------------------

// Side is 'B'/'S' on the wire, or 'T'/'E' when the order was a short sale; the
// short forms are read as Side::sell and written back as 'S'.
struct Accepted {
    Nanos ts{};
    Token token;
    Side side{};
    Qty shares{};
    Symbol stock;
    Price price{};
    std::uint32_t time_in_force{};
    char firm[4]{};
    char display{};
    OrderRef ref{};
    char capacity{};
    char intermarket_sweep{};
    Qty min_qty{};
    char cross_type{};
    char order_state{'L'};  // 'L' live, 'D' accepted and immediately canceled
    char bbo_weight{' '};

    friend bool operator==(const Accepted&, const Accepted&) = default;
};

// `a.token` is the replacement token, `previous` the one it supersedes.
// `a.shares` is the open quantity after the replace, not the requested total.
struct Replaced {
    Accepted a;
    Token previous;

    friend bool operator==(const Replaced&, const Replaced&) = default;
};

struct Canceled {
    Nanos ts{};
    Token token;
    Qty decrement{};  // incremental, not cumulative
    char reason{'U'};

    friend bool operator==(const Canceled&, const Canceled&) = default;
};

struct Executed {
    Nanos ts{};
    Token token;
    Qty shares{};  // incremental
    Price price{};
    char liquidity{'A'};
    std::uint64_t match{};

    friend bool operator==(const Executed&, const Executed&) = default;
};

struct Rejected {
    Nanos ts{};
    Token token;
    char reason{'O'};

    friend bool operator==(const Rejected&, const Rejected&) = default;
};

// ---------------------------------------------------------------------------
// Wire lengths
// ---------------------------------------------------------------------------

inline constexpr std::size_t kEnterOrderLength = 49;
inline constexpr std::size_t kReplaceOrderLength = 47;
inline constexpr std::size_t kCancelOrderLength = 19;
inline constexpr std::size_t kAcceptedLength = 66;
inline constexpr std::size_t kReplacedLength = 80;
inline constexpr std::size_t kCanceledLength = 28;
inline constexpr std::size_t kExecutedLength = 40;
inline constexpr std::size_t kRejectedLength = 24;
inline constexpr std::size_t kMaxMessageLength = kReplacedLength;

// Wire length of an inbound message by its type byte, 0 if unsupported.
constexpr std::size_t inbound_length(char type) noexcept {
    switch (type) {
        case 'O': return kEnterOrderLength;
        case 'U': return kReplaceOrderLength;
        case 'X': return kCancelOrderLength;
        default: return 0;
    }
}

// Wire length of an outbound message by its type byte, 0 if unsupported. System
// event 'S', AIQ cancel 'D', broken trade 'B', reference-price execution 'G' and
// the other administrative messages are deliberately not decoded.
constexpr std::size_t outbound_length(char type) noexcept {
    switch (type) {
        case 'A': return kAcceptedLength;
        case 'U': return kReplacedLength;
        case 'C': return kCanceledLength;
        case 'E': return kExecutedLength;
        case 'J': return kRejectedLength;
        default: return 0;
    }
}

// Ignores every message. A handler that cares about a few types derives from it,
// writes `using NullHandler::on;` and adds overloads for those types.
struct NullHandler {
    template <class M>
    void on(const M&) noexcept {}
};

}  // namespace optitrade::ouch

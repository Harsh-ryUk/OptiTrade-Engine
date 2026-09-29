#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace optitrade {

// Prices are fixed point with four implied decimals, exactly like ITCH "Price(4)"
// and OUCH prices: 1'234'500 means 123.4500. Signed 64-bit so that price * qty
// arithmetic cannot overflow for any legal ITCH price (max 200'000.0000).
using Price = std::int64_t;
using Qty = std::uint32_t;
using OrderRef = std::uint64_t;  // ITCH order reference number (day unique)
using Locate = std::uint16_t;    // ITCH stock locate: per-day instrument index
using Nanos = std::uint64_t;     // nanoseconds since midnight (or capture start)

inline constexpr Price kPriceScale = 10'000;
inline constexpr Qty kMaxOrderQty = 999'999;  // OUCH: strictly below 1,000,000

enum class Side : std::uint8_t { buy = 0, sell = 1 };

constexpr Side opposite(Side s) noexcept {
    return s == Side::buy ? Side::sell : Side::buy;
}
constexpr std::size_t index(Side s) noexcept { return static_cast<std::size_t>(s); }

// Eight ASCII characters, right padded with spaces (the ITCH/OUCH "Stock" field).
class Symbol {
public:
    static constexpr std::size_t kSize = 8;

    constexpr Symbol() noexcept = default;

    constexpr explicit Symbol(std::string_view text) noexcept {
        for (std::size_t i = 0; i < kSize && i < text.size(); ++i) chars_[i] = text[i];
    }

    static Symbol from_wire(const void* eight_bytes) noexcept {
        Symbol s;
        std::memcpy(s.chars_.data(), eight_bytes, kSize);
        return s;
    }

    constexpr const std::array<char, kSize>& raw() const noexcept { return chars_; }

    // Text without the trailing padding.
    constexpr std::string_view view() const noexcept {
        std::size_t n = kSize;
        while (n > 0 && (chars_[n - 1] == ' ' || chars_[n - 1] == '\0')) --n;
        return {chars_.data(), n};
    }

    constexpr bool empty() const noexcept { return view().empty(); }

    friend constexpr bool operator==(const Symbol&, const Symbol&) = default;

private:
    std::array<char, kSize> chars_{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
};

}  // namespace optitrade

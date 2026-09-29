#pragma once

#include <cstddef>
#include <cstdint>

// Byte-order helpers. Both feeds we speak (ITCH, OUCH, MoldUDP64) are big-endian;
// the capture file format is little-endian. Compilers turn the shifts below into
// single loads/bswaps.
namespace optitrade::be {

inline std::uint16_t load16(const std::byte* p) noexcept {
    return static_cast<std::uint16_t>((std::uint16_t{std::to_integer<std::uint8_t>(p[0])} << 8) |
                                      std::to_integer<std::uint8_t>(p[1]));
}
inline std::uint32_t load32(const std::byte* p) noexcept {
    return (std::uint32_t{load16(p)} << 16) | load16(p + 2);
}
inline std::uint64_t load48(const std::byte* p) noexcept {
    return (std::uint64_t{load16(p)} << 32) | load32(p + 2);
}
inline std::uint64_t load64(const std::byte* p) noexcept {
    return (std::uint64_t{load32(p)} << 32) | load32(p + 4);
}

inline void store16(std::byte* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::byte>(v >> 8);
    p[1] = static_cast<std::byte>(v);
}
inline void store32(std::byte* p, std::uint32_t v) noexcept {
    store16(p, static_cast<std::uint16_t>(v >> 16));
    store16(p + 2, static_cast<std::uint16_t>(v));
}
inline void store48(std::byte* p, std::uint64_t v) noexcept {
    store16(p, static_cast<std::uint16_t>(v >> 32));
    store32(p + 2, static_cast<std::uint32_t>(v));
}
inline void store64(std::byte* p, std::uint64_t v) noexcept {
    store32(p, static_cast<std::uint32_t>(v >> 32));
    store32(p + 4, static_cast<std::uint32_t>(v));
}

}  // namespace optitrade::be

namespace optitrade::le {

inline std::uint16_t load16(const std::byte* p) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[0]) |
                                      (std::uint16_t{std::to_integer<std::uint8_t>(p[1])} << 8));
}
inline std::uint32_t load32(const std::byte* p) noexcept {
    return load16(p) | (std::uint32_t{load16(p + 2)} << 16);
}
inline std::uint64_t load64(const std::byte* p) noexcept {
    return load32(p) | (std::uint64_t{load32(p + 4)} << 32);
}

inline void store16(std::byte* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::byte>(v);
    p[1] = static_cast<std::byte>(v >> 8);
}
inline void store32(std::byte* p, std::uint32_t v) noexcept {
    store16(p, static_cast<std::uint16_t>(v));
    store16(p + 2, static_cast<std::uint16_t>(v >> 16));
}
inline void store64(std::byte* p, std::uint64_t v) noexcept {
    store32(p, static_cast<std::uint32_t>(v));
    store32(p + 4, static_cast<std::uint32_t>(v >> 32));
}

}  // namespace optitrade::le

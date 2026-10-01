#pragma once

#include <cstddef>
#include <cstdint>

namespace optitrade {

// FNV-1a, 64 bit. A cheap, portable fingerprint of a stream of events, used to
// prove that two runs (or two compilers) produced bit-identical results.
class Digest {
public:
    void update(const void* data, std::size_t n) noexcept {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i) {
            h_ ^= p[i];
            h_ *= 0x100000001B3ULL;
        }
    }
    // Feeds the value byte by byte, least significant first (endian independent).
    // Narrower and signed integers convert implicitly, i.e. they are zero/sign
    // extended to eight bytes, which is stable across platforms.
    void update(std::uint64_t v) noexcept {
        for (int i = 0; i < 8; ++i) {
            h_ ^= (v >> (8 * i)) & 0xFF;
            h_ *= 0x100000001B3ULL;
        }
    }
    std::uint64_t value() const noexcept { return h_; }

private:
    std::uint64_t h_{0xCBF29CE484222325ULL};
};

}  // namespace optitrade

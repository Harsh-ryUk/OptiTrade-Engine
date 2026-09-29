#pragma once

#include <cstdint>

namespace optitrade {

namespace detail {
// 64x64 -> 128 bit multiply is the one non-standard construct in the library; the
// __extension__ marker keeps -Wpedantic quiet on GCC (Clang accepts it silently).
__extension__ typedef unsigned __int128 u128;
}  // namespace detail

// xoshiro256** seeded through splitmix64. Used instead of <random> because the
// standard distributions are implementation-defined: identical seeds must give
// identical synthetic markets (and identical backtest digests) on every
// compiler and platform. Everything here is plain integer arithmetic.
//
// Not cryptographic, not thread safe. Copying an Rng snapshots its stream.
class Rng {
public:
    // splitmix64 expands the 64-bit seed into the 256-bit state. Its finaliser is a
    // bijection, so at most one of the four words can be zero: the all-zero state
    // (a fixed point of xoshiro) is unreachable for any seed, including 0.
    explicit Rng(std::uint64_t seed) noexcept {
        for (auto& w : s_) {
            seed += 0x9E3779B97F4A7C15ULL;
            std::uint64_t z = seed;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            w = z ^ (z >> 31);
        }
    }

    std::uint64_t next() noexcept {
        const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 45);
        return result;
    }

    // Uniform in [0, n). Lemire's multiply-shift with rejection: unlike `next() % n`
    // there is no modulo bias, and the division is only paid on the rare slow path.
    // n must be > 0 (n == 0 returns 0 without consuming more than one draw).
    std::uint64_t bounded(std::uint64_t n) noexcept {
        detail::u128 m = static_cast<detail::u128>(next()) * n;
        auto low = static_cast<std::uint64_t>(m);
        if (low < n) {
            const std::uint64_t threshold = (0 - n) % n;
            while (low < threshold) {
                m = static_cast<detail::u128>(next()) * n;
                low = static_cast<std::uint64_t>(m);
            }
        }
        return static_cast<std::uint64_t>(m >> 64);
    }

    // Uniform in [lo, hi] inclusive; requires lo <= hi. The span is computed in
    // unsigned arithmetic because `hi - lo` overflows int64_t for wide ranges, and
    // the full 64-bit range (span + 1 == 2^64) is served by a raw draw.
    std::int64_t range(std::int64_t lo, std::int64_t hi) noexcept {
        const auto ulo = static_cast<std::uint64_t>(lo);
        const std::uint64_t span = static_cast<std::uint64_t>(hi) - ulo;
        const std::uint64_t offset = span == UINT64_MAX ? next() : bounded(span + 1);
        return static_cast<std::int64_t>(ulo + offset);
    }

    // True with probability num/den (den > 0). num >= den is always true.
    bool chance(std::uint64_t num, std::uint64_t den) noexcept { return bounded(den) < num; }

    // Number of failures before the first success of a Bernoulli(num/den) trial,
    // capped at `cap` so that a tiny success probability cannot loop for long.
    // Mean is roughly (den-num)/num when the cap is not binding.
    std::uint32_t geometric(std::uint64_t num, std::uint64_t den, std::uint32_t cap) noexcept {
        std::uint32_t n = 0;
        while (n < cap && !chance(num, den)) ++n;
        return n;
    }

private:
    static constexpr std::uint64_t rotl(std::uint64_t x, int k) noexcept {
        return (x << k) | (x >> (64 - k));
    }
    std::uint64_t s_[4];
};

}  // namespace optitrade

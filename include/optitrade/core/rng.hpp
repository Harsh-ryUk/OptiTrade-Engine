#pragma once

#include <cstdint>

namespace optitrade {

// xoshiro256** seeded through splitmix64. Used instead of <random> because the
// standard distributions are implementation-defined: identical seeds must give
// identical synthetic markets (and identical backtest digests) on every
// compiler and platform. Everything here is plain integer arithmetic.
class Rng {
public:
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

    // Uniform in [0, n). n must be > 0. (Lemire's multiply-shift, unbiased.)
    std::uint64_t bounded(std::uint64_t n) noexcept {
        unsigned __int128 m = static_cast<unsigned __int128>(next()) * n;
        auto low = static_cast<std::uint64_t>(m);
        if (low < n) {
            const std::uint64_t threshold = (0 - n) % n;
            while (low < threshold) {
                m = static_cast<unsigned __int128>(next()) * n;
                low = static_cast<std::uint64_t>(m);
            }
        }
        return static_cast<std::uint64_t>(m >> 64);
    }

    // Uniform in [lo, hi] inclusive.
    std::int64_t range(std::int64_t lo, std::int64_t hi) noexcept {
        return lo + static_cast<std::int64_t>(bounded(static_cast<std::uint64_t>(hi - lo) + 1));
    }

    // True with probability num/den.
    bool chance(std::uint64_t num, std::uint64_t den) noexcept { return bounded(den) < num; }

    // Number of failures before the first success of a Bernoulli(num/den) trial,
    // capped at `cap`. Mean is roughly (den-num)/num.
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

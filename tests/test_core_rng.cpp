// Rng: known-answer vectors, comparison with independent reference implementations
// of splitmix64 / xoshiro256** / Lemire's bounded draw, and statistical sanity of the
// derived helpers. Everything is seeded, so a passing run always passes.

#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <vector>

#include "check.hpp"
#include "optitrade/core/rng.hpp"

using namespace optitrade;

namespace {

// ---------------------------------------------------------------------------
// Reference implementations, written from the published descriptions
// (Vigna, "splitmix64.c" and "xoshiro256starstar.c"; Lemire, "Fast Random Integer
// Generation in an Interval") and structured differently from the production code:
// explicit state array, named helper for rotation, and a 64x64->128 product built
// from 32-bit limbs instead of a compiler 128-bit type.
// ---------------------------------------------------------------------------

class RefSplitMix64 {
public:
    explicit RefSplitMix64(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

private:
    std::uint64_t state_;
};

std::uint64_t rol64(std::uint64_t v, unsigned n) { return (v << n) | (v >> (64U - n)); }

class RefXoshiro256ss {
public:
    explicit RefXoshiro256ss(const std::array<std::uint64_t, 4>& state) : s_(state) {}
    std::uint64_t next() {
        const std::uint64_t result = rol64(s_[1] * 5, 7) * 9;
        const std::uint64_t t = s_[1] << 17;
        s_[2] = s_[2] ^ s_[0];
        s_[3] = s_[3] ^ s_[1];
        s_[1] = s_[1] ^ s_[2];
        s_[0] = s_[0] ^ s_[3];
        s_[2] = s_[2] ^ t;
        s_[3] = rol64(s_[3], 45);
        return result;
    }

private:
    std::array<std::uint64_t, 4> s_;
};

// Seeding as documented: four consecutive splitmix64 outputs.
RefXoshiro256ss reference_generator(std::uint64_t seed) {
    RefSplitMix64 sm(seed);
    std::array<std::uint64_t, 4> st{};
    for (auto& w : st) w = sm.next();
    return RefXoshiro256ss(st);
}

// High 64 bits of a*b from 32-bit limbs.
std::uint64_t mul_high(std::uint64_t a, std::uint64_t b) {
    const std::uint64_t a0 = a & 0xFFFFFFFFULL, a1 = a >> 32;
    const std::uint64_t b0 = b & 0xFFFFFFFFULL, b1 = b >> 32;
    const std::uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const std::uint64_t carry = ((p00 >> 32) + (p01 & 0xFFFFFFFFULL) + (p10 & 0xFFFFFFFFULL)) >> 32;
    return p11 + (p01 >> 32) + (p10 >> 32) + carry;
}

// Lemire's nearly-divisionless method, exactly as published.
std::uint64_t reference_bounded(RefXoshiro256ss& g, std::uint64_t n) {
    std::uint64_t x = g.next();
    std::uint64_t low = x * n;
    std::uint64_t high = mul_high(x, n);
    if (low < n) {
        const std::uint64_t threshold = (~n + 1) % n;  // 2^64 mod n
        while (low < threshold) {
            x = g.next();
            low = x * n;
            high = mul_high(x, n);
        }
    }
    return high;
}

}  // namespace

// ---------------------------------------------------------------------------
// The reference implementations themselves are checked against published vectors
// ---------------------------------------------------------------------------

OT_TEST(reference_splitmix64_matches_published_vector) {
    // Seed 0: the widely quoted output of Vigna's splitmix64.c.
    RefSplitMix64 sm(0);
    OT_CHECK_EQ(sm.next(), std::uint64_t{0xE220A8397B1DCDAFULL});
    OT_CHECK_EQ(sm.next(), std::uint64_t{0x6E789E6AA1B965F4ULL});
    OT_CHECK_EQ(sm.next(), std::uint64_t{0x06C45D188009454FULL});
    OT_CHECK_EQ(sm.next(), std::uint64_t{0xF88BB8A8724C81ECULL});
}

OT_TEST(reference_xoshiro_matches_published_vector) {
    // State {1,2,3,4}: the reference outputs used by xoshiro test suites.
    RefXoshiro256ss g({1, 2, 3, 4});
    const std::uint64_t expected[] = {11520ULL,
                                      0ULL,
                                      1509978240ULL,
                                      1215971899390074240ULL,
                                      1216172134540287360ULL,
                                      607988272756665600ULL,
                                      16172922978634559625ULL,
                                      8476171486693032832ULL,
                                      10595114339597558777ULL,
                                      2904607092377533576ULL};
    for (std::uint64_t e : expected) OT_CHECK_EQ(g.next(), e);
}

OT_TEST(reference_mul_high_is_correct) {
    OT_CHECK_EQ(mul_high(0, 12345), std::uint64_t{0});
    OT_CHECK_EQ(mul_high(1ULL << 63, 2), std::uint64_t{1});
    OT_CHECK_EQ(mul_high(~0ULL, ~0ULL), ~0ULL - 1);  // (2^64-1)^2 = 2^128 - 2^65 + 1
    OT_CHECK_EQ(mul_high(0xFFFFFFFFULL, 0xFFFFFFFFULL), std::uint64_t{0});
    OT_CHECK_EQ(mul_high(1ULL << 32, 1ULL << 32), std::uint64_t{1});
    OT_CHECK_EQ(mul_high(0x123456789ABCDEF0ULL, 0x0FEDCBA987654321ULL), std::uint64_t{0x0121FA00AD77D742ULL});
}

// ---------------------------------------------------------------------------
// Rng against known answers (computed by an independent Python implementation)
// ---------------------------------------------------------------------------

OT_TEST(first_outputs_for_fixed_seeds) {
    struct Vec {
        std::uint64_t seed;
        std::uint64_t out[6];
    };
    const Vec vectors[] = {
        {0, {0x99EC5F36CB75F2B4ULL, 0xBF6E1F784956452AULL, 0x1A5F849D4933E6E0ULL, 0x6AA594F1262D2D2CULL,
             0xBBA5AD4A1F842E59ULL, 0xFFEF8375D9EBCACAULL}},
        {1, {0xB3F2AF6D0FC710C5ULL, 0x853B559647364CEAULL, 0x92F89756082A4514ULL, 0x642E1C7BC266A3A7ULL,
             0xB27A48E29A233673ULL, 0x24C123126FFDA722ULL}},
        {42, {0x15780B2E0C2EC716ULL, 0x6104D9866D113A7EULL, 0xAE17533239E499A1ULL, 0xECB8AD4703B360A1ULL,
              0xFDE6DC7FE2EC5E64ULL, 0xC50DA53101795238ULL}},
        {0xDEADBEEFCAFEF00DULL, {0x9E32CFB5BB93EEBBULL, 0x16006BD9D4AC0014ULL, 0x8ADA5D6D34B6538EULL,
                                 0x7C327CA32346A238ULL, 0xC43A6D6A3492CED2ULL, 0xDB639ECB036A9C04ULL}},
        {0xFFFFFFFFFFFFFFFFULL, {0x8F5520D52A7EAD08ULL, 0xC476A018CAA1802DULL, 0x81DE31C0D260469EULL,
                                 0xBF658D7E065F3C2FULL, 0x913593FDA1BCA32AULL, 0xBB535E93941BA525ULL}},
    };
    for (const Vec& v : vectors) {
        Rng rng(v.seed);
        for (std::uint64_t expected : v.out) OT_CHECK_EQ(rng.next(), expected);
    }
}

OT_TEST(long_range_checkpoints_for_seed_42) {
    Rng rng(42);
    std::uint64_t fold = 0;
    for (int i = 0; i < 10000; ++i) {
        const std::uint64_t v = rng.next();
        fold ^= v;
        if (i == 999) OT_CHECK_EQ(v, std::uint64_t{0x8DE5848C61AB8968ULL});
        if (i == 9999) OT_CHECK_EQ(v, std::uint64_t{0xEED6344DF08981A9ULL});
    }
    OT_CHECK_EQ(fold, std::uint64_t{0xEE5557930F3024BBULL});
}

// The whole helper API in one fixed call sequence; the final next() proves that
// every call consumed exactly as many raw draws as the reference implementation.
OT_TEST(helper_api_golden_sequence) {
    Rng rng(7);
    const std::uint64_t b10[] = {7, 2, 8, 9, 9, 8, 0, 1};
    for (std::uint64_t e : b10) OT_CHECK_EQ(rng.bounded(10), e);
    const std::uint64_t b1000[] = {403, 151, 541, 731, 938, 880, 451, 560};
    for (std::uint64_t e : b1000) OT_CHECK_EQ(rng.bounded(1000), e);
    const std::uint64_t big[] = {0x20DB7ACCF9ED2EE0ULL, 0x556F1B4EA7A6F4A1ULL, 0x5FF334ABF1FBE0B8ULL, 0x107B5421F8516AB0ULL};
    for (std::uint64_t e : big) OT_CHECK_EQ(rng.bounded((1ULL << 63) + 5), e);
    const std::uint64_t awkward[] = {0x06FBB1A4F15EB297ULL, 0x64C63E44704E64EFULL, 0x51E0155BF67B62E8ULL, 0x0EF2279B5F01D942ULL};
    for (std::uint64_t e : awkward) OT_CHECK_EQ(rng.bounded(0xAAAAAAAAAAAAAAABULL), e);
    const std::int64_t r5[] = {-4, 4, 5, 5, -1, -2, 2, 5};
    for (std::int64_t e : r5) OT_CHECK_EQ(rng.range(-5, 5), e);
    const std::int64_t full[] = {INT64_C(8914754166513841274), INT64_C(7138823683104134724), INT64_C(-5244029508021173654)};
    for (std::int64_t e : full) OT_CHECK_EQ(rng.range(INT64_MIN, INT64_MAX), e);
    const bool c13[] = {false, true, false, true, false, false, false, false, false, false};
    for (bool e : c13) OT_CHECK_EQ(rng.chance(1, 3), e);
    const std::uint32_t g14[] = {4, 3, 6, 2, 9, 0, 5, 1, 1, 0};
    for (std::uint32_t e : g14) OT_CHECK_EQ(rng.geometric(1, 4, 100), e);
    for (int i = 0; i < 4; ++i) OT_CHECK_EQ(rng.geometric(1, 1000, 7), std::uint32_t{7});
    OT_CHECK_EQ(rng.next(), std::uint64_t{0xAA92FA7CBF1CE457ULL});
}

// ---------------------------------------------------------------------------
// Differential comparison with the reference implementations
// ---------------------------------------------------------------------------

OT_TEST(next_matches_reference_for_many_seeds) {
    for (std::uint64_t seed : {0ULL, 1ULL, 2ULL, 3ULL, 42ULL, 1234567ULL, 0x8000000000000000ULL, ~0ULL}) {
        Rng rng(seed);
        RefXoshiro256ss ref = reference_generator(seed);
        for (int i = 0; i < 20000; ++i) {
            const std::uint64_t a = rng.next();
            const std::uint64_t b = ref.next();
            if (a != b) {
                OT_CHECK_EQ(a, b);
                return;
            }
        }
    }
}

// Mixed bounds, including ones that force the rejection loop. Comparing the next
// raw draw afterwards proves both consumed the same number of values.
OT_TEST(bounded_matches_reference_and_consumes_same_draws) {
    Rng rng(555);
    RefXoshiro256ss ref = reference_generator(555);
    Rng chooser(777);
    const std::uint64_t fixed_bounds[] = {1, 2, 3, 5, 6, 7, 10, 100, 1000, 65537, 1ULL << 32, (1ULL << 32) + 1,
                                          (1ULL << 63) - 1, 1ULL << 63, (1ULL << 63) + 1, 0xAAAAAAAAAAAAAAABULL,
                                          0xFFFFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFFFFFULL};
    for (int i = 0; i < 100000; ++i) {
        std::uint64_t n;
        if (i % 3 == 0) n = fixed_bounds[chooser.bounded(std::size(fixed_bounds))];
        else if (i % 3 == 1) n = chooser.next() | 1;
        else n = 1 + chooser.bounded(1000);
        const std::uint64_t a = rng.bounded(n);
        const std::uint64_t b = reference_bounded(ref, n);
        if (a != b || a >= n) {
            OT_CHECK_EQ(a, b);
            OT_CHECK(a < n);
            return;
        }
    }
    OT_CHECK_EQ(rng.next(), ref.next());
}

// Power-of-two bounds never reject, so the result is simply the top bits.
OT_TEST(bounded_power_of_two_takes_top_bits) {
    for (unsigned k = 0; k < 64; ++k) {
        Rng a(k + 1), b(k + 1);
        for (int i = 0; i < 100; ++i) {
            const std::uint64_t raw = b.next();
            const std::uint64_t expected = (k == 0) ? 0 : raw >> (64 - k);
            OT_CHECK_EQ(a.bounded(1ULL << k), expected);
        }
    }
}

OT_TEST(bounded_of_one_is_zero_and_costs_one_draw) {
    Rng a(9), b(9);
    for (int i = 0; i < 1000; ++i) {
        OT_CHECK_EQ(a.bounded(1), std::uint64_t{0});
        b.next();
    }
    OT_CHECK_EQ(a.next(), b.next());
}

// ---------------------------------------------------------------------------
// Determinism and independence
// ---------------------------------------------------------------------------

OT_TEST(same_seed_same_stream) {
    Rng a(2024), b(2024);
    for (int i = 0; i < 50000; ++i) OT_CHECK_EQ(a.next(), b.next());
}

OT_TEST(different_seeds_give_different_streams) {
    std::vector<std::uint64_t> firsts;
    for (std::uint64_t seed = 0; seed < 1000; ++seed) firsts.push_back(Rng(seed).next());
    for (std::size_t i = 0; i < firsts.size(); ++i)
        for (std::size_t j = i + 1; j < firsts.size(); ++j)
            if (firsts[i] == firsts[j]) {
                OT_CHECK(false);
                return;
            }
}

OT_TEST(copy_snapshots_the_stream) {
    Rng a(31337);
    for (int i = 0; i < 17; ++i) a.next();
    Rng b = a;  // copy mid-stream
    std::vector<std::uint64_t> from_a;
    for (int i = 0; i < 100; ++i) from_a.push_back(a.next());
    for (int i = 0; i < 100; ++i) OT_CHECK_EQ(b.next(), from_a[static_cast<std::size_t>(i)]);
}

OT_TEST(seed_zero_is_not_degenerate) {
    Rng rng(0);
    std::uint64_t ored = 0;
    std::uint64_t prev = rng.next();
    OT_CHECK(prev != 0);
    for (int i = 0; i < 1000; ++i) {
        const std::uint64_t v = rng.next();
        OT_CHECK(v != prev);
        ored |= v;
        prev = v;
    }
    OT_CHECK_EQ(ored, ~std::uint64_t{0});
}

OT_TEST(every_output_bit_is_balanced) {
    Rng rng(20240229);
    constexpr int kDraws = 200000;
    std::array<int, 64> ones{};
    for (int i = 0; i < kDraws; ++i) {
        const std::uint64_t v = rng.next();
        for (unsigned bit = 0; bit < 64; ++bit) ones[bit] += static_cast<int>((v >> bit) & 1);
    }
    // sigma = sqrt(n)/2 ~ 224; allow ~5 sigma.
    for (unsigned bit = 0; bit < 64; ++bit) {
        OT_CHECK(ones[bit] > kDraws / 2 - 1100);
        OT_CHECK(ones[bit] < kDraws / 2 + 1100);
    }
}

// ---------------------------------------------------------------------------
// Uniformity
// ---------------------------------------------------------------------------

OT_TEST(bounded_is_uniform_over_small_ranges) {
    for (std::uint64_t n : {2ULL, 3ULL, 7ULL, 10ULL, 16ULL, 100ULL}) {
        Rng rng(n * 1000 + 1);
        std::vector<int> count(n, 0);
        constexpr int kDraws = 1'000'000;
        for (int i = 0; i < kDraws; ++i) {
            const std::uint64_t v = rng.bounded(n);
            OT_CHECK(v < n);
            ++count[v];
        }
        const double expected = static_cast<double>(kDraws) / static_cast<double>(n);
        const double sigma = std::sqrt(expected * (1.0 - 1.0 / static_cast<double>(n)));
        for (std::uint64_t v = 0; v < n; ++v) {
            const double dev = std::fabs(static_cast<double>(count[v]) - expected);
            OT_CHECK(dev < 5.0 * sigma);
        }
    }
}

// n just above 2/3 of the 64-bit range: `next() % n` would hit the lower half of
// the range twice as often (2/3 vs 1/2), the rejection step removes that bias.
OT_TEST(bounded_has_no_modulo_bias) {
    Rng rng(99);
    const std::uint64_t n = 0xAAAAAAAAAAAAAAABULL;
    constexpr int kDraws = 2'000'000;
    int lower = 0;
    for (int i = 0; i < kDraws; ++i)
        if (rng.bounded(n) < n / 2) ++lower;
    const double fraction = static_cast<double>(lower) / kDraws;
    OT_CHECK(fraction > 0.497 && fraction < 0.503);
}

// ---------------------------------------------------------------------------
// range()
// ---------------------------------------------------------------------------

OT_TEST(range_covers_both_inclusive_endpoints_only) {
    Rng rng(5);
    std::array<int, 7> hits{};
    for (int i = 0; i < 70000; ++i) {
        const std::int64_t v = rng.range(-3, 3);
        OT_CHECK(v >= -3 && v <= 3);
        if (v >= -3 && v <= 3) ++hits[static_cast<std::size_t>(v + 3)];
    }
    for (int h : hits) OT_CHECK(h > 9000 && h < 11000);  // 10000 +- ~10 sigma
}

OT_TEST(range_degenerate_and_adjacent) {
    Rng rng(6);
    for (int i = 0; i < 100; ++i) OT_CHECK_EQ(rng.range(5, 5), std::int64_t{5});
    for (int i = 0; i < 100; ++i) OT_CHECK_EQ(rng.range(INT64_MIN, INT64_MIN), INT64_MIN);
    for (int i = 0; i < 100; ++i) OT_CHECK_EQ(rng.range(INT64_MAX, INT64_MAX), INT64_MAX);
    bool saw_lo = false, saw_hi = false;
    for (int i = 0; i < 200; ++i) {
        const std::int64_t v = rng.range(-1, 0);
        OT_CHECK(v == -1 || v == 0);
        saw_lo |= (v == -1);
        saw_hi |= (v == 0);
    }
    OT_CHECK(saw_lo && saw_hi);
}

// hi - lo overflows a signed 64-bit integer in all of these; UBSan would flag a
// signed subtraction, and results must still stay inside [lo, hi].
OT_TEST(range_handles_spans_wider_than_int64) {
    struct Span {
        std::int64_t lo, hi;
    };
    const Span spans[] = {{INT64_MIN, INT64_MAX}, {INT64_MIN, 0}, {-1, INT64_MAX}, {INT64_MIN, INT64_MAX - 1},
                          {INT64_MIN + 1, INT64_MAX}, {INT64_MAX - 1, INT64_MAX}, {INT64_MIN, INT64_MIN + 1},
                          {-(INT64_C(1) << 62), (INT64_C(1) << 62)}};
    Rng rng(8);
    for (const Span& s : spans) {
        // Offsets are measured in unsigned arithmetic: they exceed INT64_MAX.
        const std::uint64_t width = static_cast<std::uint64_t>(s.hi) - static_cast<std::uint64_t>(s.lo);
        bool low_half = false, high_half = false;
        for (int i = 0; i < 5000; ++i) {
            const std::int64_t v = rng.range(s.lo, s.hi);
            if (v < s.lo || v > s.hi) {
                OT_CHECK(v >= s.lo && v <= s.hi);
                return;
            }
            const std::uint64_t offset = static_cast<std::uint64_t>(v) - static_cast<std::uint64_t>(s.lo);
            low_half |= offset <= width / 2;
            high_half |= offset > width / 2;
        }
        OT_CHECK(low_half && high_half);
    }
}

// The full domain has 2^64 values, which no bounded() call can express: the result
// is lo plus a raw draw, i.e. the draw with its top bit flipped when lo == INT64_MIN.
OT_TEST(range_over_the_full_domain_offsets_a_raw_draw) {
    Rng a(77), b(77);
    for (int i = 0; i < 1000; ++i)
        OT_CHECK_EQ(a.range(INT64_MIN, INT64_MAX), static_cast<std::int64_t>(b.next() ^ (1ULL << 63)));
}

OT_TEST(range_matches_reference_bounded) {
    Rng rng(4321);
    RefXoshiro256ss ref = reference_generator(4321);
    Rng chooser(1);
    for (int i = 0; i < 50000; ++i) {
        const std::int64_t lo = chooser.range(-1'000'000'000, 1'000'000'000);
        const std::int64_t hi = lo + chooser.range(0, 2'000'000);
        const std::int64_t expected =
            static_cast<std::int64_t>(static_cast<std::uint64_t>(lo) +
                                      reference_bounded(ref, static_cast<std::uint64_t>(hi - lo) + 1));
        if (rng.range(lo, hi) != expected) {
            OT_CHECK(false);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// chance() and geometric()
// ---------------------------------------------------------------------------

OT_TEST(chance_extremes) {
    Rng rng(10);
    for (int i = 0; i < 10000; ++i) {
        OT_CHECK(!rng.chance(0, 1));
        OT_CHECK(!rng.chance(0, 1000));
        OT_CHECK(rng.chance(1, 1));
        OT_CHECK(rng.chance(1000, 1000));
        OT_CHECK(rng.chance(5, 3));  // num > den: certain
    }
}

OT_TEST(chance_frequency_matches_probability) {
    struct Case {
        std::uint64_t num, den;
    };
    for (const Case& c : {Case{1, 2}, Case{1, 10}, Case{3, 7}, Case{99, 100}, Case{1, 1000}}) {
        Rng rng(c.num * 131 + c.den);
        constexpr int kDraws = 1'000'000;
        int hits = 0;
        for (int i = 0; i < kDraws; ++i) hits += rng.chance(c.num, c.den) ? 1 : 0;
        const double p = static_cast<double>(c.num) / static_cast<double>(c.den);
        const double sigma = std::sqrt(kDraws * p * (1 - p));
        OT_CHECK(std::fabs(hits - kDraws * p) < 5 * sigma);
    }
}

OT_TEST(geometric_respects_the_cap) {
    Rng rng(11);
    for (int i = 0; i < 1000; ++i) {
        OT_CHECK_EQ(rng.geometric(0, 1, 5), std::uint32_t{5});          // can never succeed
        OT_CHECK_EQ(rng.geometric(1, 1'000'000'000, 9), std::uint32_t{9});
        OT_CHECK_EQ(rng.geometric(1, 1, 5), std::uint32_t{0});          // always succeeds first try
        OT_CHECK(rng.geometric(1, 3, 4) <= 4);
        OT_CHECK(rng.geometric(1, 100, 1) <= 1);
    }
}

OT_TEST(geometric_with_zero_cap_draws_nothing) {
    Rng a(12), b(12);
    for (int i = 0; i < 100; ++i) OT_CHECK_EQ(a.geometric(1, 2, 0), std::uint32_t{0});
    OT_CHECK_EQ(a.next(), b.next());
}

OT_TEST(geometric_distribution) {
    // Uncapped enough: failures before first success of p = 1/4 -> mean 3, P(0) = 1/4.
    {
        Rng rng(13);
        constexpr int kDraws = 400000;
        std::uint64_t sum = 0;
        int zeros = 0;
        for (int i = 0; i < kDraws; ++i) {
            const std::uint32_t g = rng.geometric(1, 4, 1000);
            sum += g;
            zeros += (g == 0);
        }
        const double mean = static_cast<double>(sum) / kDraws;
        OT_CHECK(mean > 2.97 && mean < 3.03);  // sd of the mean ~ 0.0037
        const double p0 = static_cast<double>(zeros) / kDraws;
        OT_CHECK(p0 > 0.245 && p0 < 0.255);
    }
    // Capped: P(result == cap) = (1-p)^cap.
    {
        Rng rng(14);
        constexpr int kDraws = 400000;
        int at_cap = 0;
        for (int i = 0; i < kDraws; ++i) at_cap += (rng.geometric(1, 10, 4) == 4);
        const double frac = static_cast<double>(at_cap) / kDraws;  // 0.9^4 = 0.6561
        OT_CHECK(frac > 0.650 && frac < 0.662);
    }
}

OT_TEST_MAIN()

// Byte-order helpers: exact byte layouts, sign/zero extension, unaligned addresses,
// write extent (guard bytes), and randomized round trips against an oracle built
// from plain loops rather than the shift ladders used by the header.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <vector>

#include "check.hpp"
#include "optitrade/core/endian.hpp"
#include "optitrade/core/rng.hpp"

using namespace optitrade;

namespace {

constexpr std::byte B(unsigned v) { return static_cast<std::byte>(v); }

// Oracle: the `n` low bytes of v, most significant first.
std::vector<std::byte> big_endian_bytes(std::uint64_t v, unsigned n) {
    std::vector<std::byte> out(n);
    for (unsigned i = 0; i < n; ++i) out[i] = static_cast<std::byte>((v >> (8 * (n - 1 - i))) & 0xFF);
    return out;
}

std::vector<std::byte> little_endian_bytes(std::uint64_t v, unsigned n) {
    std::vector<std::byte> out(n);
    for (unsigned i = 0; i < n; ++i) out[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    return out;
}

std::uint64_t compose_big_endian(const std::byte* p, unsigned n) {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v = v * 256 + std::to_integer<unsigned>(p[i]);
    return v;
}

std::uint64_t compose_little_endian(const std::byte* p, unsigned n) {
    std::uint64_t v = 0;
    for (unsigned i = n; i-- > 0;) v = v * 256 + std::to_integer<unsigned>(p[i]);
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// Hand-written layouts
// ---------------------------------------------------------------------------

OT_TEST(big_endian_known_layouts) {
    const std::byte b16[] = {B(0x12), B(0x34)};
    OT_CHECK_EQ(be::load16(b16), std::uint16_t{0x1234});
    const std::byte b32[] = {B(0x12), B(0x34), B(0x56), B(0x78)};
    OT_CHECK_EQ(be::load32(b32), std::uint32_t{0x12345678});
    const std::byte b48[] = {B(0x01), B(0x02), B(0x03), B(0x04), B(0x05), B(0x06)};
    OT_CHECK_EQ(be::load48(b48), std::uint64_t{0x010203040506ULL});
    const std::byte b64[] = {B(0x01), B(0x23), B(0x45), B(0x67), B(0x89), B(0xAB), B(0xCD), B(0xEF)};
    OT_CHECK_EQ(be::load64(b64), std::uint64_t{0x0123456789ABCDEFULL});
}

OT_TEST(little_endian_known_layouts) {
    const std::byte b16[] = {B(0x34), B(0x12)};
    OT_CHECK_EQ(le::load16(b16), std::uint16_t{0x1234});
    const std::byte b32[] = {B(0x78), B(0x56), B(0x34), B(0x12)};
    OT_CHECK_EQ(le::load32(b32), std::uint32_t{0x12345678});
    const std::byte b64[] = {B(0xEF), B(0xCD), B(0xAB), B(0x89), B(0x67), B(0x45), B(0x23), B(0x01)};
    OT_CHECK_EQ(le::load64(b64), std::uint64_t{0x0123456789ABCDEFULL});
}

OT_TEST(big_endian_stores_write_the_documented_bytes) {
    std::byte buf[8];
    be::store16(buf, 0xABCD);
    OT_CHECK(buf[0] == B(0xAB) && buf[1] == B(0xCD));
    be::store32(buf, 0x01020304);
    OT_CHECK(buf[0] == B(1) && buf[1] == B(2) && buf[2] == B(3) && buf[3] == B(4));
    be::store48(buf, 0x0A0B0C0D0E0FULL);
    const std::byte want48[] = {B(0x0A), B(0x0B), B(0x0C), B(0x0D), B(0x0E), B(0x0F)};
    OT_CHECK(std::equal(std::begin(want48), std::end(want48), buf));
    be::store64(buf, 0x1122334455667788ULL);
    const std::byte want64[] = {B(0x11), B(0x22), B(0x33), B(0x44), B(0x55), B(0x66), B(0x77), B(0x88)};
    OT_CHECK(std::equal(std::begin(want64), std::end(want64), buf));
}

OT_TEST(little_endian_stores_write_the_documented_bytes) {
    std::byte buf[8];
    le::store16(buf, 0xABCD);
    OT_CHECK(buf[0] == B(0xCD) && buf[1] == B(0xAB));
    le::store32(buf, 0x01020304);
    OT_CHECK(buf[0] == B(4) && buf[1] == B(3) && buf[2] == B(2) && buf[3] == B(1));
    le::store64(buf, 0x1122334455667788ULL);
    const std::byte want[] = {B(0x88), B(0x77), B(0x66), B(0x55), B(0x44), B(0x33), B(0x22), B(0x11)};
    OT_CHECK(std::equal(std::begin(want), std::end(want), buf));
}

// ITCH timestamps: nanoseconds since midnight in six big-endian bytes.
OT_TEST(itch_timestamp_examples) {
    // 09:30:00.000000000 = 34'200'000'000'000 ns
    const std::byte open[] = {B(0x1F), B(0x1A), B(0xCE), B(0xD9), B(0xF0), B(0x00)};
    OT_CHECK_EQ(be::load48(open), std::uint64_t{34'200'000'000'000ULL});
    std::byte out[6];
    be::store48(out, 34'200'000'000'000ULL);
    OT_CHECK(std::equal(std::begin(open), std::end(open), out));
    // 23:59:59.999999999
    const std::byte last[] = {B(0x4E), B(0x94), B(0x91), B(0x4E), B(0xFF), B(0xFF)};
    OT_CHECK_EQ(be::load48(last), std::uint64_t{86'399'999'999'999ULL});
}

// ---------------------------------------------------------------------------
// Extension, truncation and extent
// ---------------------------------------------------------------------------

OT_TEST(loads_zero_extend_high_bit_bytes) {
    const std::byte hi16[] = {B(0xFF), B(0xFE)};
    OT_CHECK_EQ(be::load16(hi16), std::uint16_t{0xFFFE});
    OT_CHECK_EQ(le::load16(hi16), std::uint16_t{0xFEFF});
    const std::byte hi32[] = {B(0x80), B(0), B(0), B(0)};
    OT_CHECK_EQ(be::load32(hi32), std::uint32_t{0x80000000u});
    OT_CHECK_EQ(le::load32(hi32), std::uint32_t{0x80u});
    const std::byte ones[] = {B(0xFF), B(0xFF), B(0xFF), B(0xFF), B(0xFF), B(0xFF), B(0xFF), B(0xFF)};
    OT_CHECK_EQ(be::load48(ones), std::uint64_t{0xFFFFFFFFFFFFULL});  // not sign extended into bits 48..63
    OT_CHECK_EQ(be::load64(ones), ~std::uint64_t{0});
    OT_CHECK_EQ(le::load64(ones), ~std::uint64_t{0});
    const std::byte top[] = {B(0x80), B(0), B(0), B(0), B(0), B(0), B(0), B(0)};
    OT_CHECK_EQ(be::load64(top), std::uint64_t{0x8000000000000000ULL});
}

OT_TEST(store48_truncates_to_the_low_48_bits) {
    std::byte buf[6];
    be::store48(buf, 0xFFFF010203040506ULL);
    OT_CHECK_EQ(be::load48(buf), std::uint64_t{0x010203040506ULL});
    be::store48(buf, ~std::uint64_t{0});
    OT_CHECK_EQ(be::load48(buf), std::uint64_t{0xFFFFFFFFFFFFULL});
}

OT_TEST(stores_write_exactly_their_width) {
    for (unsigned width : {2u, 4u, 6u, 8u}) {
        std::array<std::byte, 16> buf;
        buf.fill(B(0xAA));
        constexpr unsigned kOffset = 4;
        switch (width) {
            case 2: be::store16(buf.data() + kOffset, 0x0000); break;
            case 4: be::store32(buf.data() + kOffset, 0); break;
            case 6: be::store48(buf.data() + kOffset, 0); break;
            default: be::store64(buf.data() + kOffset, 0); break;
        }
        for (unsigned i = 0; i < buf.size(); ++i) {
            const bool inside = i >= kOffset && i < kOffset + width;
            OT_CHECK(buf[i] == (inside ? B(0x00) : B(0xAA)));
        }
        buf.fill(B(0xAA));
        switch (width) {
            case 2: le::store16(buf.data() + kOffset, 0); break;
            case 4: le::store32(buf.data() + kOffset, 0); break;
            case 6: continue;  // no little-endian 48-bit helper
            default: le::store64(buf.data() + kOffset, 0); break;
        }
        for (unsigned i = 0; i < buf.size(); ++i) {
            const bool inside = i >= kOffset && i < kOffset + width;
            OT_CHECK(buf[i] == (inside ? B(0x00) : B(0xAA)));
        }
    }
}

// Buffers of exactly the access width on the heap: AddressSanitizer reports any
// read or write beyond them.
OT_TEST(accesses_stay_inside_exactly_sized_buffers) {
    {
        auto p = std::make_unique<std::byte[]>(2);
        be::store16(p.get(), 0xBEEF);
        OT_CHECK_EQ(be::load16(p.get()), std::uint16_t{0xBEEF});
        le::store16(p.get(), 0xBEEF);
        OT_CHECK_EQ(le::load16(p.get()), std::uint16_t{0xBEEF});
    }
    {
        auto p = std::make_unique<std::byte[]>(4);
        be::store32(p.get(), 0xDEADBEEF);
        OT_CHECK_EQ(be::load32(p.get()), std::uint32_t{0xDEADBEEF});
        le::store32(p.get(), 0xDEADBEEF);
        OT_CHECK_EQ(le::load32(p.get()), std::uint32_t{0xDEADBEEF});
    }
    {
        auto p = std::make_unique<std::byte[]>(6);
        be::store48(p.get(), 0x123456789ABCULL);
        OT_CHECK_EQ(be::load48(p.get()), std::uint64_t{0x123456789ABCULL});
    }
    {
        auto p = std::make_unique<std::byte[]>(8);
        be::store64(p.get(), 0xFEDCBA9876543210ULL);
        OT_CHECK_EQ(be::load64(p.get()), std::uint64_t{0xFEDCBA9876543210ULL});
        le::store64(p.get(), 0xFEDCBA9876543210ULL);
        OT_CHECK_EQ(le::load64(p.get()), std::uint64_t{0xFEDCBA9876543210ULL});
    }
}

// Wire buffers are packed: fields sit at arbitrary offsets. All accesses are byte
// based, so every alignment must work (UBSan would flag a misaligned wide load).
OT_TEST(every_alignment_works) {
    std::array<std::byte, 32> buf{};
    for (unsigned i = 0; i < buf.size(); ++i) buf[i] = B(0x10 + i * 7);
    for (unsigned off = 0; off < 9; ++off) {
        const std::byte* p = buf.data() + off;
        OT_CHECK_EQ(be::load16(p), compose_big_endian(p, 2));
        OT_CHECK_EQ(be::load32(p), compose_big_endian(p, 4));
        OT_CHECK_EQ(be::load48(p), compose_big_endian(p, 6));
        OT_CHECK_EQ(be::load64(p), compose_big_endian(p, 8));
        OT_CHECK_EQ(le::load16(p), compose_little_endian(p, 2));
        OT_CHECK_EQ(le::load32(p), compose_little_endian(p, 4));
        OT_CHECK_EQ(le::load64(p), compose_little_endian(p, 8));
    }
    std::array<std::byte, 24> out{};
    for (unsigned off = 1; off < 8; ++off) {
        be::store64(out.data() + off, 0x0102030405060708ULL);
        OT_CHECK_EQ(be::load64(out.data() + off), std::uint64_t{0x0102030405060708ULL});
        le::store32(out.data() + off, 0xA1B2C3D4u);
        OT_CHECK_EQ(le::load32(out.data() + off), std::uint32_t{0xA1B2C3D4u});
    }
}

// ---------------------------------------------------------------------------
// Round trips and oracle comparison
// ---------------------------------------------------------------------------

OT_TEST(every_16_bit_value_round_trips_with_the_right_bytes) {
    std::byte buf[2];
    for (std::uint32_t v = 0; v <= 0xFFFF; ++v) {
        be::store16(buf, static_cast<std::uint16_t>(v));
        if (std::to_integer<unsigned>(buf[0]) != (v >> 8) || std::to_integer<unsigned>(buf[1]) != (v & 0xFF) ||
            be::load16(buf) != v) {
            OT_CHECK(false);
            return;
        }
        le::store16(buf, static_cast<std::uint16_t>(v));
        if (std::to_integer<unsigned>(buf[0]) != (v & 0xFF) || std::to_integer<unsigned>(buf[1]) != (v >> 8) ||
            le::load16(buf) != v) {
            OT_CHECK(false);
            return;
        }
    }
}

OT_TEST(boundary_values_round_trip) {
    const std::uint64_t interesting[] = {0,
                                         1,
                                         0x7F,
                                         0x80,
                                         0xFF,
                                         0x100,
                                         0x7FFF,
                                         0x8000,
                                         0xFFFF,
                                         0x10000,
                                         0x7FFFFFFFULL,
                                         0x80000000ULL,
                                         0xFFFFFFFFULL,
                                         0x100000000ULL,
                                         0x7FFFFFFFFFFFULL,
                                         0x800000000000ULL,
                                         0xFFFFFFFFFFFFULL,
                                         0x7FFFFFFFFFFFFFFFULL,
                                         0x8000000000000000ULL,
                                         0xFFFFFFFFFFFFFFFFULL};
    std::byte buf[8];
    for (std::uint64_t v : interesting) {
        be::store64(buf, v);
        OT_CHECK_EQ(be::load64(buf), v);
        le::store64(buf, v);
        OT_CHECK_EQ(le::load64(buf), v);
        be::store32(buf, static_cast<std::uint32_t>(v));
        OT_CHECK_EQ(be::load32(buf), static_cast<std::uint32_t>(v));
        le::store32(buf, static_cast<std::uint32_t>(v));
        OT_CHECK_EQ(le::load32(buf), static_cast<std::uint32_t>(v));
        be::store48(buf, v);
        OT_CHECK_EQ(be::load48(buf), v & 0xFFFFFFFFFFFFULL);
    }
}

OT_TEST(random_values_match_the_loop_oracle) {
    Rng rng(20240615);
    std::byte buf[8];
    for (int i = 0; i < 100000; ++i) {
        const std::uint64_t v = rng.next() >> rng.bounded(64);

        be::store64(buf, v);
        OT_CHECK(std::equal(buf, buf + 8, big_endian_bytes(v, 8).begin()));
        OT_CHECK_EQ(be::load64(buf), v);

        le::store64(buf, v);
        OT_CHECK(std::equal(buf, buf + 8, little_endian_bytes(v, 8).begin()));
        OT_CHECK_EQ(le::load64(buf), v);

        be::store48(buf, v);
        OT_CHECK(std::equal(buf, buf + 6, big_endian_bytes(v, 6).begin()));
        OT_CHECK_EQ(be::load48(buf), v & 0xFFFFFFFFFFFFULL);

        be::store32(buf, static_cast<std::uint32_t>(v));
        OT_CHECK(std::equal(buf, buf + 4, big_endian_bytes(v, 4).begin()));
        le::store32(buf, static_cast<std::uint32_t>(v));
        OT_CHECK(std::equal(buf, buf + 4, little_endian_bytes(v, 4).begin()));
    }
}

// Loads from arbitrary bytes agree with the oracle (this direction does not go
// through the header's stores, so a symmetric store/load bug cannot hide).
OT_TEST(loads_of_random_bytes_match_the_oracle) {
    Rng rng(99);
    std::array<std::byte, 8> buf{};
    for (int i = 0; i < 100000; ++i) {
        for (auto& b : buf) b = static_cast<std::byte>(rng.next() & 0xFF);
        OT_CHECK_EQ(be::load16(buf.data()), compose_big_endian(buf.data(), 2));
        OT_CHECK_EQ(be::load32(buf.data()), compose_big_endian(buf.data(), 4));
        OT_CHECK_EQ(be::load48(buf.data()), compose_big_endian(buf.data(), 6));
        OT_CHECK_EQ(be::load64(buf.data()), compose_big_endian(buf.data(), 8));
        OT_CHECK_EQ(le::load16(buf.data()), compose_little_endian(buf.data(), 2));
        OT_CHECK_EQ(le::load32(buf.data()), compose_little_endian(buf.data(), 4));
        OT_CHECK_EQ(le::load64(buf.data()), compose_little_endian(buf.data(), 8));
    }
}

OT_TEST(big_and_little_endian_are_byte_reversals_of_each_other) {
    Rng rng(5);
    std::byte fwd[8], rev[8];
    for (int i = 0; i < 1000; ++i) {
        for (int k = 0; k < 8; ++k) fwd[k] = static_cast<std::byte>(rng.next() & 0xFF);
        for (int k = 0; k < 8; ++k) rev[k] = fwd[7 - k];
        OT_CHECK_EQ(be::load64(fwd), le::load64(rev));
        OT_CHECK_EQ(be::load32(fwd), le::load32(rev + 4));
        OT_CHECK_EQ(be::load16(fwd), le::load16(rev + 6));
    }
}

OT_TEST_MAIN()

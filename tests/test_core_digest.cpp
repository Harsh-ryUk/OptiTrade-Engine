// Digest (FNV-1a, 64 bit): published and independently computed vectors, chunking
// invariance, and the byte order used when feeding integers.

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "check.hpp"
#include "optitrade/core/digest.hpp"
#include "optitrade/core/rng.hpp"

using namespace optitrade;

namespace {

std::uint64_t of_text(std::string_view s) {
    Digest d;
    d.update(s.data(), s.size());
    return d.value();
}

// Straightforward FNV-1a, used as an oracle for long inputs.
std::uint64_t slow_fnv1a(const std::vector<unsigned char>& bytes, std::uint64_t h = 0xCBF29CE484222325ULL) {
    for (unsigned char b : bytes) {
        h ^= b;
        h *= 1099511628211ULL;
    }
    return h;
}

}  // namespace

OT_TEST(offset_basis_for_empty_input) {
    Digest d;
    OT_CHECK_EQ(d.value(), std::uint64_t{0xCBF29CE484222325ULL});
    OT_CHECK_EQ(of_text(""), std::uint64_t{0xCBF29CE484222325ULL});
}

// The first three are from the FNV reference test suite.
OT_TEST(known_string_vectors) {
    OT_CHECK_EQ(of_text("a"), std::uint64_t{0xAF63DC4C8601EC8CULL});
    OT_CHECK_EQ(of_text("foobar"), std::uint64_t{0x85944171F73967E8ULL});
    OT_CHECK_EQ(of_text("b"), std::uint64_t{0xAF63DF4C8601F1A5ULL});
    OT_CHECK_EQ(of_text("foo"), std::uint64_t{0xDCB27518FED9D577ULL});
    OT_CHECK_EQ(of_text("hello"), std::uint64_t{0xA430D84680AABD0BULL});
    OT_CHECK_EQ(of_text("The quick brown fox jumps over the lazy dog"), std::uint64_t{0xF3F9B7F5E7E47110ULL});
}

OT_TEST(bytes_with_high_bit_are_not_sign_extended) {
    const unsigned char ff[3] = {0xFF, 0xFF, 0xFF};
    Digest d;
    d.update(ff, 3);
    OT_CHECK_EQ(d.value(), std::uint64_t{0xF998341BE47BAE14ULL});

    unsigned char all[256];
    for (int i = 0; i < 256; ++i) all[i] = static_cast<unsigned char>(i);
    Digest e;
    e.update(all, sizeof all);
    OT_CHECK_EQ(e.value(), std::uint64_t{0x4242DC5249C33625ULL});

    const unsigned char zero = 0;
    Digest z;
    z.update(&zero, 1);
    OT_CHECK_EQ(z.value(), std::uint64_t{0xAF63BD4C8601B7DFULL});
}

OT_TEST(empty_updates_change_nothing) {
    Digest d;
    d.update("abc", 3);
    const std::uint64_t before = d.value();
    d.update(nullptr, 0);
    d.update("xyz", 0);
    OT_CHECK_EQ(d.value(), before);
    OT_CHECK_EQ(d.value(), before);  // value() does not consume or alter state
}

// Known answers for the integer overload (little-endian byte feed), from an
// independent implementation.
OT_TEST(integer_update_known_answers) {
    struct Vec {
        std::uint64_t v, expected;
    };
    for (const Vec& c : {Vec{0, 0xA8C7F832281A39C5ULL}, Vec{1, 0x89CD31291D2AEFA4ULL},
                         Vec{0x0123456789ABCDEFULL, 0x37EB3F3347761C55ULL},
                         Vec{0xFFFFFFFFFFFFFFFFULL, 0x8CF51A8BFCA3883DULL},
                         Vec{0x8000000000000000ULL, 0xA8C7783228196045ULL}}) {
        Digest d;
        d.update(c.v);
        OT_CHECK_EQ(d.value(), c.expected);
    }
}

OT_TEST(integer_update_feeds_least_significant_byte_first) {
    Digest by_value;
    by_value.update(std::uint64_t{0x0807060504030201ULL});
    Digest by_bytes;
    const unsigned char le[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    by_bytes.update(le, 8);
    OT_CHECK_EQ(by_value.value(), by_bytes.value());

    Digest reversed;
    const unsigned char be[8] = {8, 7, 6, 5, 4, 3, 2, 1};
    reversed.update(be, 8);
    OT_CHECK(by_value.value() != reversed.value());
}

OT_TEST(integer_update_matches_bytewise_feed_for_random_values) {
    Rng rng(1);
    for (int i = 0; i < 20000; ++i) {
        const std::uint64_t v = rng.next() >> rng.bounded(64);  // mix of magnitudes
        std::array<unsigned char, 8> bytes{};
        for (unsigned b = 0; b < 8; ++b) bytes[b] = static_cast<unsigned char>((v >> (8 * b)) & 0xFF);
        Digest a, b;
        a.update(v);
        b.update(bytes.data(), bytes.size());
        if (a.value() != b.value()) {
            OT_CHECK_EQ(a.value(), b.value());
            return;
        }
    }
}

// Narrow and signed arguments convert to uint64_t before feeding: always eight bytes,
// sign extended for negative values (documented in the header).
OT_TEST(narrow_and_signed_integers_widen_to_eight_bytes) {
    Digest wide, narrow;
    wide.update(std::uint64_t{5});
    narrow.update(std::uint32_t{5});
    OT_CHECK_EQ(wide.value(), narrow.value());

    Digest neg, all_ones;
    neg.update(-1);
    all_ones.update(std::uint64_t{0xFFFFFFFFFFFFFFFFULL});
    OT_CHECK_EQ(neg.value(), all_ones.value());

    Digest u8;
    u8.update(std::uint8_t{200});
    Digest ref;
    ref.update(std::uint64_t{200});
    OT_CHECK_EQ(u8.value(), ref.value());
}

OT_TEST(mixed_updates_chain_in_order) {
    Digest d;
    d.update("abc", 3);
    d.update(std::uint64_t{1});
    d.update("xyz", 3);
    OT_CHECK_EQ(d.value(), std::uint64_t{0x3AF74518ECFB89A5ULL});

    Digest pair;
    pair.update(std::uint64_t{123456789});
    pair.update(std::uint64_t{0xFFFFFFFFFFFFFFFFULL});
    OT_CHECK_EQ(pair.value(), std::uint64_t{0x17267554AB287BB1ULL});
}

OT_TEST(splitting_the_input_never_changes_the_result) {
    Rng rng(77);
    std::vector<unsigned char> data(129);
    for (auto& b : data) b = static_cast<unsigned char>(rng.next());
    const std::uint64_t whole = slow_fnv1a(data);
    Digest one;
    one.update(data.data(), data.size());
    OT_CHECK_EQ(one.value(), whole);
    for (std::size_t cut = 0; cut <= data.size(); ++cut) {
        Digest d;
        d.update(data.data(), cut);
        d.update(data.data() + cut, data.size() - cut);
        OT_CHECK_EQ(d.value(), whole);
    }
    // And byte at a time.
    Digest bytewise;
    for (unsigned char b : data) bytewise.update(&b, 1);
    OT_CHECK_EQ(bytewise.value(), whole);
}

OT_TEST(long_random_inputs_match_reference_fnv) {
    Rng rng(4);
    for (int round = 0; round < 20; ++round) {
        std::vector<unsigned char> data(static_cast<std::size_t>(rng.bounded(5000)));
        for (auto& b : data) b = static_cast<unsigned char>(rng.next());
        Digest d;
        d.update(data.data(), data.size());
        OT_CHECK_EQ(d.value(), slow_fnv1a(data));
    }
}

// A fingerprint that could not tell nearby streams apart would be useless for the
// determinism tests that rely on it.
OT_TEST(order_and_single_bit_changes_are_detected) {
    Digest ab, ba;
    ab.update(std::uint64_t{1});
    ab.update(std::uint64_t{2});
    ba.update(std::uint64_t{2});
    ba.update(std::uint64_t{1});
    OT_CHECK(ab.value() != ba.value());

    Rng rng(3);
    std::array<unsigned char, 64> data{};
    for (auto& b : data) b = static_cast<unsigned char>(rng.next());
    Digest base;
    base.update(data.data(), data.size());
    for (std::size_t byte = 0; byte < data.size(); ++byte)
        for (unsigned bit = 0; bit < 8; ++bit) {
            auto flipped = data;
            flipped[byte] = static_cast<unsigned char>(flipped[byte] ^ (1u << bit));
            Digest d;
            d.update(flipped.data(), flipped.size());
            OT_CHECK(d.value() != base.value());
        }
}

OT_TEST(independent_instances_do_not_share_state) {
    Digest a, b;
    a.update("first", 5);
    OT_CHECK_EQ(b.value(), std::uint64_t{0xCBF29CE484222325ULL});
    b.update("first", 5);
    OT_CHECK_EQ(a.value(), b.value());
    Digest copy = a;
    copy.update("more", 4);
    OT_CHECK(copy.value() != a.value());
    OT_CHECK_EQ(a.value(), b.value());
}

OT_TEST_MAIN()

// MoldUDP64 framing tests. The packets below are written out byte by byte from the
// protocol layout (session(10) | sequence(u64) | count(u16) | [length(u16) payload]...),
// never produced by the code under test, so an encoder/decoder pair sharing a mistake
// cannot pass. The randomized tests compare against separately written naive models.

#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

#include "check.hpp"
#include "optitrade/core/rng.hpp"
#include "optitrade/net/mold64.hpp"

namespace {

using namespace optitrade;
using namespace optitrade::net::mold64;
using Bytes = std::vector<std::byte>;
using Session = std::array<char, kSessionSize>;

Bytes hex(std::initializer_list<unsigned> v) {
    Bytes out;
    for (unsigned x : v) out.push_back(static_cast<std::byte>(x));
    return out;
}
Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}
std::span<const std::byte> view(const Bytes& b) { return {b.data(), b.size()}; }
Bytes filled(std::size_t n, unsigned value) { return Bytes(n, static_cast<std::byte>(value)); }

Session session_of(const char (&s)[11]) {
    Session out{};
    std::memcpy(out.data(), s, kSessionSize);
    return out;
}
const Session kSessA = session_of("ABCDEFGHIJ");
const Session kSessB = session_of("SESSION001");

// "ABCDEFGHIJ", sequence 3001 (0x0BB9), count 0.
const Bytes kHeartbeat = hex({0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A,
                              0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0B, 0xB9, 0x00, 0x00});
// "ABCDEFGHIJ", sequence 1'000'000 (0x0F4240), count 0xFFFF.
const Bytes kEosPkt = hex({0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A,
                                 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x42, 0x40, 0xFF, 0xFF});
// "SESSION001", sequence 0x0000000100000005, three messages of 3, 1 and 5 bytes.
const Bytes kThree = hex({0x53, 0x45, 0x53, 0x53, 0x49, 0x4F, 0x4E, 0x30, 0x30, 0x31,
                          0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x03,
                          0x00, 0x03, 0xAA, 0xBB, 0xCC,
                          0x00, 0x01, 0xDD,
                          0x00, 0x05, 0x01, 0x02, 0x03, 0x04, 0x05});

// Header of a data packet for the error-path tests: session "SESSION001", sequence 7.
Bytes data_header(unsigned count) {
    return hex({0x53, 0x45, 0x53, 0x53, 0x49, 0x4F, 0x4E, 0x30, 0x30, 0x31, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x07, (count >> 8) & 0xFFu, count & 0xFFu});
}

struct Got {
    std::uint64_t seq{};
    std::ptrdiff_t offset{};  // where the span starts inside the packet
    Bytes bytes;
};
struct Run {
    DecodeStatus status{};
    Header hdr;
    std::vector<Got> got;
};

Run run(const Bytes& packet) {
    Run r;
    r.status = for_each_message(view(packet), r.hdr, [&](std::uint64_t seq, std::span<const std::byte> m) {
        r.got.push_back({seq, m.data() - packet.data(), Bytes(m.begin(), m.end())});
    });
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// parse_header
// ---------------------------------------------------------------------------------------

OT_TEST(parse_header_heartbeat) {
    Header h;
    OT_CHECK(parse_header(view(kHeartbeat), h) == DecodeStatus::ok);
    OT_CHECK(h.session == kSessA);
    OT_CHECK_EQ(h.sequence, std::uint64_t{3001});
    OT_CHECK_EQ(h.count, std::uint16_t{0});
    OT_CHECK(h.kind == Kind::heartbeat);
}

OT_TEST(parse_header_end_of_session) {
    Header h;
    OT_CHECK(parse_header(view(kEosPkt), h) == DecodeStatus::ok);
    OT_CHECK(h.session == kSessA);
    OT_CHECK_EQ(h.sequence, std::uint64_t{1'000'000});
    OT_CHECK_EQ(h.count, kEndOfSession);
    OT_CHECK(h.kind == Kind::end_of_session);
}

OT_TEST(parse_header_field_offsets_and_byte_order) {
    // Distinct byte values everywhere so a swapped or shifted field cannot go unnoticed;
    // session bytes include NUL, space and high-bit values (it is opaque, not a C string).
    const Bytes p = hex({0x00, 0xFF, 0x20, 0x7E, 0x80, 0x01, 0x02, 0x03, 0x04, 0x05,
                         0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x01, 0x02});
    Header h;
    OT_CHECK(parse_header(view(p), h) == DecodeStatus::ok);
    const char expect[kSessionSize] = {'\x00', '\xFF', ' ', '~', '\x80', '\x01', '\x02', '\x03', '\x04', '\x05'};
    OT_CHECK(std::memcmp(h.session.data(), expect, kSessionSize) == 0);
    OT_CHECK_EQ(h.sequence, std::uint64_t{0x0102030405060708ULL});
    OT_CHECK_EQ(h.count, std::uint16_t{0x0102});
    OT_CHECK(h.kind == Kind::data);
}

OT_TEST(parse_header_count_boundaries) {
    Header h;
    Bytes p = data_header(1);
    OT_CHECK(parse_header(view(p), h) == DecodeStatus::ok);
    OT_CHECK(h.kind == Kind::data);
    p = data_header(0xFFFE);  // largest real message count
    OT_CHECK(parse_header(view(p), h) == DecodeStatus::ok);
    OT_CHECK(h.kind == Kind::data);
    OT_CHECK_EQ(h.count, std::uint16_t{0xFFFE});
    p = data_header(0xFFFF);
    OT_CHECK(parse_header(view(p), h) == DecodeStatus::ok);
    OT_CHECK(h.kind == Kind::end_of_session);
}

OT_TEST(parse_header_truncated_leaves_output_untouched) {
    for (std::size_t n = 0; n < kHeaderSize; ++n) {
        Header h;
        h.sequence = 0xDEAD;
        h.count = 77;
        h.kind = Kind::heartbeat;
        OT_CHECK(parse_header(std::span<const std::byte>(kHeartbeat.data(), n), h) == DecodeStatus::truncated);
        OT_CHECK_EQ(h.sequence, std::uint64_t{0xDEAD});
        OT_CHECK_EQ(h.count, std::uint16_t{77});
        OT_CHECK(h.kind == Kind::heartbeat);
    }
}

OT_TEST(parse_header_does_not_look_at_the_body) {
    // Garbage after a valid header is for_each_message's business, not parse_header's.
    Bytes p = cat({data_header(3), hex({0xFF, 0xFF, 0xFF})});
    Header h;
    OT_CHECK(parse_header(view(p), h) == DecodeStatus::ok);
    OT_CHECK_EQ(h.count, std::uint16_t{3});
}

// ---------------------------------------------------------------------------------------
// for_each_message: accepted packets
// ---------------------------------------------------------------------------------------

OT_TEST(for_each_heartbeat_delivers_nothing) {
    const Run r = run(kHeartbeat);
    OT_CHECK(r.status == DecodeStatus::ok);
    OT_CHECK(r.hdr.kind == Kind::heartbeat);
    OT_CHECK_EQ(r.hdr.sequence, std::uint64_t{3001});
    OT_CHECK_EQ(r.got.size(), std::size_t{0});
}

OT_TEST(for_each_end_of_session_delivers_nothing) {
    // count 0xFFFF must not be mistaken for 65535 blocks.
    const Run r = run(kEosPkt);
    OT_CHECK(r.status == DecodeStatus::ok);
    OT_CHECK(r.hdr.kind == Kind::end_of_session);
    OT_CHECK_EQ(r.got.size(), std::size_t{0});
}

OT_TEST(for_each_three_messages) {
    const Run r = run(kThree);
    OT_CHECK(r.status == DecodeStatus::ok);
    OT_CHECK(r.hdr.session == kSessB);
    OT_CHECK_EQ(r.hdr.sequence, std::uint64_t{0x0000000100000005ULL});
    OT_CHECK_EQ(r.hdr.count, std::uint16_t{3});
    OT_CHECK(r.hdr.kind == Kind::data);
    OT_CHECK_EQ(r.got.size(), std::size_t{3});
    if (r.got.size() != 3) return;
    // Sequences count up from the header value (the first one exceeds 32 bits on purpose).
    OT_CHECK_EQ(r.got[0].seq, std::uint64_t{4294967301ULL});
    OT_CHECK_EQ(r.got[1].seq, std::uint64_t{4294967302ULL});
    OT_CHECK_EQ(r.got[2].seq, std::uint64_t{4294967303ULL});
    // Spans alias the packet at the payload offsets (zero copy).
    OT_CHECK_EQ(r.got[0].offset, std::ptrdiff_t{22});
    OT_CHECK_EQ(r.got[1].offset, std::ptrdiff_t{27});
    OT_CHECK_EQ(r.got[2].offset, std::ptrdiff_t{30});
    OT_CHECK(r.got[0].bytes == hex({0xAA, 0xBB, 0xCC}));
    OT_CHECK(r.got[1].bytes == hex({0xDD}));
    OT_CHECK(r.got[2].bytes == hex({0x01, 0x02, 0x03, 0x04, 0x05}));
}

OT_TEST(for_each_zero_length_block_is_an_empty_message) {
    const Bytes p = cat({data_header(3), hex({0x00, 0x00, 0x00, 0x02, 0xAB, 0xCD, 0x00, 0x00})});
    const Run r = run(p);
    OT_CHECK(r.status == DecodeStatus::ok);
    OT_CHECK_EQ(r.got.size(), std::size_t{3});
    if (r.got.size() != 3) return;
    OT_CHECK_EQ(r.got[0].bytes.size(), std::size_t{0});
    OT_CHECK(r.got[1].bytes == hex({0xAB, 0xCD}));
    OT_CHECK_EQ(r.got[2].bytes.size(), std::size_t{0});
    OT_CHECK_EQ(r.got[0].seq, std::uint64_t{7});
    OT_CHECK_EQ(r.got[2].seq, std::uint64_t{9});
}

OT_TEST(for_each_exactly_full_ethernet_payload) {
    // 1472 bytes = 1500 MTU - IPv4 - UDP: header 20 + blocks of 500, 500 and 446 bytes.
    const Bytes p = cat({data_header(3),
                         hex({0x01, 0xF4}), filled(500, 0x11),
                         hex({0x01, 0xF4}), filled(500, 0x22),
                         hex({0x01, 0xBE}), filled(446, 0x33)});
    OT_CHECK_EQ(p.size(), std::size_t{1472});
    const Run r = run(p);
    OT_CHECK(r.status == DecodeStatus::ok);
    OT_CHECK_EQ(r.got.size(), std::size_t{3});
    if (r.got.size() != 3) return;
    OT_CHECK_EQ(r.got[0].bytes.size(), std::size_t{500});
    OT_CHECK_EQ(r.got[2].bytes.size(), std::size_t{446});
    OT_CHECK(r.got[2].bytes == filled(446, 0x33));
    OT_CHECK_EQ(r.got[2].offset + static_cast<std::ptrdiff_t>(r.got[2].bytes.size()), std::ptrdiff_t{1472});
}

OT_TEST(for_each_largest_block_and_largest_count) {
    {
        const Bytes p = cat({data_header(1), hex({0xFF, 0xFF}), filled(65535, 0x5A)});
        const Run r = run(p);
        OT_CHECK(r.status == DecodeStatus::ok);
        OT_CHECK_EQ(r.got.size(), std::size_t{1});
        if (!r.got.empty()) OT_CHECK_EQ(r.got[0].bytes.size(), std::size_t{65535});
    }
    {
        Bytes p = data_header(0xFFFE);
        for (int i = 0; i < 0xFFFE; ++i) p.insert(p.end(), {std::byte{0}, std::byte{0}});
        const Run r = run(p);
        OT_CHECK(r.status == DecodeStatus::ok);
        OT_CHECK_EQ(r.got.size(), std::size_t{0xFFFE});
        if (!r.got.empty()) OT_CHECK_EQ(r.got.back().seq, std::uint64_t{7 + 0xFFFD});
    }
}

// ---------------------------------------------------------------------------------------
// for_each_message: rejected packets (the handler must never run)
// ---------------------------------------------------------------------------------------

OT_TEST(for_each_truncated_header) {
    for (std::size_t n = 0; n < kHeaderSize; ++n) {
        const Bytes p(kThree.begin(), kThree.begin() + static_cast<std::ptrdiff_t>(n));
        const Run r = run(p);
        OT_CHECK(r.status == DecodeStatus::truncated);
        OT_CHECK_EQ(r.got.size(), std::size_t{0});
    }
}

OT_TEST(for_each_every_truncation_of_a_valid_packet_is_rejected_without_callbacks) {
    // Cutting kThree at any point leaves fewer bytes than its blocks announce.
    for (std::size_t n = 0; n < kThree.size(); ++n) {
        const Bytes p(kThree.begin(), kThree.begin() + static_cast<std::ptrdiff_t>(n));
        const Run r = run(p);
        OT_CHECK(r.status == DecodeStatus::truncated);
        OT_CHECK_EQ(r.got.size(), std::size_t{0});
    }
}

OT_TEST(for_each_block_length_runs_past_the_end) {
    // Length says 5, only 4 bytes follow.
    Run r = run(cat({data_header(1), hex({0x00, 0x05, 1, 2, 3, 4})}));
    OT_CHECK(r.status == DecodeStatus::truncated);
    OT_CHECK_EQ(r.got.size(), std::size_t{0});
    // Length prefix itself cut off after one byte.
    r = run(cat({data_header(1), hex({0x00})}));
    OT_CHECK(r.status == DecodeStatus::truncated);
    // The first block is fine, the second overruns: nothing may be delivered, not even the
    // good block (validate-then-deliver).
    r = run(cat({data_header(2), hex({0x00, 0x01, 0xAA, 0x00, 0x09, 0xBB})}));
    OT_CHECK(r.status == DecodeStatus::truncated);
    OT_CHECK_EQ(r.got.size(), std::size_t{0});
    // Maximum length field against a tiny packet.
    r = run(cat({data_header(1), hex({0xFF, 0xFF, 0x01})}));
    OT_CHECK(r.status == DecodeStatus::truncated);
    OT_CHECK_EQ(r.got.size(), std::size_t{0});
}

OT_TEST(for_each_count_larger_than_blocks_present) {
    Run r = run(cat({data_header(3), hex({0x00, 0x01, 0xAA, 0x00, 0x01, 0xBB})}));
    OT_CHECK(r.status == DecodeStatus::truncated);
    OT_CHECK_EQ(r.got.size(), std::size_t{0});
    // Header only, but it promises a message.
    r = run(data_header(1));
    OT_CHECK(r.status == DecodeStatus::truncated);
    // Largest count against an almost empty body must fail fast, not loop over 65534 blocks.
    r = run(cat({data_header(0xFFFE), hex({0x00, 0x00})}));
    OT_CHECK(r.status == DecodeStatus::truncated);
    OT_CHECK_EQ(r.got.size(), std::size_t{0});
}

OT_TEST(for_each_count_smaller_than_blocks_present) {
    const Run r = run(cat({data_header(1), hex({0x00, 0x01, 0xAA, 0x00, 0x01, 0xBB})}));
    OT_CHECK(r.status == DecodeStatus::bad_length);
    OT_CHECK_EQ(r.got.size(), std::size_t{0});
}

OT_TEST(for_each_trailing_garbage) {
    Run r = run(cat({data_header(1), hex({0x00, 0x02, 0xAA, 0xBB, 0x99})}));
    OT_CHECK(r.status == DecodeStatus::bad_length);
    OT_CHECK_EQ(r.got.size(), std::size_t{0});
    // One trailing byte is too short to be another block but still forbidden.
    r = run(cat({kThree, hex({0x00})}));
    OT_CHECK(r.status == DecodeStatus::bad_length);
    OT_CHECK_EQ(r.got.size(), std::size_t{0});
}

OT_TEST(for_each_control_packets_must_be_header_only) {
    for (const Bytes* base : {&kHeartbeat, &kEosPkt}) {
        Run r = run(cat({*base, hex({0x00})}));
        OT_CHECK(r.status == DecodeStatus::bad_length);
        r = run(cat({*base, hex({0x00, 0x00})}));  // an empty block is still a block
        OT_CHECK(r.status == DecodeStatus::bad_length);
        r = run(cat({*base, hex({0x00, 0x01, 0xAA})}));
        OT_CHECK(r.status == DecodeStatus::bad_length);
        OT_CHECK_EQ(r.got.size(), std::size_t{0});
    }
}

OT_TEST(for_each_fills_header_even_when_blocks_are_rejected) {
    const Run r = run(cat({data_header(2), hex({0x00})}));
    OT_CHECK(r.status == DecodeStatus::truncated);
    OT_CHECK_EQ(r.hdr.sequence, std::uint64_t{7});
    OT_CHECK_EQ(r.hdr.count, std::uint16_t{2});
}

// ---------------------------------------------------------------------------------------
// PacketBuilder
// ---------------------------------------------------------------------------------------

OT_TEST(builder_known_answer) {
    Bytes buf(64, std::byte{0xEE});
    PacketBuilder b(std::span<std::byte>(buf), kSessB, 0x0000000100000005ULL);
    OT_CHECK(b.add(view(hex({0xAA, 0xBB, 0xCC}))));
    OT_CHECK(b.add(view(hex({0xDD}))));
    OT_CHECK(b.add(view(hex({0x01, 0x02, 0x03, 0x04, 0x05}))));
    OT_CHECK_EQ(b.count(), std::uint16_t{3});
    const auto pkt = b.finish();
    OT_CHECK_EQ(pkt.size(), kThree.size());
    OT_CHECK(Bytes(pkt.begin(), pkt.end()) == kThree);
    OT_CHECK(pkt.data() == buf.data());
    // Nothing beyond the packet may have been written.
    for (std::size_t i = pkt.size(); i < buf.size(); ++i) OT_CHECK(buf[i] == std::byte{0xEE});
}

OT_TEST(builder_without_messages_is_a_heartbeat) {
    Bytes buf(kHeaderSize);
    PacketBuilder b(std::span<std::byte>(buf), kSessA, 3001);
    const auto pkt = b.finish();
    OT_CHECK(Bytes(pkt.begin(), pkt.end()) == kHeartbeat);
    Header h;
    OT_CHECK(parse_header(pkt, h) == DecodeStatus::ok);
    OT_CHECK(h.kind == Kind::heartbeat);
}

OT_TEST(builder_capacity_edge) {
    // 20 header + (2 + 3) = 25 bytes holds exactly one 3-byte message.
    Bytes buf(25, std::byte{0xEE});
    PacketBuilder b(std::span<std::byte>(buf), kSessA, 100);
    OT_CHECK(!b.add(view(filled(4, 1))));  // one byte too many
    OT_CHECK_EQ(b.count(), std::uint16_t{0});
    OT_CHECK(b.add(view(filled(3, 1))));   // exactly fills the buffer
    OT_CHECK_EQ(b.size(), std::size_t{25});
    OT_CHECK(!b.add(view(Bytes{})));       // even an empty block needs 2 bytes
    OT_CHECK(!b.add(view(filled(1, 2))));
    OT_CHECK_EQ(b.count(), std::uint16_t{1});
    const auto pkt = b.finish();
    OT_CHECK_EQ(pkt.size(), std::size_t{25});
    const Run r = run(Bytes(pkt.begin(), pkt.end()));
    OT_CHECK(r.status == DecodeStatus::ok);
    OT_CHECK_EQ(r.got.size(), std::size_t{1});

    // One byte less: the same message no longer fits, but a 2-byte one does.
    Bytes small(24, std::byte{0xEE});
    PacketBuilder c(std::span<std::byte>(small), kSessA, 100);
    OT_CHECK(!c.add(view(filled(3, 1))));
    OT_CHECK(c.add(view(filled(2, 1))));
    OT_CHECK_EQ(c.finish().size(), std::size_t{24});
}

OT_TEST(builder_failed_add_after_success_keeps_the_packet_intact) {
    Bytes buf(30, std::byte{0xEE});
    PacketBuilder b(std::span<std::byte>(buf), kSessA, 1);
    OT_CHECK(b.add(view(hex({0x11, 0x22}))));
    OT_CHECK(!b.add(view(filled(100, 9))));
    const auto pkt = b.finish();
    OT_CHECK_EQ(pkt.size(), std::size_t{24});
    OT_CHECK_EQ(pkt[18], std::byte{0});
    OT_CHECK_EQ(pkt[19], std::byte{1});
    for (std::size_t i = pkt.size(); i < buf.size(); ++i) OT_CHECK(buf[i] == std::byte{0xEE});
}

OT_TEST(builder_buffer_too_small_for_the_header_is_inert) {
    Bytes buf(kHeaderSize - 1, std::byte{0xEE});
    PacketBuilder b(std::span<std::byte>(buf), kSessA, 1);
    OT_CHECK(!b.add(view(hex({1}))));
    OT_CHECK(!b.add(view(Bytes{})));
    OT_CHECK_EQ(b.count(), std::uint16_t{0});
    OT_CHECK_EQ(b.size(), std::size_t{0});
    OT_CHECK(b.finish().empty());
    for (std::byte x : buf) OT_CHECK(x == std::byte{0xEE});

    PacketBuilder z(std::span<std::byte>{}, kSessA, 1);
    OT_CHECK(!z.add(view(hex({1}))));
    OT_CHECK(z.finish().empty());
}

OT_TEST(builder_header_only_buffer) {
    Bytes buf(kHeaderSize);
    PacketBuilder b(std::span<std::byte>(buf), kSessA, 5);
    OT_CHECK(!b.add(view(Bytes{})));
    OT_CHECK_EQ(b.finish().size(), kHeaderSize);
}

OT_TEST(builder_zero_length_and_oversized_messages) {
    Bytes buf(80'000);
    PacketBuilder b(std::span<std::byte>(buf), kSessA, 1);
    OT_CHECK(b.add(view(Bytes{})));                    // legal, parses back as an empty message
    OT_CHECK(!b.add(view(filled(65536, 1))));          // does not fit the 16-bit length field
    OT_CHECK(b.add(view(filled(65535, 2))));           // the largest expressible block
    OT_CHECK_EQ(b.count(), std::uint16_t{2});
    const auto pkt = b.finish();
    const Run r = run(Bytes(pkt.begin(), pkt.end()));
    OT_CHECK(r.status == DecodeStatus::ok);
    OT_CHECK_EQ(r.got.size(), std::size_t{2});
    if (r.got.size() == 2) {
        OT_CHECK_EQ(r.got[0].bytes.size(), std::size_t{0});
        OT_CHECK_EQ(r.got[1].bytes.size(), std::size_t{65535});
    }
}

OT_TEST(builder_count_stops_at_65534) {
    // 0xFFFF would read as end-of-session, so the builder must refuse the 65535th message
    // even though the buffer has room for it.
    Bytes buf(kHeaderSize + 2 * 70'000);
    PacketBuilder b(std::span<std::byte>(buf), kSessA, 10);
    std::size_t accepted = 0;
    for (int i = 0; i < 70'000; ++i) {
        if (b.add(view(Bytes{}))) ++accepted;
    }
    OT_CHECK_EQ(accepted, std::size_t{65534});
    OT_CHECK_EQ(b.count(), std::uint16_t{65534});
    OT_CHECK(!b.add(view(hex({1}))));
    const auto pkt = b.finish();
    OT_CHECK_EQ(pkt[18], std::byte{0xFF});
    OT_CHECK_EQ(pkt[19], std::byte{0xFE});
    Header h;
    OT_CHECK(parse_header(pkt, h) == DecodeStatus::ok);
    OT_CHECK(h.kind == Kind::data);
    std::size_t n = 0;
    Header h2;
    OT_CHECK(for_each_message(pkt, h2, [&](std::uint64_t, std::span<const std::byte>) { ++n; }) ==
             DecodeStatus::ok);
    OT_CHECK_EQ(n, std::size_t{65534});
}

OT_TEST(builder_finish_is_idempotent_and_seals) {
    Bytes buf(64);
    PacketBuilder b(std::span<std::byte>(buf), kSessB, 0x0000000100000005ULL);
    OT_CHECK(b.add(view(hex({0xAA, 0xBB, 0xCC}))));
    const auto first = b.finish();
    const Bytes snapshot(first.begin(), first.end());
    const auto second = b.finish();
    OT_CHECK(first.data() == second.data());
    OT_CHECK_EQ(first.size(), second.size());
    OT_CHECK(Bytes(second.begin(), second.end()) == snapshot);
    // Sealed: a late add must not alter a packet that may already be on its way out.
    OT_CHECK(!b.add(view(hex({0xEE}))));
    OT_CHECK_EQ(b.count(), std::uint16_t{1});
    const auto third = b.finish();
    OT_CHECK(Bytes(third.begin(), third.end()) == snapshot);
}

OT_TEST(control_packet_writers_known_answer) {
    Bytes buf(kHeaderSize + 4, std::byte{0xEE});
    OT_CHECK_EQ(write_heartbeat(std::span<std::byte>(buf), kSessA, 3001), kHeaderSize);
    OT_CHECK(Bytes(buf.begin(), buf.begin() + kHeaderSize) == kHeartbeat);
    OT_CHECK_EQ(write_end_of_session(std::span<std::byte>(buf), kSessA, 1'000'000), kHeaderSize);
    OT_CHECK(Bytes(buf.begin(), buf.begin() + kHeaderSize) == kEosPkt);
    for (std::size_t i = kHeaderSize; i < buf.size(); ++i) OT_CHECK(buf[i] == std::byte{0xEE});

    Bytes tiny(kHeaderSize - 1, std::byte{0xEE});
    OT_CHECK_EQ(write_heartbeat(std::span<std::byte>(tiny), kSessA, 1), std::size_t{0});
    OT_CHECK_EQ(write_end_of_session(std::span<std::byte>(tiny), kSessA, 1), std::size_t{0});
    for (std::byte x : tiny) OT_CHECK(x == std::byte{0xEE});
}

// ---------------------------------------------------------------------------------------
// SequenceTracker
// ---------------------------------------------------------------------------------------

namespace {

Header data_pkt(const Session& s, std::uint64_t seq, std::uint16_t count) {
    return {s, seq, count, Kind::data};
}
Header heartbeat_pkt(const Session& s, std::uint64_t next_seq) { return {s, next_seq, 0, Kind::heartbeat}; }
Header eos_pkt(const Session& s, std::uint64_t next_seq) {
    return {s, next_seq, kEndOfSession, Kind::end_of_session};
}
using R = SequenceCheck::Result;

}  // namespace

OT_TEST(tracker_first_packet_sets_the_baseline) {
    SequenceTracker t;
    // Joining mid-session at sequence 5000 is not a gap from 0.
    const SequenceCheck c = t.on_packet(data_pkt(kSessA, 5000, 4));
    OT_CHECK(c.result == R::in_order);
    OT_CHECK_EQ(c.expected, std::uint64_t{5000});
    OT_CHECK_EQ(c.received, std::uint64_t{5000});
    OT_CHECK_EQ(t.expected(), std::uint64_t{5004});
    OT_CHECK_EQ(t.gaps(), std::uint64_t{0});
}

OT_TEST(tracker_in_order_multi_message_packets) {
    SequenceTracker t;
    OT_CHECK(t.on_packet(data_pkt(kSessA, 1, 3)).result == R::in_order);  // 1,2,3
    SequenceCheck c = t.on_packet(data_pkt(kSessA, 4, 2));                // 4,5
    OT_CHECK(c.result == R::in_order);
    OT_CHECK_EQ(c.expected, std::uint64_t{4});
    c = t.on_packet(data_pkt(kSessA, 6, 500));
    OT_CHECK(c.result == R::in_order);
    OT_CHECK_EQ(t.expected(), std::uint64_t{506});
    c = t.on_packet(data_pkt(kSessA, 506, 1));
    OT_CHECK(c.result == R::in_order);
    OT_CHECK_EQ(t.gaps(), std::uint64_t{0});
    OT_CHECK_EQ(t.missing_messages(), std::uint64_t{0});
    OT_CHECK_EQ(t.duplicates(), std::uint64_t{0});
}

OT_TEST(tracker_gap_reports_missing_count_and_resynchronises) {
    SequenceTracker t;
    t.on_packet(data_pkt(kSessA, 1, 3));  // expects 4 next
    SequenceCheck c = t.on_packet(data_pkt(kSessA, 9, 1));
    OT_CHECK(c.result == R::gap);
    OT_CHECK_EQ(c.expected, std::uint64_t{4});
    OT_CHECK_EQ(c.received, std::uint64_t{9});
    OT_CHECK_EQ(c.missing, std::uint64_t{5});  // 4,5,6,7,8
    OT_CHECK_EQ(t.gaps(), std::uint64_t{1});
    OT_CHECK_EQ(t.missing_messages(), std::uint64_t{5});
    // The loss is counted once: the next packet continues from the one that exposed it.
    c = t.on_packet(data_pkt(kSessA, 10, 2));
    OT_CHECK(c.result == R::in_order);
    OT_CHECK_EQ(t.gaps(), std::uint64_t{1});
    // A second, separate loss adds to the totals.
    c = t.on_packet(data_pkt(kSessA, 13, 1));  // expected 12
    OT_CHECK(c.result == R::gap);
    OT_CHECK_EQ(c.missing, std::uint64_t{1});
    OT_CHECK_EQ(t.gaps(), std::uint64_t{2});
    OT_CHECK_EQ(t.missing_messages(), std::uint64_t{6});
}

OT_TEST(tracker_duplicate_and_old_packets) {
    SequenceTracker t;
    t.on_packet(data_pkt(kSessA, 1, 10));  // expects 11
    SequenceCheck c = t.on_packet(data_pkt(kSessA, 1, 10));  // exact replay
    OT_CHECK(c.result == R::duplicate);
    OT_CHECK_EQ(c.expected, std::uint64_t{11});
    OT_CHECK_EQ(c.received, std::uint64_t{1});
    OT_CHECK_EQ(c.missing, std::uint64_t{0});
    c = t.on_packet(data_pkt(kSessA, 4, 2));  // old, inside what we already have
    OT_CHECK(c.result == R::duplicate);
    OT_CHECK_EQ(t.expected(), std::uint64_t{11});  // state unchanged
    OT_CHECK_EQ(t.duplicates(), std::uint64_t{2});
    OT_CHECK_EQ(t.gaps(), std::uint64_t{0});
    OT_CHECK(t.on_packet(data_pkt(kSessA, 11, 1)).result == R::in_order);
}

OT_TEST(tracker_overlapping_retransmission_extends_expected) {
    SequenceTracker t;
    t.on_packet(data_pkt(kSessA, 1, 10));  // expects 11
    // Sequence 9..12: 9 and 10 are old, 11 and 12 are new. Still a "duplicate" (it starts
    // below expected), but the new tail must be accounted for.
    const SequenceCheck c = t.on_packet(data_pkt(kSessA, 9, 4));
    OT_CHECK(c.result == R::duplicate);
    OT_CHECK_EQ(c.expected, std::uint64_t{11});
    OT_CHECK_EQ(t.expected(), std::uint64_t{13});
    // So the next in-order packet is not reported as a gap.
    OT_CHECK(t.on_packet(data_pkt(kSessA, 13, 1)).result == R::in_order);
    OT_CHECK_EQ(t.gaps(), std::uint64_t{0});
    OT_CHECK_EQ(t.missing_messages(), std::uint64_t{0});
}

OT_TEST(tracker_heartbeat_reveals_a_gap) {
    SequenceTracker t;
    t.on_packet(data_pkt(kSessA, 1, 19));  // expects 20
    // In sync: a heartbeat at the expected sequence changes nothing.
    SequenceCheck c = t.on_packet(heartbeat_pkt(kSessA, 20));
    OT_CHECK(c.result == R::in_order);
    OT_CHECK_EQ(t.expected(), std::uint64_t{20});
    // The publisher says its next message is 25: 20..24 never reached us.
    c = t.on_packet(heartbeat_pkt(kSessA, 25));
    OT_CHECK(c.result == R::gap);
    OT_CHECK_EQ(c.expected, std::uint64_t{20});
    OT_CHECK_EQ(c.received, std::uint64_t{25});
    OT_CHECK_EQ(c.missing, std::uint64_t{5});
    OT_CHECK_EQ(t.gaps(), std::uint64_t{1});
    OT_CHECK_EQ(t.missing_messages(), std::uint64_t{5});
    // Sequencing continues from the heartbeat's position.
    OT_CHECK(t.on_packet(data_pkt(kSessA, 25, 2)).result == R::in_order);
    // A stale heartbeat (older than what we hold) is a duplicate and moves nothing.
    c = t.on_packet(heartbeat_pkt(kSessA, 10));
    OT_CHECK(c.result == R::duplicate);
    OT_CHECK_EQ(t.expected(), std::uint64_t{27});
}

OT_TEST(tracker_end_of_session_advances_nothing) {
    SequenceTracker t;
    t.on_packet(data_pkt(kSessA, 1, 19));  // expects 20
    // The 0xFFFF marker is not 65535 messages.
    SequenceCheck c = t.on_packet(eos_pkt(kSessA, 20));
    OT_CHECK(c.result == R::in_order);
    OT_CHECK_EQ(t.expected(), std::uint64_t{20});
    // An end-of-session that discloses lost messages is a gap like any heartbeat.
    c = t.on_packet(eos_pkt(kSessA, 23));
    OT_CHECK(c.result == R::gap);
    OT_CHECK_EQ(c.missing, std::uint64_t{3});
    OT_CHECK_EQ(t.expected(), std::uint64_t{23});
}

OT_TEST(tracker_session_change_resets_sequencing_but_keeps_statistics) {
    SequenceTracker t;
    t.on_packet(data_pkt(kSessA, 1, 3));
    t.on_packet(data_pkt(kSessA, 10, 1));  // one gap of 6
    t.on_packet(data_pkt(kSessA, 2, 1));   // one duplicate
    OT_CHECK_EQ(t.gaps(), std::uint64_t{1});
    OT_CHECK_EQ(t.duplicates(), std::uint64_t{1});
    // New session restarts at 1: neither a duplicate nor a gap.
    SequenceCheck c = t.on_packet(data_pkt(kSessB, 1, 4));
    OT_CHECK(c.result == R::session_changed);
    OT_CHECK_EQ(c.expected, std::uint64_t{11});  // where session A had got to
    OT_CHECK_EQ(c.received, std::uint64_t{1});
    OT_CHECK_EQ(c.missing, std::uint64_t{0});
    OT_CHECK_EQ(t.expected(), std::uint64_t{5});
    OT_CHECK(t.on_packet(data_pkt(kSessB, 5, 1)).result == R::in_order);
    OT_CHECK(t.on_packet(data_pkt(kSessB, 9, 1)).result == R::gap);
    // Statistics accumulate across sessions.
    OT_CHECK_EQ(t.gaps(), std::uint64_t{2});
    // Going back to the old session is another change (packets of A are not "duplicates" of B).
    OT_CHECK(t.on_packet(data_pkt(kSessA, 11, 1)).result == R::session_changed);
    // A single differing byte in the session string is enough.
    Session s2 = kSessA;
    s2[9] = 'K';
    OT_CHECK(t.on_packet(data_pkt(s2, 12, 1)).result == R::session_changed);
}

OT_TEST(tracker_missing_counter_saturates) {
    SequenceTracker t;
    t.on_packet(data_pkt(kSessA, 0, 1));
    const std::uint64_t far = 0xFFFF'FFFF'FFFF'FFF0ULL;
    SequenceCheck c = t.on_packet(heartbeat_pkt(kSessA, far));
    OT_CHECK(c.result == R::gap);
    OT_CHECK_EQ(c.missing, far - 1);
    OT_CHECK_EQ(t.missing_messages(), far - 1);
    c = t.on_packet(heartbeat_pkt(kSessB, 0));  // new session, then another huge jump
    OT_CHECK(c.result == R::session_changed);
    c = t.on_packet(heartbeat_pkt(kSessB, far));
    OT_CHECK(c.result == R::gap);
    OT_CHECK_EQ(t.missing_messages(), std::numeric_limits<std::uint64_t>::max());
    OT_CHECK_EQ(t.gaps(), std::uint64_t{2});
}

// ---------------------------------------------------------------------------------------
// Randomized differential tests against naive models (fixed seeds)
// ---------------------------------------------------------------------------------------

namespace {

void put_be(Bytes& out, std::uint64_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}

struct NaiveResult {
    DecodeStatus st{DecodeStatus::ok};
    std::vector<std::pair<std::uint64_t, Bytes>> msgs;
};

NaiveResult naive_parse(const Bytes& p) {
    NaiveResult r;
    if (p.size() < 20) {
        r.st = DecodeStatus::truncated;
        return r;
    }
    std::uint64_t seq = 0;
    for (int i = 10; i < 18; ++i) seq = seq * 256 + std::to_integer<unsigned>(p[static_cast<std::size_t>(i)]);
    const unsigned count = std::to_integer<unsigned>(p[18]) * 256 + std::to_integer<unsigned>(p[19]);
    if (count == 0 || count == 0xFFFF) {
        r.st = p.size() == 20 ? DecodeStatus::ok : DecodeStatus::bad_length;
        return r;
    }
    std::size_t off = 20;
    for (unsigned i = 0; i < count; ++i) {
        if (off + 2 > p.size()) return {DecodeStatus::truncated, {}};
        const std::size_t len = std::to_integer<unsigned>(p[off]) * 256 + std::to_integer<unsigned>(p[off + 1]);
        off += 2;
        if (off + len > p.size()) return {DecodeStatus::truncated, {}};
        r.msgs.emplace_back(seq + i, Bytes(p.begin() + static_cast<std::ptrdiff_t>(off),
                                           p.begin() + static_cast<std::ptrdiff_t>(off + len)));
        off += len;
    }
    if (off != p.size()) return {DecodeStatus::bad_length, {}};
    return r;
}

}  // namespace

OT_TEST(builder_matches_naive_encoder_and_parser_roundtrips_random_packets) {
    Rng rng(0xC0FFEE);
    for (int iter = 0; iter < 3000; ++iter) {
        Session sess{};
        for (char& c : sess) c = static_cast<char>(rng.bounded(256));
        const std::uint64_t seq = rng.next();
        const std::size_t cap = static_cast<std::size_t>(rng.bounded(1600));
        Bytes buf(cap, std::byte{0xEE});
        PacketBuilder b(std::span<std::byte>(buf), sess, seq);

        // Naive model: append bytes by hand and refuse what would not fit.
        Bytes expect;
        std::vector<Bytes> msgs;
        const bool usable = cap >= kHeaderSize;
        if (usable) {
            for (char c : sess) expect.push_back(static_cast<std::byte>(c));
            put_be(expect, seq, 8);
            put_be(expect, 0, 2);
        }
        const int adds = static_cast<int>(rng.bounded(40));
        for (int i = 0; i < adds; ++i) {
            const std::size_t len = rng.chance(1, 8) ? 0 : static_cast<std::size_t>(rng.bounded(200));
            Bytes m(len);
            for (auto& x : m) x = static_cast<std::byte>(rng.bounded(256));
            const bool fits = usable && expect.size() + 2 + len <= cap;
            OT_CHECK_EQ(b.add(view(m)), fits);
            if (fits) {
                put_be(expect, len, 2);
                expect.insert(expect.end(), m.begin(), m.end());
                msgs.push_back(m);
            }
        }
        if (usable) {
            expect[18] = static_cast<std::byte>(msgs.size() >> 8);
            expect[19] = static_cast<std::byte>(msgs.size() & 0xFF);
        }
        const auto pkt = b.finish();
        OT_CHECK(Bytes(pkt.begin(), pkt.end()) == expect);
        for (std::size_t i = pkt.size(); i < buf.size(); ++i) OT_CHECK(buf[i] == std::byte{0xEE});

        if (!usable) continue;
        Header h;
        std::size_t k = 0;
        bool same = true;
        const DecodeStatus st = for_each_message(pkt, h, [&](std::uint64_t s, std::span<const std::byte> m) {
            same = same && k < msgs.size() && s == seq + k && Bytes(m.begin(), m.end()) == msgs[k];
            ++k;
        });
        OT_CHECK(st == DecodeStatus::ok);
        OT_CHECK(same);
        OT_CHECK_EQ(k, msgs.size());
        OT_CHECK(h.session == sess);
        OT_CHECK_EQ(h.sequence, seq);
    }
}

OT_TEST(for_each_matches_naive_parser_on_mutated_and_random_packets) {
    Rng rng(0xBADC0DE);
    int ok_seen = 0, trunc_seen = 0, badlen_seen = 0;
    for (int iter = 0; iter < 20000; ++iter) {
        Bytes p;
        if (rng.chance(1, 5)) {
            p.resize(static_cast<std::size_t>(rng.bounded(80)));
            for (auto& x : p) x = static_cast<std::byte>(rng.bounded(256));
        } else {
            // A valid packet made by hand, then damaged.
            const unsigned n = static_cast<unsigned>(rng.bounded(6));
            for (int i = 0; i < 10; ++i) p.push_back(static_cast<std::byte>('A' + i));
            put_be(p, rng.next(), 8);
            put_be(p, rng.chance(1, 10) ? (rng.chance(1, 2) ? 0 : 0xFFFF) : n, 2);
            for (unsigned i = 0; i < n; ++i) {
                const std::size_t len = static_cast<std::size_t>(rng.bounded(12));
                put_be(p, len, 2);
                for (std::size_t j = 0; j < len; ++j) p.push_back(static_cast<std::byte>(rng.bounded(256)));
            }
            const int damage = static_cast<int>(rng.bounded(6));
            if (damage == 1 && !p.empty()) p.resize(static_cast<std::size_t>(rng.bounded(p.size())));
            if (damage == 2) p.push_back(static_cast<std::byte>(rng.bounded(256)));
            if (damage == 3 && !p.empty()) p[static_cast<std::size_t>(rng.bounded(p.size()))] ^= static_cast<std::byte>(1u << rng.bounded(8));
            if (damage == 4 && p.size() > 20) p[20 + static_cast<std::size_t>(rng.bounded(p.size() - 20))] = static_cast<std::byte>(rng.bounded(256));
        }
        const NaiveResult want = naive_parse(p);
        std::vector<std::pair<std::uint64_t, Bytes>> got;
        Header h;
        const DecodeStatus st = for_each_message(view(p), h, [&](std::uint64_t s, std::span<const std::byte> m) {
            got.emplace_back(s, Bytes(m.begin(), m.end()));
        });
        OT_CHECK(st == want.st);
        OT_CHECK(got == want.msgs);
        ok_seen += st == DecodeStatus::ok;
        trunc_seen += st == DecodeStatus::truncated;
        badlen_seen += st == DecodeStatus::bad_length;
    }
    // The generator must actually exercise all three outcomes, or the comparison proves little.
    OT_CHECK(ok_seen > 2000);
    OT_CHECK(trunc_seen > 2000);
    OT_CHECK(badlen_seen > 500);
}

OT_TEST(tracker_matches_reference_model_on_random_streams) {
    Rng rng(0x5EEDF00D);
    for (int stream = 0; stream < 200; ++stream) {
        SequenceTracker t;
        // Reference model kept as (last_seq, last_advance) exactly as the contract words it:
        // expected = last sequence + last count. All values stay far below 2^62, so signed
        // 64-bit differences are exact and independent of unsigned comparison tricks.
        bool have = false;
        Session cur{};
        std::uint64_t last_seq = 0, last_adv = 0;
        std::uint64_t gaps = 0, dups = 0, missing = 0;
        std::uint64_t seq_base = rng.bounded(1'000'000);

        for (int i = 0; i < 300; ++i) {
            const std::uint64_t expected = last_seq + last_adv;
            Header h;
            h.session = cur;
            const std::uint64_t roll = rng.bounded(100);
            if (!have) {
                h.session = kSessA;
                h.sequence = seq_base;
            } else if (roll < 55) {
                h.sequence = expected;
            } else if (roll < 68) {
                h.sequence = expected + 1 + rng.bounded(50);
            } else if (roll < 80) {
                h.sequence = expected > 30 ? expected - 1 - rng.bounded(30) : 0;
            } else if (roll < 96) {
                h.sequence = expected;
            } else {
                h.session = (h.session == kSessA) ? kSessB : kSessA;
                h.sequence = rng.bounded(1000);
            }
            const std::uint64_t kind_roll = rng.bounded(10);
            if (kind_roll < 6) {
                h.count = static_cast<std::uint16_t>(1 + rng.bounded(12));
                h.kind = Kind::data;
            } else if (kind_roll < 9) {
                h.count = 0;
                h.kind = Kind::heartbeat;
            } else {
                h.count = kEndOfSession;
                h.kind = Kind::end_of_session;
            }
            const unsigned adv = h.kind == Kind::data ? h.count : 0;

            SequenceCheck::Result want;
            std::uint64_t want_missing = 0;
            std::uint64_t want_expected = expected;
            if (!have) {
                want = R::in_order;
                want_expected = h.sequence;
                have = true;
                cur = h.session;
                last_seq = h.sequence;
                last_adv = adv;
            } else if (h.session != cur) {
                want = R::session_changed;
                cur = h.session;
                last_seq = h.sequence;
                last_adv = adv;
            } else {
                const std::int64_t diff = static_cast<std::int64_t>(h.sequence) - static_cast<std::int64_t>(expected);
                if (diff == 0) {
                    want = R::in_order;
                    last_seq = h.sequence;
                    last_adv = adv;
                } else if (diff > 0) {
                    want = R::gap;
                    want_missing = static_cast<std::uint64_t>(diff);
                    ++gaps;
                    missing += want_missing;
                    last_seq = h.sequence;
                    last_adv = adv;
                } else {
                    want = R::duplicate;
                    ++dups;
                    if (h.sequence + adv > expected) {
                        last_seq = h.sequence;
                        last_adv = adv;
                    }
                }
            }
            const SequenceCheck got = t.on_packet(h);
            OT_CHECK(got.result == want);
            OT_CHECK_EQ(got.expected, want_expected);
            OT_CHECK_EQ(got.received, h.sequence);
            OT_CHECK_EQ(got.missing, want_missing);
            OT_CHECK_EQ(t.expected(), last_seq + last_adv);
            OT_CHECK_EQ(t.gaps(), gaps);
            OT_CHECK_EQ(t.duplicates(), dups);
            OT_CHECK_EQ(t.missing_messages(), missing);
        }
    }
}

OT_TEST_MAIN()

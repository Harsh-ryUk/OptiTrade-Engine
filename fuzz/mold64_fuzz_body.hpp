#pragma once

// Fuzz body for the MoldUDP64 codec, shared by the libFuzzer entry point (fuzz_mold64.cpp) and
// the deterministic smoke test (tests/test_mold64_fuzz_smoke.cpp). Each input is used three ways:
//
//  1. as a raw datagram: parse_header / for_each_message are checked against a naive parser
//     written independently, every delivered span must lie inside the input at the offset the
//     framing dictates, and an accepted packet must be reproducible byte for byte by
//     PacketBuilder (or write_heartbeat / write_end_of_session);
//  2. as the header of a SequenceTracker session, whose behaviour on repeats, gaps and session
//     changes is fixed by the documented rules;
//  3. as a script driving PacketBuilder against an exactly sized heap buffer, so that ASan
//     sees any write past the announced capacity.
//
// An invariant violation aborts the process, which libFuzzer reports as a crash.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "optitrade/core/decode_status.hpp"
#include "optitrade/core/endian.hpp"
#include "optitrade/net/mold64.hpp"

#define OT_MOLD64_FUZZ_CHECK(cond)                                                          \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            std::fprintf(stderr, "mold64 fuzz invariant violated: %s (%s:%d)\n", #cond,     \
                         __FILE__, __LINE__);                                               \
            std::abort();                                                                   \
        }                                                                                   \
    } while (0)

namespace optitrade::fuzz {

namespace mold64_detail {

namespace m = ::optitrade::net::mold64;

struct Verdict {
    DecodeStatus status{DecodeStatus::ok};
    std::size_t messages{0};
};

// Deliberately structured differently from the library (offsets counted upward, arithmetic
// on the remaining length done last) so that one shared slip is unlikely to hide in both.
inline Verdict reference_verdict(const std::uint8_t* d, std::size_t n) {
    if (n < m::kHeaderSize) return {DecodeStatus::truncated, 0};
    const unsigned count = (unsigned{d[18]} << 8) | d[19];
    if (count == 0 || count == 0xFFFF) {
        return {n == m::kHeaderSize ? DecodeStatus::ok : DecodeStatus::bad_length, 0};
    }
    std::size_t at = m::kHeaderSize;
    for (unsigned i = 0; i < count; ++i) {
        if (at + 2 > n) return {DecodeStatus::truncated, 0};
        const std::size_t len = (std::size_t{d[at]} << 8) | d[at + 1];
        if (at + 2 + len > n) return {DecodeStatus::truncated, 0};
        at += 2 + len;
    }
    return {at == n ? DecodeStatus::ok : DecodeStatus::bad_length, count};
}

inline void check_header_fields(const std::uint8_t* d, const m::Header& h) {
    OT_MOLD64_FUZZ_CHECK(std::memcmp(h.session.data(), d, m::kSessionSize) == 0);
    std::uint64_t seq = 0;
    for (int i = 10; i < 18; ++i) seq = (seq << 8) | d[i];
    OT_MOLD64_FUZZ_CHECK(h.sequence == seq);
    const auto count = static_cast<std::uint16_t>((unsigned{d[18]} << 8) | d[19]);
    OT_MOLD64_FUZZ_CHECK(h.count == count);
    OT_MOLD64_FUZZ_CHECK(h.kind == (count == 0        ? m::Kind::heartbeat
                                    : count == 0xFFFF ? m::Kind::end_of_session
                                                      : m::Kind::data));
}

// Stage 1: the datagram interpretation.
inline DecodeStatus check_datagram(const std::uint8_t* data, std::size_t size) {
    const auto* bytes = reinterpret_cast<const std::byte*>(data);
    const std::span<const std::byte> packet(bytes, size);

    // parse_header on its own.
    m::Header sentinel;
    sentinel.sequence = 0x1122334455667788ULL;
    sentinel.count = 4242;
    sentinel.kind = m::Kind::end_of_session;
    m::Header ph = sentinel;
    const DecodeStatus hs = m::parse_header(packet, ph);
    if (size < m::kHeaderSize) {
        OT_MOLD64_FUZZ_CHECK(hs == DecodeStatus::truncated);
        OT_MOLD64_FUZZ_CHECK(ph.sequence == sentinel.sequence && ph.count == sentinel.count &&
                             ph.kind == sentinel.kind);
    } else {
        OT_MOLD64_FUZZ_CHECK(hs == DecodeStatus::ok);
        check_header_fields(data, ph);
    }

    // for_each_message against the reference parser.
    m::Header h;
    std::vector<std::span<const std::byte>> messages;
    std::size_t next_block = m::kHeaderSize;  // where the next length prefix must sit
    const DecodeStatus st = m::for_each_message(packet, h, [&](std::uint64_t seq, std::span<const std::byte> msg) {
        OT_MOLD64_FUZZ_CHECK(seq == h.sequence + messages.size());
        OT_MOLD64_FUZZ_CHECK(next_block + 2 <= size);
        const std::size_t len = (std::size_t{data[next_block]} << 8) | data[next_block + 1];
        OT_MOLD64_FUZZ_CHECK(msg.size() == len);
        OT_MOLD64_FUZZ_CHECK(msg.data() == bytes + next_block + 2);  // zero copy, inside the input
        OT_MOLD64_FUZZ_CHECK(next_block + 2 + len <= size);
        next_block += 2 + len;
        messages.push_back(msg);
    });
    const Verdict want = reference_verdict(data, size);
    OT_MOLD64_FUZZ_CHECK(st == want.status);
    if (size >= m::kHeaderSize) check_header_fields(data, h);
    if (st != DecodeStatus::ok) {
        OT_MOLD64_FUZZ_CHECK(messages.empty());  // validate first: no partial delivery
        return st;
    }
    OT_MOLD64_FUZZ_CHECK(messages.size() == want.messages);
    if (h.kind == m::Kind::data) OT_MOLD64_FUZZ_CHECK(next_block == size);

    // Whatever parses must be reproducible by the writers (the wire format is canonical).
    std::vector<std::byte> out(size);
    if (h.kind == m::Kind::data) {
        m::PacketBuilder b{std::span<std::byte>(out), h.session, h.sequence};
        for (const auto& msg : messages) OT_MOLD64_FUZZ_CHECK(b.add(msg));
        const std::byte one{0};
        OT_MOLD64_FUZZ_CHECK(!b.add(std::span<const std::byte>(&one, 1)));  // buffer exactly full
        OT_MOLD64_FUZZ_CHECK(b.count() == h.count);
        const auto rebuilt = b.finish();
        OT_MOLD64_FUZZ_CHECK(rebuilt.size() == size && std::memcmp(rebuilt.data(), data, size) == 0);
    } else {
        const std::size_t n = h.kind == m::Kind::heartbeat
                                  ? m::write_heartbeat(std::span<std::byte>(out), h.session, h.sequence)
                                  : m::write_end_of_session(std::span<std::byte>(out), h.session, h.sequence);
        OT_MOLD64_FUZZ_CHECK(n == m::kHeaderSize && std::memcmp(out.data(), data, size) == 0);
    }
    return st;
}

// Stage 2: sequence tracking on the packet's header (only for packets that validated).
inline void check_tracker(const m::Header& h) {
    const std::uint64_t advance = h.kind == m::Kind::data ? h.count : 0;
    // Sequence arithmetic is specified for non-wrapping counters; skip the inputs that would wrap.
    if (h.sequence > std::numeric_limits<std::uint64_t>::max() - advance - 2) return;

    using R = m::SequenceCheck::Result;
    m::SequenceTracker t;
    m::SequenceCheck c = t.on_packet(h);
    OT_MOLD64_FUZZ_CHECK(c.result == R::in_order && c.expected == h.sequence && c.received == h.sequence);
    OT_MOLD64_FUZZ_CHECK(t.expected() == h.sequence + advance);

    c = t.on_packet(h);  // replay
    if (advance > 0) {
        OT_MOLD64_FUZZ_CHECK(c.result == R::duplicate && t.duplicates() == 1);
    } else {
        OT_MOLD64_FUZZ_CHECK(c.result == R::in_order && t.duplicates() == 0);
    }
    OT_MOLD64_FUZZ_CHECK(t.expected() == h.sequence + advance);

    m::Header ahead = h;  // skip exactly one message
    ahead.sequence = h.sequence + advance + 1;
    c = t.on_packet(ahead);
    OT_MOLD64_FUZZ_CHECK(c.result == R::gap && c.missing == 1 && t.gaps() == 1 && t.missing_messages() == 1);
    OT_MOLD64_FUZZ_CHECK(t.expected() == ahead.sequence + advance);

    m::Header other = h;  // a single flipped session bit is a new session
    other.session[0] = static_cast<char>(other.session[0] ^ 1);
    c = t.on_packet(other);
    OT_MOLD64_FUZZ_CHECK(c.result == R::session_changed && c.missing == 0);
    OT_MOLD64_FUZZ_CHECK(t.expected() == other.sequence + advance);
    OT_MOLD64_FUZZ_CHECK(t.gaps() == 1);  // statistics survive the change
}

// Stage 3: PacketBuilder driven by the input as a script.
//   bytes 0-1  capacity (mod 600, so both sides of the 20-byte header limit are common)
//   bytes 2-11 session, 12-19 first sequence, then repeated [len][len bytes] messages
inline void check_builder_script(const std::uint8_t* data, std::size_t size) {
    if (size < 20) return;
    const std::size_t cap = ((std::size_t{data[0]} << 8) | data[1]) % 600;
    std::array<char, m::kSessionSize> session{};
    std::memcpy(session.data(), data + 2, m::kSessionSize);
    const std::uint64_t seq = be::load64(reinterpret_cast<const std::byte*>(data) + 12);

    std::vector<std::byte> storage(cap);  // exact size: ASan flags any overrun
    m::PacketBuilder b{std::span<std::byte>(storage), session, seq};
    const bool usable = cap >= m::kHeaderSize;
    std::size_t used = usable ? m::kHeaderSize : 0;
    std::vector<std::pair<std::size_t, std::size_t>> accepted;  // (offset in input, length)

    for (std::size_t pos = 20; pos < size;) {
        const std::size_t want = data[pos++];
        const std::size_t len = std::min(want, size - pos);
        const auto* msg = reinterpret_cast<const std::byte*>(data) + pos;
        const bool fits = usable && used + 2 + len <= cap;
        OT_MOLD64_FUZZ_CHECK(b.add(std::span<const std::byte>(msg, len)) == fits);
        if (fits) {
            used += 2 + len;
            accepted.emplace_back(pos, len);
        }
        pos += len;
    }
    OT_MOLD64_FUZZ_CHECK(b.count() == accepted.size());
    const auto pkt = b.finish();
    OT_MOLD64_FUZZ_CHECK(b.finish().data() == pkt.data() && b.finish().size() == pkt.size());
    if (!usable) {
        OT_MOLD64_FUZZ_CHECK(pkt.empty());
        return;
    }
    OT_MOLD64_FUZZ_CHECK(pkt.size() == used && pkt.data() == storage.data());

    m::Header h;
    std::size_t k = 0;
    const DecodeStatus st = m::for_each_message(pkt, h, [&](std::uint64_t s, std::span<const std::byte> msg) {
        OT_MOLD64_FUZZ_CHECK(k < accepted.size());
        OT_MOLD64_FUZZ_CHECK(s == seq + k);
        OT_MOLD64_FUZZ_CHECK(msg.size() == accepted[k].second);
        OT_MOLD64_FUZZ_CHECK(msg.empty() ||
                             std::memcmp(msg.data(), data + accepted[k].first, msg.size()) == 0);
        ++k;
    });
    OT_MOLD64_FUZZ_CHECK(st == DecodeStatus::ok && k == accepted.size());
    OT_MOLD64_FUZZ_CHECK(h.session == session && h.sequence == seq);
    OT_MOLD64_FUZZ_CHECK(h.kind == (accepted.empty() ? m::Kind::heartbeat : m::Kind::data));
}

}  // namespace mold64_detail

// Returns the verdict of for_each_message on the raw input so that a driver can check its
// generator reaches every outcome; libFuzzer ignores it.
inline DecodeStatus mold64_one(const std::uint8_t* data, std::size_t size) {
    const DecodeStatus st = mold64_detail::check_datagram(data, size);
    if (st == DecodeStatus::ok) {
        net::mold64::Header h;
        (void)net::mold64::parse_header(std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), size), h);
        mold64_detail::check_tracker(h);
    }
    mold64_detail::check_builder_script(data, size);
    return st;
}

}  // namespace optitrade::fuzz

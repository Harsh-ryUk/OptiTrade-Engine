#pragma once

// MoldUDP64 v1.00 framing: header parsing, packet construction and loss detection.
//
// Wire layout (all integers big-endian):
//
//   offset  size  field
//        0    10  session            (alpha, space padded; not interpreted here)
//       10     8  sequence           (sequence number of the first message in the packet)
//       18     2  message count      (0 = heartbeat, 0xFFFF = end of session)
//       20     -  message blocks     (count x [u16 length][payload]); data packets only
//
// A heartbeat carries the sequence number of the *next* message the sender will
// publish, which is what lets a receiver notice a lost tail without waiting for
// the next data packet. End of session is identical but with count 0xFFFF; the
// 0xFFFF is a marker, not a message count.
//
// Nothing in this file allocates, throws or reads outside the span it is given.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

#include "optitrade/core/decode_status.hpp"
#include "optitrade/core/endian.hpp"

namespace optitrade::net::mold64 {

inline constexpr std::size_t kHeaderSize = 20;
inline constexpr std::size_t kSessionSize = 10;
inline constexpr std::uint16_t kEndOfSession = 0xFFFF;
// 0xFFFF is reserved for end of session, so a data packet carries at most 0xFFFE messages.
inline constexpr std::uint16_t kMaxMessageCount = 0xFFFE;
// The block length field is 16 bits wide.
inline constexpr std::size_t kMaxMessageSize = 0xFFFF;

enum class Kind : std::uint8_t { data, heartbeat, end_of_session };

struct Header {
    std::array<char, kSessionSize> session{};
    std::uint64_t sequence{};
    std::uint16_t count{};
    Kind kind{};
};

constexpr Kind kind_of(std::uint16_t count) noexcept {
    return count == 0 ? Kind::heartbeat : count == kEndOfSession ? Kind::end_of_session : Kind::data;
}

// Decodes the fixed 20-byte header only; the message blocks are not looked at (use
// for_each_message to validate them). Returns `truncated` for fewer than 20 bytes,
// in which case `out` is left untouched. Every 20-byte prefix is otherwise a legal
// header, so no other status is possible.
inline DecodeStatus parse_header(std::span<const std::byte> packet, Header& out) noexcept {
    if (packet.size() < kHeaderSize) return DecodeStatus::truncated;
    const std::byte* p = packet.data();
    std::memcpy(out.session.data(), p, kSessionSize);
    out.sequence = be::load64(p + 10);
    out.count = be::load16(p + 18);
    out.kind = kind_of(out.count);
    return DecodeStatus::ok;
}

namespace detail {

// Walks the message blocks without delivering anything. Keeping validation separate from
// delivery means a handler is never invoked for a packet that turns out to be malformed
// halfway through: the feed handler cannot end up with half of a corrupt packet applied.
//
// Status choice: running out of bytes (a length prefix or payload cut short, or fewer blocks
// than `count` promises) is `truncated`; surplus bytes (blocks beyond `count`, or any payload
// on a heartbeat / end-of-session packet) are `bad_length`.
inline DecodeStatus validate_blocks(std::span<const std::byte> packet, const Header& h) noexcept {
    const std::size_t size = packet.size();  // >= kHeaderSize, guaranteed by parse_header
    if (h.kind != Kind::data) return size == kHeaderSize ? DecodeStatus::ok : DecodeStatus::bad_length;
    std::size_t pos = kHeaderSize;
    for (std::uint16_t i = 0; i < h.count; ++i) {
        if (size - pos < 2) return DecodeStatus::truncated;
        const std::size_t len = be::load16(packet.data() + pos);
        pos += 2;
        if (size - pos < len) return DecodeStatus::truncated;
        pos += len;
    }
    return pos == size ? DecodeStatus::ok : DecodeStatus::bad_length;
}

inline std::size_t write_header(std::span<std::byte> out, const std::array<char, kSessionSize>& session,
                                std::uint64_t sequence, std::uint16_t count) noexcept {
    if (out.size() < kHeaderSize) return 0;
    std::memcpy(out.data(), session.data(), kSessionSize);
    be::store64(out.data() + 10, sequence);
    be::store16(out.data() + 18, count);
    return kHeaderSize;
}

}  // namespace detail

// Validates the whole packet first (every block length inside the packet, block count equal
// to header.count, no trailing bytes), then calls f(sequence_of_message, message_span) once
// per block in wire order. The spans point into `packet` (zero copy) and a zero-length block
// is delivered as an empty span. On any error f is never called.
//
// `hdr` is filled as soon as the 20-byte header has been read, so it describes the packet
// even when the blocks are rejected (useful for logging); it must not be fed to a
// SequenceTracker unless the return value is ok. Heartbeat and end-of-session packets
// must consist of the header alone.
template <class F>
DecodeStatus for_each_message(std::span<const std::byte> packet, Header& hdr, F&& f) noexcept {
    static_assert(std::is_invocable_v<F&, std::uint64_t, std::span<const std::byte>>,
                  "f must accept (std::uint64_t sequence, std::span<const std::byte> message)");
    if (const DecodeStatus st = parse_header(packet, hdr); st != DecodeStatus::ok) return st;
    if (const DecodeStatus st = detail::validate_blocks(packet, hdr); st != DecodeStatus::ok) return st;
    if (hdr.kind != Kind::data) return DecodeStatus::ok;

    std::size_t pos = kHeaderSize;
    for (std::uint16_t i = 0; i < hdr.count; ++i) {
        const std::size_t len = be::load16(packet.data() + pos);
        pos += 2;
        f(hdr.sequence + std::uint64_t{i}, packet.subspan(pos, len));
        pos += len;
    }
    return DecodeStatus::ok;
}

// Control packets are header-only, so they are written directly rather than through a
// PacketBuilder. Both return kHeaderSize, or 0 if `out` is too small.
inline std::size_t write_heartbeat(std::span<std::byte> out, const std::array<char, kSessionSize>& session,
                                   std::uint64_t next_sequence) noexcept {
    return detail::write_header(out, session, next_sequence, 0);
}
inline std::size_t write_end_of_session(std::span<std::byte> out,
                                        const std::array<char, kSessionSize>& session,
                                        std::uint64_t next_sequence) noexcept {
    return detail::write_header(out, session, next_sequence, kEndOfSession);
}

// Assembles one data packet in caller-owned memory. The caller bounds the packet size by the
// size of the buffer it passes (typically the path MTU minus IP/UDP headers), flushes when
// add() reports the message no longer fits, and starts a new builder at
// first_seq + count().
//
// Lifecycle: add()... then finish(). finish() seals the packet: it stores the message count,
// later calls return the same span, and further add() calls are refused so a span that has
// already been handed to the socket can never change underneath it. A packet with no
// messages finishes as a heartbeat.
class PacketBuilder {
public:
    PacketBuilder(std::span<std::byte> buffer, std::array<char, kSessionSize> session,
                  std::uint64_t first_seq) noexcept
        : buf_(buffer) {
        // A buffer that cannot hold the header leaves the builder inert (size_ == 0).
        size_ = detail::write_header(buf_, session, first_seq, 0);
    }

    // False if the message does not fit, exceeds 65535 bytes, the packet already holds
    // kMaxMessageCount messages, or the builder is sealed / inert. A refused add never
    // modifies the packet. Empty messages are legal (the parser accepts zero-length blocks).
    bool add(std::span<const std::byte> message) noexcept {
        if (sealed_ || size_ == 0 || count_ >= kMaxMessageCount) return false;
        if (message.size() > kMaxMessageSize) return false;
        const std::size_t need = 2 + message.size();
        if (need > buf_.size() - size_) return false;
        std::byte* dst = buf_.data() + size_;
        be::store16(dst, static_cast<std::uint16_t>(message.size()));
        if (!message.empty()) std::memcpy(dst + 2, message.data(), message.size());
        size_ += need;
        ++count_;
        return true;
    }

    // The finished packet (empty span if the buffer was too small for even the header).
    std::span<const std::byte> finish() noexcept {
        if (size_ == 0) return {};
        if (!sealed_) {
            be::store16(buf_.data() + 18, count_);
            sealed_ = true;
        }
        return {buf_.data(), size_};
    }

    std::uint16_t count() const noexcept { return count_; }
    // Bytes the packet occupies so far, header included (0 for an inert builder).
    std::size_t size() const noexcept { return size_; }

private:
    std::span<std::byte> buf_;
    std::size_t size_{0};
    std::uint16_t count_{0};
    bool sealed_{false};
};

struct SequenceCheck {
    enum class Result : std::uint8_t { in_order, gap, duplicate, session_changed };
    Result result{Result::in_order};
    std::uint64_t expected{};  // sequence the tracker was waiting for (previous session's for session_changed)
    std::uint64_t received{};  // sequence the packet starts at
    std::uint64_t missing{};   // messages skipped; non-zero only for a gap
};

// Detects loss on a session. Feed it the header of every successfully validated packet,
// heartbeats and end-of-session included.
//
//  * expected = sequence + count of the last accepted packet. Heartbeats and end-of-session
//    add nothing (their count is not a message count) but carry the next sequence number,
//    so a heartbeat ahead of `expected` exposes a gap that no data packet has revealed yet.
//  * The first packet ever seen only establishes the baseline and reports in_order: a
//    receiver may legitimately join mid-session.
//  * A different session string reports session_changed and restarts sequencing from that
//    packet; the cumulative counters below are deliberately kept so monitoring survives a
//    session rollover.
//  * After a gap the tracker resynchronises to the packet that revealed it, so one loss is
//    counted once.
//  * A sequence below `expected` is a duplicate (or a retransmission). If such a packet
//    reaches beyond `expected`, the new tail is still accounted for (expected advances)
//    and the caller can skip `expected - received` leading messages.
//  * Sequence numbers are treated as non-wrapping; a 64-bit counter cannot run out in practice.
class SequenceTracker {
    static constexpr std::uint64_t kMax64 = std::numeric_limits<std::uint64_t>::max();

public:
    SequenceCheck on_packet(const Header& h) noexcept {
        // Only real messages advance the sequence; the end-of-session marker is not a count.
        const std::uint64_t advance = h.kind == Kind::data ? h.count : 0;
        const std::uint64_t end = h.sequence + advance;

        SequenceCheck r;
        r.received = h.sequence;

        if (!synced_) {
            synced_ = true;
            session_ = h.session;
            expected_ = end;
            r.expected = h.sequence;
            return r;
        }
        r.expected = expected_;
        if (h.session != session_) {
            session_ = h.session;
            expected_ = end;
            r.result = SequenceCheck::Result::session_changed;
            return r;
        }
        if (h.sequence == expected_) {
            expected_ = end;
        } else if (h.sequence > expected_) {
            r.result = SequenceCheck::Result::gap;
            r.missing = h.sequence - expected_;
            ++gaps_;
            missing_ = missing_ > kMax64 - r.missing ? kMax64 : missing_ + r.missing;
            expected_ = end;
        } else {
            r.result = SequenceCheck::Result::duplicate;
            ++duplicates_;
            if (end > expected_) expected_ = end;
        }
        return r;
    }

    std::uint64_t gaps() const noexcept { return gaps_; }
    // Total messages skipped over all gaps (saturates instead of wrapping).
    std::uint64_t missing_messages() const noexcept { return missing_; }
    std::uint64_t duplicates() const noexcept { return duplicates_; }
    // Sequence number the next packet is expected to start at (0 before the first packet).
    std::uint64_t expected() const noexcept { return expected_; }

private:
    std::array<char, kSessionSize> session_{};
    std::uint64_t expected_{0};
    std::uint64_t gaps_{0};
    std::uint64_t missing_{0};
    std::uint64_t duplicates_{0};
    bool synced_{false};
};

}  // namespace optitrade::net::mold64

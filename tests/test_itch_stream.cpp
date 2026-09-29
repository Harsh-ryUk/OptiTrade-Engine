// Framing tests for the BinaryFILE-style ITCH stream decoder: partial frames,
// arbitrary chunking, zero-length and unsupported frames, malformed messages
// inside a stream, and a differential test against a naive frame walker.
//
// Frames are assembled by hand (two prefix bytes written explicitly) from
// hand-built message bytes, so the tests do not depend on the encoder.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "check.hpp"
#include "optitrade/core/rng.hpp"
#include "optitrade/itch/decoder.hpp"
#include "optitrade/itch/encoder.hpp"

namespace itch = optitrade::itch;
using optitrade::DecodeStatus;
using optitrade::Nanos;

namespace {

using Buf = std::vector<std::byte>;

template <class... T>
constexpr std::array<std::byte, sizeof...(T)> bytes(T... v) {
    return {static_cast<std::byte>(v)...};
}

// Hand-built messages (offsets as in the ITCH 5.0 tables). Their timestamp
// field, bytes 5..10, is overwritten per use so each stream entry is identifiable.
constexpr auto kS = bytes('S', 0, 0, 0, 7, 0, 0, 0, 0, 0, 0, 'O');
constexpr auto kR = bytes('R', 1, 2, 3, 4, 0, 0, 0, 0, 0, 0, 'S', 'Q', 'Q', 'Q', ' ', ' ', ' ', ' ', 'Q', 'D', 0, 0, 0,
                          100, 'Y', 'C', 'Z', ' ', 'P', 'N', ' ', '1', 'Y', 0, 0, 0, 3, 'Y');
constexpr auto kA = bytes('A', 0x12, 0x34, 0xAB, 0xCD, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                          'B', 0, 0, 1, 0xF4, 'A', 'A', 'P', 'L', ' ', ' ', ' ', ' ', 0, 0x12, 0xD6, 0x44);
constexpr auto kF = bytes('F', 0, 2, 0, 9, 0, 0, 0, 0, 0, 0, 0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0x2A, 'S', 0, 0x0F, 0x42,
                          0x3F, 'M', 'S', 'F', 'T', ' ', ' ', ' ', ' ', 0, 3, 0x0D, 0x40, 'G', 'S', 'C', 'O');
constexpr auto kE = bytes('E', 0, 5, 0, 6, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 100, 0x11, 0x22, 0x33,
                          0x44, 0x55, 0x66, 0x77, 0x88);
constexpr auto kC = bytes('C', 0, 10, 0, 11, 0, 0, 0, 0, 0, 0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0, 0, 0,
                          50, 15, 14, 13, 12, 11, 10, 9, 8, 'Y', 0, 0x0F, 0x42, 0x40);
constexpr auto kX = bytes('X', 0, 3, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0x2C, 0, 0, 0, 25);
constexpr auto kD = bytes('D', 0, 9, 0, 8, 0, 0, 0, 0, 0, 0, 0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10);
constexpr auto kU = bytes('U', 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22, 0x33, 0x33,
                          0x33, 0x33, 0x44, 0x44, 0x44, 0x44, 0, 0, 2, 0x58, 0, 0x0F, 0x42, 0x41);
constexpr auto kP = bytes('P', 0, 12, 0, 13, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'B', 0, 0, 3, 0xE8, 'T', 'S', 'L',
                          'A', ' ', ' ', ' ', ' ', 0, 0x0A, 0xBC, 0xDE, 0, 0, 0, 1, 0, 0, 0, 2);

// A typo in a hand-typed vector must fail the build, not silently shift a field.
static_assert(kS.size() == 12 && kR.size() == 39 && kA.size() == 36 && kF.size() == 40 && kE.size() == 31 &&
              kC.size() == 36 && kX.size() == 23 && kD.size() == 19 && kU.size() == 35 && kP.size() == 44);

std::array<std::span<const std::byte>, 10> all_messages() {
    return {std::span<const std::byte>(kS), std::span<const std::byte>(kR), std::span<const std::byte>(kA),
            std::span<const std::byte>(kF), std::span<const std::byte>(kE), std::span<const std::byte>(kC),
            std::span<const std::byte>(kX), std::span<const std::byte>(kD), std::span<const std::byte>(kU),
            std::span<const std::byte>(kP)};
}

// A copy of `msg` whose 48-bit timestamp is `ts`, written byte by byte.
Buf stamped(std::span<const std::byte> msg, Nanos ts) {
    Buf b(msg.begin(), msg.end());
    for (std::size_t i = 0; i < 6; ++i) b[5 + i] = static_cast<std::byte>((ts >> (8 * (5 - i))) & 0xFF);
    return b;
}

void append_frame(Buf& out, std::span<const std::byte> msg) {
    out.push_back(static_cast<std::byte>(msg.size() >> 8));
    out.push_back(static_cast<std::byte>(msg.size() & 0xFF));
    out.insert(out.end(), msg.begin(), msg.end());
}

// A frame of an unmodelled ITCH type: `len` bytes, type first, filler after.
Buf unsupported_message(char type, std::size_t len) {
    Buf b(len, std::byte{0xA5});
    b[0] = static_cast<std::byte>(type);
    return b;
}

char tag(const itch::SystemEvent&) { return 'S'; }
char tag(const itch::StockDirectory&) { return 'R'; }
char tag(const itch::AddOrder& m) { return m.has_attribution ? 'F' : 'A'; }
char tag(const itch::OrderExecuted&) { return 'E'; }
char tag(const itch::OrderExecutedPrice&) { return 'C'; }
char tag(const itch::OrderCancel&) { return 'X'; }
char tag(const itch::OrderDelete&) { return 'D'; }
char tag(const itch::OrderReplace&) { return 'U'; }
char tag(const itch::Trade&) { return 'P'; }

using Seen = std::vector<std::pair<char, Nanos>>;

// Records (type, timestamp) of every delivered message, in order.
struct Log {
    Seen seen;
    template <class M>
    void on(const M& m) {
        seen.emplace_back(tag(m), m.h.timestamp);
    }
};

// Appends (type, timestamp) of every delivered message to an external list.
struct SeenSink {
    Seen* out;
    template <class M>
    void on(const M& m) {
        out->emplace_back(tag(m), m.h.timestamp);
    }
};

// Runs the decoder on an exactly-sized copy so AddressSanitizer sees any overread.
itch::StreamResult run(std::span<const std::byte> stream, Log& log) {
    const Buf exact(stream.begin(), stream.end());
    return itch::decode_stream(exact, log);
}

struct Fed {
    Seen seen;
    std::size_t messages = 0;
    std::size_t skipped = 0;
    std::size_t leftover = 0;
};

// The way a socket reader would use the decoder: append a chunk, decode, drop
// what was consumed, repeat.
Fed feed_in_chunks(std::span<const std::byte> stream, std::size_t chunk) {
    Fed f;
    Log log;
    Buf pending;
    for (std::size_t off = 0; off < stream.size(); off += chunk) {
        const std::size_t n = std::min(chunk, stream.size() - off);
        pending.insert(pending.end(), stream.begin() + static_cast<std::ptrdiff_t>(off),
                       stream.begin() + static_cast<std::ptrdiff_t>(off + n));
        const itch::StreamResult r = run(pending, log);
        f.messages += r.messages;
        f.skipped += r.skipped;
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(r.consumed));
    }
    f.seen = std::move(log.seen);
    f.leftover = pending.size();
    return f;
}

// A stream mixing supported, unsupported, zero-length and malformed frames.
// Returns the frame boundaries too (offset of each frame's first byte).
Buf mixed_stream(std::vector<std::size_t>& frame_starts) {
    Buf s;
    Nanos ts = 1;
    auto add = [&](const Buf& msg) {
        frame_starts.push_back(s.size());
        append_frame(s, msg);
    };
    for (std::span<const std::byte> m : all_messages()) add(stamped(m, ts++));
    add(unsupported_message('H', 25));
    add(Buf{});                                   // zero-length frame
    add(stamped(kA, ts++));
    add(unsupported_message('Q', 40));
    Buf bad_side = stamped(kA, ts++);
    bad_side[19] = std::byte{'?'};
    add(bad_side);
    add(stamped(kE, ts++));
    Buf too_long = stamped(kD, ts++);
    too_long.push_back(std::byte{0});
    add(too_long);
    add(stamped(kD, ts++));
    Buf too_short(kX.begin(), kX.begin() + 22);
    add(too_short);
    add(stamped(kP, ts++));
    return s;
}

}  // namespace

OT_TEST(empty_buffer_consumes_nothing) {
    Log log;
    const itch::StreamResult r = itch::decode_stream(std::span<const std::byte>{}, log);
    OT_CHECK_EQ(r.consumed, std::size_t{0});
    OT_CHECK_EQ(r.messages, std::size_t{0});
    OT_CHECK_EQ(r.skipped, std::size_t{0});
    OT_CHECK(r.last_error == DecodeStatus::ok);
    OT_CHECK(log.seen.empty());
}

OT_TEST(fewer_than_two_bytes_is_a_partial_prefix) {
    Log log;
    const Buf one{std::byte{0}};
    const itch::StreamResult r = run(one, log);
    OT_CHECK_EQ(r.consumed, std::size_t{0});
    OT_CHECK_EQ(r.messages + r.skipped, std::size_t{0});
}

OT_TEST(prefix_without_its_body_is_not_consumed) {
    Log log;
    const Buf s{std::byte{0}, std::byte{36}};  // announces a 36 byte message, none present
    const itch::StreamResult r = run(s, log);
    OT_CHECK_EQ(r.consumed, std::size_t{0});
    OT_CHECK_EQ(r.messages + r.skipped, std::size_t{0});
    OT_CHECK(log.seen.empty());
}

OT_TEST(each_supported_type_decodes_from_a_single_frame) {
    const char types[] = {'S', 'R', 'A', 'F', 'E', 'C', 'X', 'D', 'U', 'P'};
    std::size_t i = 0;
    for (std::span<const std::byte> m : all_messages()) {
        Buf s;
        append_frame(s, stamped(m, 77));
        OT_CHECK_EQ(s.size(), m.size() + 2);
        Log log;
        const itch::StreamResult r = run(s, log);
        OT_CHECK_EQ(r.consumed, s.size());
        OT_CHECK_EQ(r.messages, std::size_t{1});
        OT_CHECK_EQ(r.skipped, std::size_t{0});
        OT_CHECK(r.last_error == DecodeStatus::ok);
        OT_CHECK_EQ(log.seen.size(), std::size_t{1});
        if (log.seen.size() == 1) {
            OT_CHECK_EQ(log.seen[0].first, types[i]);
            OT_CHECK_EQ(log.seen[0].second, Nanos{77});
        }
        ++i;
    }
}

OT_TEST(frames_are_delivered_in_stream_order) {
    Buf s;
    Nanos ts = 100;
    Seen expected;
    const char types[] = {'P', 'D', 'A', 'S', 'F', 'U', 'C', 'X', 'E', 'R'};
    const auto msgs = all_messages();
    const std::size_t order[] = {9, 7, 2, 0, 3, 8, 5, 6, 4, 1};  // indices into msgs, matching `types`
    for (std::size_t k = 0; k < 10; ++k) {
        append_frame(s, stamped(msgs[order[k]], ts));
        expected.emplace_back(types[k], ts);
        ++ts;
    }
    Log log;
    const itch::StreamResult r = run(s, log);
    OT_CHECK_EQ(r.consumed, s.size());
    OT_CHECK_EQ(r.messages, std::size_t{10});
    OT_CHECK(log.seen == expected);
}

OT_TEST(partial_trailing_frame_is_left_for_the_caller_at_every_cut) {
    std::vector<std::size_t> starts;
    Buf s;
    Nanos ts = 1;
    for (std::span<const std::byte> m : all_messages()) {
        starts.push_back(s.size());
        append_frame(s, stamped(m, ts++));
    }
    starts.push_back(s.size());  // end boundary

    for (std::size_t cut = 0; cut <= s.size(); ++cut) {
        // Largest frame boundary at or before the cut.
        std::size_t boundary = 0;
        std::size_t complete = 0;
        for (std::size_t k = 0; k < starts.size(); ++k) {
            if (starts[k] <= cut) {
                boundary = starts[k];
                complete = k;
            }
        }
        Log head;
        const itch::StreamResult r = run(std::span<const std::byte>(s.data(), cut), head);
        OT_CHECK_EQ(r.consumed, boundary);
        OT_CHECK_EQ(r.messages, complete);
        OT_CHECK_EQ(head.seen.size(), complete);
        OT_CHECK_EQ(r.skipped, std::size_t{0});

        // Resume: the unconsumed tail plus the rest of the stream completes the job.
        Log tail;
        const itch::StreamResult r2 = run(std::span<const std::byte>(s.data() + r.consumed, s.size() - r.consumed), tail);
        OT_CHECK_EQ(r.consumed + r2.consumed, s.size());
        OT_CHECK_EQ(head.seen.size() + tail.seen.size(), std::size_t{10});
    }
}

OT_TEST(a_valid_message_cut_one_byte_short_is_not_delivered) {
    Buf s;
    append_frame(s, stamped(kA, 5));
    Log log;
    const itch::StreamResult r = run(std::span<const std::byte>(s.data(), s.size() - 1), log);
    OT_CHECK_EQ(r.consumed, std::size_t{0});
    OT_CHECK_EQ(r.messages + r.skipped, std::size_t{0});
    OT_CHECK(log.seen.empty());
}

OT_TEST(any_chunking_yields_the_same_messages) {
    std::vector<std::size_t> starts;
    const Buf s = mixed_stream(starts);
    Log whole;
    const itch::StreamResult ref = run(s, whole);
    OT_CHECK_EQ(ref.consumed, s.size());
    OT_CHECK_EQ(ref.messages, std::size_t{10 + 1 + 1 + 1 + 1});  // 10 types, A, E, D, P
    OT_CHECK_EQ(ref.skipped, std::size_t{6});  // H, zero-length, Q, bad side, too long, too short

    for (std::size_t chunk = 1; chunk <= s.size() + 1; ++chunk) {
        const Fed f = feed_in_chunks(s, chunk);
        OT_CHECK(f.seen == whole.seen);
        OT_CHECK_EQ(f.messages, ref.messages);
        OT_CHECK_EQ(f.skipped, ref.skipped);
        OT_CHECK_EQ(f.leftover, std::size_t{0});
    }
}

OT_TEST(zero_length_frames_are_skipped) {
    struct Case { const char* name; Buf stream; std::size_t messages; std::size_t skipped; };
    Buf lead{std::byte{0}, std::byte{0}};
    append_frame(lead, stamped(kD, 1));
    Buf mid;
    append_frame(mid, stamped(kD, 1));
    append_frame(mid, Buf{});
    append_frame(mid, stamped(kD, 2));
    Buf trail;
    append_frame(trail, stamped(kD, 1));
    trail.push_back(std::byte{0});
    trail.push_back(std::byte{0});
    Buf run_of_zeros(2 * 7, std::byte{0});
    Buf only_zeros(2, std::byte{0});
    const Case cases[] = {
        {"leading", lead, 1, 1},
        {"between", mid, 2, 1},
        {"trailing", trail, 1, 1},
        {"run of seven", run_of_zeros, 0, 7},
        {"single", only_zeros, 0, 1},
    };
    for (const Case& c : cases) {
        Log log;
        const itch::StreamResult r = run(c.stream, log);
        OT_CHECK_EQ(r.consumed, c.stream.size());
        OT_CHECK_EQ(r.messages, c.messages);
        OT_CHECK_EQ(r.skipped, c.skipped);
        OT_CHECK(r.last_error == DecodeStatus::truncated);
        OT_CHECK_EQ(log.seen.size(), c.messages);
    }
}

OT_TEST(unsupported_but_valid_itch_types_are_skipped_not_fatal) {
    // Message lengths from the ITCH 5.0 tables: trading action, Reg SHO, market
    // participant position, circuit breaker decline / status, IPO quoting period,
    // LULD collar, operational halt, cross trade, broken trade, NOII, RPII, DLCR.
    struct Unsupported { char type; std::size_t len; };
    const Unsupported types[] = {{'H', 25}, {'Y', 20}, {'L', 26}, {'V', 35}, {'W', 12}, {'K', 28}, {'J', 35},
                                 {'h', 21}, {'Q', 40}, {'B', 19}, {'I', 50}, {'N', 20}, {'O', 48}};
    Buf s;
    Nanos ts = 1;
    Seen expected;
    for (const Unsupported& u : types) {
        append_frame(s, unsupported_message(u.type, u.len));
        append_frame(s, stamped(kD, ts));  // a supported neighbour after every skipped frame
        expected.emplace_back('D', ts);
        ++ts;
    }
    Log log;
    const itch::StreamResult r = run(s, log);
    OT_CHECK_EQ(r.consumed, s.size());
    OT_CHECK_EQ(r.messages, std::size_t{13});
    OT_CHECK_EQ(r.skipped, std::size_t{13});
    OT_CHECK(r.last_error == DecodeStatus::unknown_type);
    OT_CHECK(log.seen == expected);
}

OT_TEST(malformed_messages_are_isolated_to_their_own_frame) {
    struct Case { const char* name; Buf msg; DecodeStatus why; };
    Buf bad_side = stamped(kA, 0);
    bad_side[19] = std::byte{'Z'};
    Buf zero_shares = stamped(kF, 0);
    for (std::size_t i = 20; i < 24; ++i) zero_shares[i] = std::byte{0};
    Buf trade_side = stamped(kP, 0);
    trade_side[19] = std::byte{0};
    Buf a_short(kA.begin(), kA.begin() + 35);
    Buf a_long = stamped(kA, 0);
    a_long.push_back(std::byte{0});
    Buf type_only{std::byte{'A'}};
    const Case cases[] = {
        {"bad side on A", bad_side, DecodeStatus::bad_field},
        {"zero shares on F", zero_shares, DecodeStatus::bad_field},
        {"bad side on P", trade_side, DecodeStatus::bad_field},
        {"A one byte short", a_short, DecodeStatus::truncated},
        {"A one byte long", a_long, DecodeStatus::bad_length},
        {"bare type byte", type_only, DecodeStatus::truncated},
    };
    for (const Case& c : cases) {
        Buf s;
        append_frame(s, stamped(kE, 1));
        append_frame(s, c.msg);
        append_frame(s, stamped(kE, 2));
        Log log;
        const itch::StreamResult r = run(s, log);
        OT_CHECK_EQ(r.consumed, s.size());
        OT_CHECK_EQ(r.messages, std::size_t{2});
        OT_CHECK_EQ(r.skipped, std::size_t{1});
        OT_CHECK(r.last_error == c.why);
        OT_CHECK_EQ(log.seen.size(), std::size_t{2});
    }
}

OT_TEST(last_error_is_the_most_recent_failure_and_survives_later_successes) {
    Buf s;
    append_frame(s, unsupported_message('H', 25));  // unknown_type
    append_frame(s, Buf{});                          // truncated (zero length)
    append_frame(s, stamped(kD, 1));                 // ok: must not clear last_error
    Log log;
    const itch::StreamResult r = run(s, log);
    OT_CHECK_EQ(r.skipped, std::size_t{2});
    OT_CHECK(r.last_error == DecodeStatus::truncated);

    Buf t;
    append_frame(t, Buf{});
    append_frame(t, unsupported_message('H', 25));
    Log log2;
    OT_CHECK(run(t, log2).last_error == DecodeStatus::unknown_type);
}

OT_TEST(largest_possible_frame) {
    // 0xFFFF is the biggest length the prefix can express.
    Buf s(2 + 0xFFFF, std::byte{0x11});
    s[0] = std::byte{0xFF};
    s[1] = std::byte{0xFF};
    s[2] = std::byte{'H'};
    Log log;
    itch::StreamResult r = run(s, log);
    OT_CHECK_EQ(r.consumed, s.size());
    OT_CHECK_EQ(r.skipped, std::size_t{1});
    OT_CHECK(r.last_error == DecodeStatus::unknown_type);

    // One byte short: the frame is incomplete, so nothing is consumed.
    r = run(std::span<const std::byte>(s.data(), s.size() - 1), log);
    OT_CHECK_EQ(r.consumed, std::size_t{0});
    OT_CHECK_EQ(r.skipped, std::size_t{0});

    // The same length announced for a supported type is a length error, not an overread.
    s[2] = std::byte{'A'};
    r = run(s, log);
    OT_CHECK_EQ(r.consumed, s.size());
    OT_CHECK(r.last_error == DecodeStatus::bad_length);
}

OT_TEST(every_type_byte_with_a_mismatched_length_is_skipped_cleanly) {
    for (int t = 0; t < 256; ++t) {
        Buf s;
        Buf msg(20, std::byte{0x33});  // no supported type is 20 bytes long
        msg[0] = static_cast<std::byte>(t);
        append_frame(s, msg);
        Log log;
        const itch::StreamResult r = run(s, log);
        OT_CHECK_EQ(r.consumed, s.size());
        OT_CHECK_EQ(r.messages, std::size_t{0});
        OT_CHECK_EQ(r.skipped, std::size_t{1});
        OT_CHECK(log.seen.empty());
    }
}

OT_TEST(encoder_output_round_trips_through_the_stream_decoder) {
    optitrade::Rng rng(20240229);
    Buf s(4096);
    Buf stream;
    Seen expected;
    for (std::uint64_t i = 1; i <= 1000; ++i) {
        std::size_t n = 0;
        char t = 0;
        switch (rng.bounded(4)) {
            case 0: {
                itch::AddOrder m{};
                m.h.timestamp = i;
                m.shares = 1 + static_cast<optitrade::Qty>(rng.bounded(1000));
                m.has_attribution = rng.chance(1, 2);
                m.price = static_cast<optitrade::Price>(rng.bounded(2'000'000));
                n = itch::encode_framed(m, s);
                t = m.has_attribution ? 'F' : 'A';
                break;
            }
            case 1: {
                itch::OrderDelete m{};
                m.h.timestamp = i;
                n = itch::encode_framed(m, s);
                t = 'D';
                break;
            }
            case 2: {
                itch::OrderExecuted m{};
                m.h.timestamp = i;
                n = itch::encode_framed(m, s);
                t = 'E';
                break;
            }
            default: {
                itch::Trade m{};
                m.h.timestamp = i;
                n = itch::encode_framed(m, s);
                t = 'P';
                break;
            }
        }
        OT_CHECK(n > 2);
        stream.insert(stream.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
        expected.emplace_back(t, i);
    }
    Log log;
    const itch::StreamResult r = run(stream, log);
    OT_CHECK_EQ(r.consumed, stream.size());
    OT_CHECK_EQ(r.messages, std::size_t{1000});
    OT_CHECK_EQ(r.skipped, std::size_t{0});
    OT_CHECK(log.seen == expected);
}

namespace {

struct RefFrame {
    std::size_t offset;  // of the message body
    std::size_t length;
};

// Naive frame walker used as the oracle for the differential test. It shares no
// code with decode_stream and computes the bounds by addition instead of subtraction.
std::vector<RefFrame> reference_frames(std::span<const std::byte> b, std::size_t& consumed) {
    std::vector<RefFrame> frames;
    std::size_t pos = 0;
    while (pos + 2 <= b.size()) {
        const std::size_t len = std::to_integer<std::size_t>(b[pos]) * 256 + std::to_integer<std::size_t>(b[pos + 1]);
        if (pos + 2 + len > b.size()) break;
        frames.push_back({pos + 2, len});
        pos += 2 + len;
    }
    consumed = pos;
    return frames;
}

Buf random_stream(optitrade::Rng& rng) {
    Buf s;
    if (rng.chance(1, 4)) {  // pure noise
        s.resize(rng.bounded(300));
        for (auto& x : s) x = static_cast<std::byte>(rng.next());
        return s;
    }
    const char types[] = {'S', 'R', 'A', 'F', 'E', 'C', 'X', 'D', 'U', 'P', 'H', 'Q', 'I', '?'};
    const std::size_t frames = rng.bounded(12);
    for (std::size_t k = 0; k < frames; ++k) {
        const char type = types[rng.bounded(sizeof types)];
        std::size_t len = itch::message_length(type);
        if (len == 0 || rng.chance(1, 5)) len = rng.bounded(60);
        else if (rng.chance(1, 8)) {
            if (rng.chance(1, 2)) ++len;
            else --len;
        }
        Buf msg(len);
        for (auto& x : msg) x = static_cast<std::byte>(rng.next());
        if (len > 0) msg[0] = static_cast<std::byte>(type);
        if (len > 19 && rng.chance(3, 4)) msg[19] = static_cast<std::byte>(rng.chance(1, 2) ? 'B' : 'S');
        append_frame(s, msg);
    }
    if (rng.chance(1, 2) && !s.empty()) s.resize(s.size() - rng.bounded(std::min<std::size_t>(s.size(), 30)));
    return s;
}

}  // namespace

OT_TEST(random_streams_match_a_naive_frame_walker_and_are_chunk_invariant) {
    optitrade::Rng rng(0xFEEDFACECAFEBEEFULL);
    std::size_t total_ok = 0;
    std::size_t total_skipped = 0;
    std::size_t total_partial = 0;
    for (int iter = 0; iter < 20'000; ++iter) {
        const Buf s = random_stream(rng);

        Log log;
        const itch::StreamResult r = run(s, log);

        std::size_t ref_consumed = 0;
        const std::vector<RefFrame> frames = reference_frames(s, ref_consumed);
        Seen expected;
        SeenSink sink{&expected};
        std::size_t ok = 0;
        std::size_t skipped = 0;
        DecodeStatus last = DecodeStatus::ok;
        for (const RefFrame& f : frames) {
            const DecodeStatus st = itch::decode(std::span<const std::byte>(s.data() + f.offset, f.length), sink);
            if (st == DecodeStatus::ok) {
                ++ok;
            } else {
                ++skipped;
                last = st;
            }
        }
        OT_CHECK_EQ(r.consumed, ref_consumed);
        OT_CHECK(r.consumed <= s.size());
        OT_CHECK_EQ(r.messages, ok);
        OT_CHECK_EQ(r.skipped, skipped);
        OT_CHECK_EQ(r.messages + r.skipped, frames.size());
        OT_CHECK(r.last_error == last);
        OT_CHECK(log.seen == expected);
        total_ok += ok;
        total_skipped += skipped;
        if (ref_consumed < s.size()) ++total_partial;

        if (iter % 4 == 0) {
            const std::size_t chunk = 1 + static_cast<std::size_t>(rng.bounded(40));
            const Fed f = feed_in_chunks(s, chunk);
            OT_CHECK(f.seen == expected);
            OT_CHECK_EQ(f.messages, ok);
            OT_CHECK_EQ(f.skipped, skipped);
            OT_CHECK_EQ(f.leftover, s.size() - ref_consumed);
        }
    }
    // The generator must reach all three outcomes or the comparison proves little.
    OT_CHECK(total_ok > 20'000);
    OT_CHECK(total_skipped > 20'000);
    OT_CHECK(total_partial > 2'000);
}

OT_TEST_MAIN()

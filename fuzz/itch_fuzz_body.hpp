#pragma once

// Fuzz body for the ITCH codec, shared by the libFuzzer entry point
// (fuzz_itch.cpp) and the deterministic smoke test (test_itch_fuzz_smoke.cpp).
//
// One input is examined two ways: as a single unframed message and as a
// BinaryFILE stream. Invariants, each of which aborts the process when broken:
//
//  * nothing crashes or reads out of bounds (the sanitizers see to that);
//  * the status of a single message follows from its length and type alone;
//  * `bad_field` is reported exactly when the side or share count is illegal;
//  * every message that decodes re-encodes to bytes that decode to an identical
//    struct, and those bytes equal the input (except the one byte, the
//    printable flag of 'C', that the decoder normalises);
//  * decode_stream never consumes more than it was given, stops on a frame
//    boundary, and agrees frame by frame with a naive walker;
//  * feeding the stream in pieces, the way a socket reader would, yields the
//    same messages as one call.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#include "optitrade/itch/decoder.hpp"
#include "optitrade/itch/encoder.hpp"

#define ITCH_FUZZ_CHECK(cond)                                                                   \
    do {                                                                                        \
        if (!(cond)) {                                                                          \
            std::fprintf(stderr, "itch fuzz invariant violated: %s (%s:%d)\n", #cond, __FILE__, \
                         __LINE__);                                                             \
            std::abort();                                                                       \
        }                                                                                       \
    } while (0)

namespace optitrade::fuzz {
namespace itch_impl {

using Bytes = std::span<const std::byte>;

struct CountOnly {
    std::size_t n = 0;
    template <class M>
    void on(const M&) noexcept {
        ++n;
    }
};

// Receives exactly one message of type M from a decode and remembers it.
template <class M>
struct Capture {
    M value{};
    int matching = 0;
    int other = 0;
    void on(const M& m) noexcept {
        value = m;
        ++matching;
    }
    template <class X>
    void on(const X&) noexcept {
        ++other;
    }
};

// Handler that verifies the encode/decode round trip of everything it receives.
struct RoundTrip {
    std::size_t delivered = 0;
    std::array<std::byte, itch::kMaxMessageLength> last{};
    std::size_t last_len = 0;

    template <class M>
    void on(const M& m) noexcept {
        ++delivered;
        last_len = itch::encode(m, std::span<std::byte>(last));
        ITCH_FUZZ_CHECK(last_len != 0);
        ITCH_FUZZ_CHECK(itch::message_length(static_cast<char>(last[0])) == last_len);

        Capture<M> again;
        ITCH_FUZZ_CHECK(itch::decode(Bytes(last.data(), last_len), again) == DecodeStatus::ok);
        ITCH_FUZZ_CHECK(again.matching == 1 && again.other == 0);
        ITCH_FUZZ_CHECK(again.value == m);

        // Encoding the struct that came back is bit-identical: the encoder is a
        // pure function of the struct.
        std::array<std::byte, itch::kMaxMessageLength> second{};
        ITCH_FUZZ_CHECK(itch::encode(again.value, std::span<std::byte>(second)) == last_len);
        ITCH_FUZZ_CHECK(std::memcmp(second.data(), last.data(), last_len) == 0);
    }
};

inline char type_of(Bytes b) noexcept { return static_cast<char>(std::to_integer<unsigned char>(b[0])); }

// Status implied by length and type alone (independent of field contents).
inline void check_length_rules(Bytes in, DecodeStatus st) {
    if (in.empty()) {
        ITCH_FUZZ_CHECK(st == DecodeStatus::truncated);
        return;
    }
    const std::size_t len = itch::message_length(type_of(in));
    if (len == 0) {
        ITCH_FUZZ_CHECK(st == DecodeStatus::unknown_type);
    } else if (in.size() < len) {
        ITCH_FUZZ_CHECK(st == DecodeStatus::truncated);
    } else if (in.size() > len) {
        ITCH_FUZZ_CHECK(st == DecodeStatus::bad_length);
    } else {
        ITCH_FUZZ_CHECK(st == DecodeStatus::ok || st == DecodeStatus::bad_field);
    }
}

// True when the field rules of the spec make this exact-length message illegal.
inline bool has_illegal_field(Bytes in) noexcept {
    const char type = type_of(in);
    if (type == 'A' || type == 'F' || type == 'P') {
        const auto side = std::to_integer<unsigned char>(in[19]);
        if (side != 'B' && side != 'S') return true;
    }
    if (type == 'A' || type == 'F') {
        if (in[20] == std::byte{0} && in[21] == std::byte{0} && in[22] == std::byte{0} && in[23] == std::byte{0}) {
            return true;
        }
    }
    return false;
}

inline void check_single_message(Bytes in) {
    RoundTrip h;
    const DecodeStatus st = itch::decode(in, h);
    check_length_rules(in, st);
    ITCH_FUZZ_CHECK(h.delivered == (st == DecodeStatus::ok ? 1u : 0u));

    if (!in.empty() && in.size() == itch::message_length(type_of(in))) {
        ITCH_FUZZ_CHECK((st == DecodeStatus::bad_field) == has_illegal_field(in));
    }
    if (st != DecodeStatus::ok) return;

    ITCH_FUZZ_CHECK(h.last_len == in.size());
    const char type = type_of(in);
    for (std::size_t i = 0; i < in.size(); ++i) {
        std::byte want = in[i];
        // The printable flag is a boolean in memory: any byte but 'Y' reads as 'N'.
        if (type == 'C' && i == 31 && want != std::byte{'Y'}) want = std::byte{'N'};
        ITCH_FUZZ_CHECK(h.last[i] == want);
    }
}

// Every prefix (up to the longest message plus slack) obeys the length rules.
inline void check_prefixes(Bytes in) {
    const std::size_t limit = std::min<std::size_t>(in.size(), itch::kMaxMessageLength + 2);
    for (std::size_t k = 0; k <= limit; ++k) {
        CountOnly c;
        const Bytes prefix = in.first(k);
        check_length_rules(prefix, itch::decode(prefix, c));
    }
}

inline void check_stream(Bytes in) {
    RoundTrip h;
    const itch::StreamResult r = itch::decode_stream(in, h);
    ITCH_FUZZ_CHECK(r.consumed <= in.size());
    ITCH_FUZZ_CHECK(h.delivered == r.messages);

    // Naive walker: frame boundaries by addition, each frame through decode().
    std::size_t pos = 0;
    std::size_t ok = 0;
    std::size_t skipped = 0;
    DecodeStatus last = DecodeStatus::ok;
    while (pos + itch::kFrameHeaderSize <= in.size()) {
        const std::size_t len =
            std::to_integer<std::size_t>(in[pos]) * 256 + std::to_integer<std::size_t>(in[pos + 1]);
        if (pos + itch::kFrameHeaderSize + len > in.size()) break;
        CountOnly c;
        const DecodeStatus st = itch::decode(in.subspan(pos + itch::kFrameHeaderSize, len), c);
        if (st == DecodeStatus::ok) {
            ++ok;
        } else {
            ++skipped;
            last = st;
        }
        pos += itch::kFrameHeaderSize + len;
    }
    ITCH_FUZZ_CHECK(r.consumed == pos);
    ITCH_FUZZ_CHECK(r.messages == ok);
    ITCH_FUZZ_CHECK(r.skipped == skipped);
    ITCH_FUZZ_CHECK(r.last_error == last);

    // Feed the same bytes in pieces, dropping what was consumed each time. The two
    // chunk sizes are derived from the input so the fuzzer can steer the split
    // points; the unit tests cover every chunk size exhaustively.
    const std::size_t chunks[] = {
        in.empty() ? std::size_t{1} : 1 + std::to_integer<std::size_t>(in.front()) % 40,
        in.empty() ? std::size_t{1} : 1 + (in.size() ^ std::to_integer<std::size_t>(in.back())) % 97};
    for (const std::size_t chunk : chunks) {
        // Reused across inputs so the steady state allocates nothing. Every decode
        // still sees an exact-length buffer, so a read past the announced data trips
        // the sanitizer.
        static thread_local std::vector<std::byte> pending;
        pending.clear();
        CountOnly c;
        std::size_t messages = 0;
        std::size_t dropped = 0;
        for (std::size_t off = 0; off < in.size(); off += chunk) {
            const std::size_t n = std::min(chunk, in.size() - off);
            pending.insert(pending.end(), in.begin() + static_cast<std::ptrdiff_t>(off),
                           in.begin() + static_cast<std::ptrdiff_t>(off + n));
            const itch::StreamResult part = itch::decode_stream(pending, c);
            ITCH_FUZZ_CHECK(part.consumed <= pending.size());
            messages += part.messages;
            dropped += part.skipped;
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(part.consumed));
        }
        ITCH_FUZZ_CHECK(messages == r.messages);
        ITCH_FUZZ_CHECK(dropped == r.skipped);
        ITCH_FUZZ_CHECK(c.n == r.messages);
        ITCH_FUZZ_CHECK(pending.size() == in.size() - r.consumed);
    }
}

}  // namespace itch_impl

inline void itch_one(const std::uint8_t* data, std::size_t size) {
    static_assert(sizeof(std::uint8_t) == sizeof(std::byte));
    const itch_impl::Bytes in(reinterpret_cast<const std::byte*>(data), size);
    itch_impl::check_single_message(in);
    itch_impl::check_prefixes(in);
    itch_impl::check_stream(in);
}

}  // namespace optitrade::fuzz

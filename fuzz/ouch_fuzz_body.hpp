#pragma once

// Fuzz body for the OUCH codec, shared by the libFuzzer entry (fuzz_ouch.cpp) and
// the deterministic smoke test (tests/test_ouch_fuzz_smoke.cpp).
//
// Arbitrary bytes are pushed through every decoder entry point. The invariants:
//   * no crash, no read outside the input (AddressSanitizer does the watching);
//   * the status follows the documented order: empty/short -> truncated, unknown
//     type byte -> unknown_type, long -> bad_length, else ok or bad_field;
//   * the handler runs exactly once when the status is ok and never otherwise;
//   * whatever decodes re-encodes to the same bytes. The one documented loss is
//     the short-sale side letter: 'E' comes back as 'T' on Enter Order, and 'T'/'E'
//     come back as 'S' on Accepted/Replaced (the message types carry no field for
//     it). Every other byte of every message is a meaningful field;
//   * encoding into a buffer one byte short returns 0, and the framed encoding is
//     the length prefix plus the same bytes;
//   * the stream decoder agrees with an independent frame walk, and splitting the
//     input at any point and carrying the unconsumed tail gives the same totals.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <type_traits>
#include <vector>

#include "optitrade/ouch/codec.hpp"

#ifndef OUCH_FUZZ_CHECK
#define OUCH_FUZZ_CHECK(cond)                                                              \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "ouch fuzz invariant violated: %s (%s:%d)\n", #cond,      \
                         __FILE__, __LINE__);                                              \
            std::abort();                                                                  \
        }                                                                                  \
    } while (0)
#endif

namespace optitrade::fuzz {
namespace ouch_body {

using Wire = std::vector<std::byte>;

inline char ch(std::byte b) { return static_cast<char>(std::to_integer<unsigned char>(b)); }

// Applies the documented lossy side mapping to a copy of the input.
template <class M>
void normalise(Wire& wire) {
    if constexpr (std::is_same_v<M, ouch::EnterOrder>) {
        if (ch(wire[15]) == 'E') wire[15] = static_cast<std::byte>('T');
    } else if constexpr (std::is_same_v<M, ouch::Accepted> || std::is_same_v<M, ouch::Replaced>) {
        if (ch(wire[23]) == 'T' || ch(wire[23]) == 'E') wire[23] = static_cast<std::byte>('S');
    }
}

template <class M>
struct Grab : ouch::NullHandler {
    using ouch::NullHandler::on;
    M value{};
    int calls = 0;
    void on(const M& m) noexcept {
        value = m;
        ++calls;
    }
};

// Checks one decoded message against the bytes it came from.
struct RoundTrip {
    std::span<const std::byte> src;
    bool inbound;
    int calls = 0;

    template <class M>
    void on(const M& m) {
        ++calls;
        const std::size_t n = src.size();

        Wire exact(n);  // heap block of exactly n bytes: any overrun is reported
        OUCH_FUZZ_CHECK(ouch::encode(m, std::span<std::byte>(exact)) == n);
        Wire expect(src.begin(), src.end());
        normalise<M>(expect);
        OUCH_FUZZ_CHECK(exact == expect);

        Wire shorter(n - 1);
        OUCH_FUZZ_CHECK(ouch::encode(m, std::span<std::byte>(shorter)) == 0);

        Wire framed(n + 2);
        OUCH_FUZZ_CHECK(ouch::encode_framed(m, std::span<std::byte>(framed)) == n + 2);
        OUCH_FUZZ_CHECK(static_cast<std::size_t>(std::to_integer<unsigned>(framed[0])) * 256 +
                            std::to_integer<unsigned>(framed[1]) == n);
        OUCH_FUZZ_CHECK(Wire(framed.begin() + 2, framed.end()) == exact);

        // Decoding the re-encoded bytes yields the very same value.
        Grab<M> again;
        const DecodeStatus st = inbound ? ouch::decode_inbound(std::span<const std::byte>(exact), again)
                                        : ouch::decode_outbound(std::span<const std::byte>(exact), again);
        OUCH_FUZZ_CHECK(st == DecodeStatus::ok);
        OUCH_FUZZ_CHECK(again.calls == 1);
        OUCH_FUZZ_CHECK(again.value == m);
    }
};

inline void check_message(std::span<const std::byte> all, bool inbound) {
    RoundTrip rt{all, inbound};
    const DecodeStatus st = inbound ? ouch::decode_inbound(all, rt) : ouch::decode_outbound(all, rt);

    const char type = all.empty() ? '\0' : ch(all[0]);
    const std::size_t want = all.empty() ? 0 : (inbound ? ouch::inbound_length(type) : ouch::outbound_length(type));
    if (all.empty()) {
        OUCH_FUZZ_CHECK(st == DecodeStatus::truncated);
    } else if (want == 0) {
        OUCH_FUZZ_CHECK(st == DecodeStatus::unknown_type);
    } else if (all.size() < want) {
        OUCH_FUZZ_CHECK(st == DecodeStatus::truncated);
    } else if (all.size() > want) {
        OUCH_FUZZ_CHECK(st == DecodeStatus::bad_length);
    } else {
        OUCH_FUZZ_CHECK(st == DecodeStatus::ok || st == DecodeStatus::bad_field);
        if (st == DecodeStatus::bad_field) {
            // Only messages that carry a constrained field can fail on content.
            OUCH_FUZZ_CHECK(inbound ? (type == 'O' || type == 'U') : (type == 'A' || type == 'U'));
        }
    }
    OUCH_FUZZ_CHECK(rt.calls == (st == DecodeStatus::ok ? 1 : 0));

    if (st == DecodeStatus::ok) {
        // A good message with a byte missing or appended must be refused.
        Wire cut(all.begin(), all.end() - 1);
        ouch::NullHandler nh;
        OUCH_FUZZ_CHECK((inbound ? ouch::decode_inbound(std::span<const std::byte>(cut), nh)
                                 : ouch::decode_outbound(std::span<const std::byte>(cut), nh)) ==
                        DecodeStatus::truncated);
        Wire grown(all.begin(), all.end());
        grown.push_back(std::byte{0});
        OUCH_FUZZ_CHECK((inbound ? ouch::decode_inbound(std::span<const std::byte>(grown), nh)
                                 : ouch::decode_outbound(std::span<const std::byte>(grown), nh)) ==
                        DecodeStatus::bad_length);
    }
}

struct Count : ouch::NullHandler {
    using ouch::NullHandler::on;
    std::size_t n = 0;
    template <class M>
    void on(const M&) noexcept {
        ++n;
    }
};

inline ouch::StreamResult run_stream(std::span<const std::byte> buf, Count& c, bool inbound) {
    return inbound ? ouch::decode_inbound_stream(buf, c) : ouch::decode_outbound_stream(buf, c);
}

inline void check_stream(std::span<const std::byte> all, bool inbound, std::size_t split) {
    Count counter;
    const ouch::StreamResult r = run_stream(all, counter, inbound);
    OUCH_FUZZ_CHECK(r.consumed <= all.size());
    OUCH_FUZZ_CHECK(counter.n == r.messages);

    // Independent walk over the frames.
    std::size_t pos = 0, frames = 0, decoded = 0;
    while (all.size() - pos >= 2) {
        const std::size_t len = (static_cast<std::size_t>(std::to_integer<unsigned>(all[pos])) << 8) |
                                std::to_integer<unsigned>(all[pos + 1]);
        if (all.size() - pos - 2 < len) break;
        Count one;
        const std::span<const std::byte> payload = all.subspan(pos + 2, len);
        const DecodeStatus st = inbound ? ouch::decode_inbound(payload, one) : ouch::decode_outbound(payload, one);
        if (st == DecodeStatus::ok) ++decoded;
        ++frames;
        pos += 2 + len;
    }
    OUCH_FUZZ_CHECK(r.consumed == pos);
    OUCH_FUZZ_CHECK(r.messages == decoded);
    OUCH_FUZZ_CHECK(r.messages + r.skipped == frames);
    OUCH_FUZZ_CHECK((r.skipped == 0) == (r.last_error == DecodeStatus::ok));

    // Delivery in two pieces, carrying the unconsumed tail, must add up to the same.
    split %= all.size() + 1;
    Wire head(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(split));
    Count first;
    const ouch::StreamResult a = run_stream(std::span<const std::byte>(head), first, inbound);
    Wire rest(head.begin() + static_cast<std::ptrdiff_t>(a.consumed), head.end());
    rest.insert(rest.end(), all.begin() + static_cast<std::ptrdiff_t>(split), all.end());
    Count second;
    const ouch::StreamResult b = run_stream(std::span<const std::byte>(rest), second, inbound);
    OUCH_FUZZ_CHECK(a.consumed + b.consumed == r.consumed);
    OUCH_FUZZ_CHECK(a.messages + b.messages == r.messages);
    OUCH_FUZZ_CHECK(a.skipped + b.skipped == r.skipped);
}

}  // namespace ouch_body

inline void ouch_one(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> all = std::as_bytes(std::span<const std::uint8_t>(data, size));
    ouch_body::check_message(all, true);
    ouch_body::check_message(all, false);
    const std::size_t split = size == 0 ? 0 : data[0];
    ouch_body::check_stream(all, true, split);
    ouch_body::check_stream(all, false, split);
}

}  // namespace optitrade::fuzz

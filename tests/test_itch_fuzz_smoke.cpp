// Runs the ITCH fuzz body on a fixed-seed corpus so every CI run exercises the
// fuzz invariants, with sanitizers, without needing libFuzzer.
//
// Three input families: (1) every single-byte mutation of one valid message of
// each type, (2) random and mutated messages, (3) framed streams with damaged
// length prefixes and truncated tails. A coverage check keeps the corpus honest:
// if the generator stopped reaching valid messages or an error class, the test
// would fail rather than pass on inputs that never got past the first byte.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "check.hpp"
#include "itch_fuzz_body.hpp"
#include "optitrade/core/rng.hpp"
#include "optitrade/itch/decoder.hpp"
#include "optitrade/itch/encoder.hpp"

namespace itch = optitrade::itch;
using optitrade::DecodeStatus;
using optitrade::Rng;

namespace {

using Buf = std::vector<std::byte>;

void run_body(const Buf& b) {
    optitrade::fuzz::itch_one(reinterpret_cast<const std::uint8_t*>(b.data()), b.size());
}

struct Coverage {
    std::array<std::size_t, 256> ok_by_type{};
    std::size_t truncated = 0;
    std::size_t unknown = 0;
    std::size_t bad_length = 0;
    std::size_t bad_field = 0;
    std::size_t zero_share_adds_rejected = 0;
    std::size_t stream_messages = 0;
    std::size_t stream_skipped = 0;
    std::size_t stream_partial = 0;

    struct Sink {
        template <class M>
        void on(const M&) noexcept {}
    };

    void observe(const Buf& b) {
        Sink sink;
        const std::span<const std::byte> in(b);
        const DecodeStatus st = itch::decode(in, sink);
        switch (st) {
            case DecodeStatus::ok: ++ok_by_type[std::to_integer<unsigned char>(in[0])]; break;
            case DecodeStatus::truncated: ++truncated; break;
            case DecodeStatus::unknown_type: ++unknown; break;
            case DecodeStatus::bad_length: ++bad_length; break;
            case DecodeStatus::bad_field:
                ++bad_field;
                if ((in[0] == std::byte{'A'} || in[0] == std::byte{'F'}) && in[20] == std::byte{0} &&
                    in[21] == std::byte{0} && in[22] == std::byte{0} && in[23] == std::byte{0}) {
                    ++zero_share_adds_rejected;
                }
                break;
        }
        const itch::StreamResult r = itch::decode_stream(in, sink);
        stream_messages += r.messages;
        stream_skipped += r.skipped;
        if (r.consumed < in.size()) ++stream_partial;
    }
};

// One valid message of every type, built through the encoder. (Correctness of the
// bytes is the message test's job; here they only seed the mutations.)
std::vector<Buf> seed_messages() {
    std::vector<Buf> seeds;
    auto keep = [&](std::size_t n, const Buf& scratch) { seeds.emplace_back(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(n)); };
    Buf s(64);
    const itch::Header h{0x1234, 0x5678, 0x0A0B0C0D0E0FULL};

    keep(itch::encode(itch::SystemEvent{h, 'O'}, s), s);

    itch::StockDirectory r{};
    r.h = h;
    r.symbol = optitrade::Symbol("SQQQ");
    r.market_category = 'Q';
    r.financial_status = 'N';
    r.round_lot_size = 100;
    r.round_lots_only = 'N';
    r.issue_classification = 'C';
    r.issue_subtype[0] = 'Z';
    r.issue_subtype[1] = ' ';
    r.authenticity = 'P';
    r.short_sale_threshold = 'N';
    r.ipo_flag = ' ';
    r.luld_tier = '1';
    r.etp_flag = 'Y';
    r.etp_leverage = 3;
    r.inverse = 'Y';
    keep(itch::encode(r, s), s);

    itch::AddOrder a{};
    a.h = h;
    a.ref = 0x0011223344556677ULL;
    a.side = optitrade::Side::buy;
    a.shares = 500;
    a.symbol = optitrade::Symbol("AAPL");
    a.price = 1'234'500;
    keep(itch::encode(a, s), s);
    a.has_attribution = true;
    a.side = optitrade::Side::sell;
    std::memcpy(a.mpid, "GSCO", 4);
    keep(itch::encode(a, s), s);

    keep(itch::encode(itch::OrderExecuted{h, 42, 100, 7}, s), s);
    keep(itch::encode(itch::OrderExecutedPrice{h, 42, 100, 7, true, 1'000'000}, s), s);
    keep(itch::encode(itch::OrderCancel{h, 42, 25}, s), s);
    keep(itch::encode(itch::OrderDelete{h, 42}, s), s);
    keep(itch::encode(itch::OrderReplace{h, 42, 43, 600, 1'000'001}, s), s);

    itch::Trade t{};
    t.h = h;
    t.side = optitrade::Side::buy;
    t.shares = 1000;
    t.symbol = optitrade::Symbol("TSLA");
    t.price = 703'710;
    t.match = 9;
    keep(itch::encode(t, s), s);
    return seeds;
}

constexpr char kSupported[] = {'S', 'R', 'A', 'F', 'E', 'C', 'X', 'D', 'U', 'P'};
constexpr char kUnsupported[] = {'H', 'Y', 'L', 'V', 'W', 'K', 'J', 'h', 'Q', 'B', 'I', 'N', 'O', '?', '\0'};

// Random bytes biased towards the values the decoder branches on.
std::byte interesting_byte(Rng& rng) {
    static constexpr unsigned char pool[] = {0x00, 0xFF, ' ', 'A', 'B', 'S', 'Y', 'N', 0x01, 0x80};
    if (rng.chance(1, 3)) return static_cast<std::byte>(pool[rng.bounded(sizeof pool)]);
    return static_cast<std::byte>(rng.next());
}

Buf random_message(Rng& rng) {
    char type;
    switch (rng.bounded(8)) {
        case 0: type = kUnsupported[rng.bounded(sizeof kUnsupported)]; break;
        case 1: type = static_cast<char>(rng.next()); break;
        default: type = kSupported[rng.bounded(sizeof kSupported)]; break;
    }
    std::size_t len = itch::message_length(type);
    if (len == 0 || rng.chance(1, 5)) {
        len = rng.bounded(64);
    } else if (rng.chance(1, 5)) {
        len = rng.chance(1, 2) ? len + 1 + rng.bounded(3) : len - 1 - rng.bounded(3);
    }
    Buf b(len);
    for (auto& x : b) x = interesting_byte(rng);
    if (len > 0) b[0] = static_cast<std::byte>(type);
    // Most of the time make the side and share count legal so decoding gets through.
    if (len > 23 && rng.chance(7, 8)) {
        b[19] = static_cast<std::byte>(rng.chance(1, 2) ? 'B' : 'S');
        const bool zero_shares = b[20] == std::byte{0} && b[21] == std::byte{0} && b[22] == std::byte{0} &&
                                 b[23] == std::byte{0};
        if (zero_shares && rng.chance(7, 8)) b[23] = std::byte{1};
    }
    // A zero share count is legal for most types and illegal for A/F: keep that
    // branch well fed, since random bytes essentially never produce it.
    if (len > 23 && rng.chance(1, 10)) {
        for (std::size_t i = 20; i < 24; ++i) b[i] = std::byte{0};
    }
    return b;
}

void mutate(Rng& rng, Buf& b) {
    const std::size_t edits = rng.bounded(4);
    for (std::size_t i = 0; i < edits && !b.empty(); ++i) {
        switch (rng.bounded(4)) {
            case 0: b[rng.bounded(b.size())] = interesting_byte(rng); break;
            case 1: b[rng.bounded(b.size())] ^= static_cast<std::byte>(1u << rng.bounded(8)); break;
            case 2: b.pop_back(); break;
            default: b.push_back(interesting_byte(rng)); break;
        }
    }
}

void append_frame(Buf& out, const Buf& msg, Rng& rng) {
    std::size_t announced = msg.size();
    if (rng.chance(1, 12)) announced = rng.chance(1, 2) ? announced + 1 : announced - (announced > 0 ? 1 : 0);
    if (rng.chance(1, 40)) announced = rng.bounded(0x10000);
    out.push_back(static_cast<std::byte>(announced >> 8));
    out.push_back(static_cast<std::byte>(announced & 0xFF));
    out.insert(out.end(), msg.begin(), msg.end());
}

Buf random_stream(Rng& rng) {
    Buf s;
    const std::size_t frames = rng.bounded(7);
    for (std::size_t i = 0; i < frames; ++i) {
        Buf m = random_message(rng);
        if (rng.chance(1, 4)) mutate(rng, m);
        append_frame(s, m, rng);
    }
    if (!s.empty() && rng.chance(1, 3)) s.resize(s.size() - rng.bounded(std::min<std::size_t>(s.size(), 40)));
    if (rng.chance(1, 10)) s.push_back(interesting_byte(rng));
    return s;
}

}  // namespace

OT_TEST(every_single_byte_mutation_of_a_valid_message_upholds_the_invariants) {
    const std::vector<Buf> seeds = seed_messages();
    OT_CHECK_EQ(seeds.size(), std::size_t{10});  // one per supported type, 'F' included
    Coverage cov;
    for (const Buf& seed : seeds) {
        run_body(seed);
        cov.observe(seed);
        for (std::size_t i = 0; i < seed.size(); ++i) {
            for (int v = 0; v < 256; ++v) {
                Buf m = seed;
                m[i] = static_cast<std::byte>(v);
                run_body(m);
                cov.observe(m);
            }
        }
        if (seed[0] == std::byte{'A'} || seed[0] == std::byte{'F'}) {
            Buf zero_shares = seed;
            for (std::size_t i = 20; i < 24; ++i) zero_shares[i] = std::byte{0};
            run_body(zero_shares);
            cov.observe(zero_shares);
        }
        // Every prefix and every one-byte extension, framed and not.
        for (std::size_t k = 0; k <= seed.size(); ++k) run_body(Buf(seed.begin(), seed.begin() + static_cast<std::ptrdiff_t>(k)));
        Buf longer = seed;
        for (int extra = 0; extra < 5; ++extra) {
            longer.push_back(std::byte{0});
            run_body(longer);
        }
    }
    for (char t : kSupported) OT_CHECK(cov.ok_by_type[static_cast<unsigned char>(t)] > 0);
    OT_CHECK(cov.bad_field > 0);
    OT_CHECK_EQ(cov.zero_share_adds_rejected, std::size_t{2});  // the A and the F seed
}

OT_TEST(random_and_mutated_inputs_uphold_the_invariants) {
    Rng rng(0x17C4F0220240229ULL);
    Coverage cov;
    constexpr int kIterations = 120'000;
    for (int i = 0; i < kIterations; ++i) {
        Buf in;
        switch (rng.bounded(10)) {
            case 0:  // pure noise
                in.resize(rng.bounded(96));
                for (auto& x : in) x = static_cast<std::byte>(rng.next());
                break;
            case 1:
            case 2:
            case 3:
            case 4:
                in = random_stream(rng);
                break;
            default:
                in = random_message(rng);
                if (rng.chance(1, 3)) mutate(rng, in);
                break;
        }
        run_body(in);
        cov.observe(in);
    }

    // The corpus must reach every accepting path and every error class, often.
    for (char t : kSupported) OT_CHECK(cov.ok_by_type[static_cast<unsigned char>(t)] > 500);
    OT_CHECK(cov.truncated > 1'000);
    OT_CHECK(cov.unknown > 1'000);
    OT_CHECK(cov.bad_length > 1'000);
    OT_CHECK(cov.bad_field > 500);
    OT_CHECK(cov.zero_share_adds_rejected > 100);
    OT_CHECK(cov.stream_messages > 20'000);
    OT_CHECK(cov.stream_skipped > 20'000);
    OT_CHECK(cov.stream_partial > 5'000);
}

OT_TEST(degenerate_inputs) {
    optitrade::fuzz::itch_one(nullptr, 0);
    const std::uint8_t one = 0x41;
    optitrade::fuzz::itch_one(&one, 1);
    Buf zeros(70'000, std::byte{0});  // longer than any single frame: a long run of empty frames
    run_body(zeros);
    Buf ones(70'000, std::byte{0xFF});  // frame length 0xFFFF that never completes
    run_body(ones);
}

OT_TEST_MAIN()

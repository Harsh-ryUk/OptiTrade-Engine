// Deterministic stand-in for the libFuzzer target: runs the fuzz body on a seed corpus and on
// a few hundred thousand generated and mutated inputs (fixed seed), so the invariants are
// exercised on every build, including toolchains that ship no libFuzzer.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "check.hpp"
#include "mold64_fuzz_body.hpp"
#include "optitrade/core/rng.hpp"

namespace {

using optitrade::DecodeStatus;
using optitrade::Rng;
using namespace optitrade::net::mold64;
using Bytes = std::vector<std::uint8_t>;

struct Tally {
    std::uint64_t total{0}, ok{0}, ok_with_messages{0}, truncated{0}, bad_length{0}, other{0};
    void add(DecodeStatus st, const Bytes& in) {
        ++total;
        switch (st) {
            case DecodeStatus::ok:
                ++ok;
                if (in.size() > kHeaderSize) ++ok_with_messages;
                break;
            case DecodeStatus::truncated: ++truncated; break;
            case DecodeStatus::bad_length: ++bad_length; break;
            default: ++other; break;
        }
    }
};

DecodeStatus feed(Tally& t, const Bytes& in) {
    const DecodeStatus st = optitrade::fuzz::mold64_one(in.empty() ? nullptr : in.data(), in.size());
    t.add(st, in);
    return st;
}

Bytes random_bytes(Rng& rng, std::size_t max_len) {
    Bytes b(static_cast<std::size_t>(rng.bounded(max_len + 1)));
    for (auto& x : b) x = static_cast<std::uint8_t>(rng.bounded(256));
    return b;
}

// A well-formed packet, so that mutations start close to the interesting part of the input space.
Bytes valid_packet(Rng& rng) {
    std::array<std::byte, 640> buf{};
    std::array<char, kSessionSize> session{};
    for (char& c : session) c = static_cast<char>(rng.bounded(256));
    std::uint64_t seq = rng.next();
    if (rng.chance(1, 2)) seq >>= 40;  // mostly small numbers: gaps and duplicates stay readable
    PacketBuilder b(std::span<std::byte>(buf), session, seq);
    const unsigned n = static_cast<unsigned>(rng.bounded(7));
    for (unsigned i = 0; i < n; ++i) {
        std::array<std::byte, 40> msg{};
        const std::size_t len = rng.chance(1, 6) ? 0 : static_cast<std::size_t>(rng.bounded(msg.size()));
        for (std::size_t j = 0; j < len; ++j) msg[j] = static_cast<std::byte>(rng.bounded(256));
        (void)b.add(std::span<const std::byte>(msg.data(), len));
    }
    const auto pkt = b.finish();
    Bytes out(pkt.size());
    for (std::size_t i = 0; i < pkt.size(); ++i) out[i] = static_cast<std::uint8_t>(pkt[i]);
    // One in eight becomes a control packet by rewriting the count.
    if (out.size() >= kHeaderSize && rng.chance(1, 8)) {
        const bool eos = rng.chance(1, 2);
        out[18] = eos ? 0xFF : 0x00;
        out[19] = eos ? 0xFF : 0x00;
        if (rng.chance(3, 4)) out.resize(kHeaderSize);
    }
    return out;
}

void mutate(Rng& rng, Bytes& b) {
    switch (rng.bounded(8)) {
        case 0:
            if (!b.empty()) b[static_cast<std::size_t>(rng.bounded(b.size()))] ^= static_cast<std::uint8_t>(1u << rng.bounded(8));
            break;
        case 1:
            if (!b.empty()) b[static_cast<std::size_t>(rng.bounded(b.size()))] = static_cast<std::uint8_t>(rng.bounded(256));
            break;
        case 2:
            if (!b.empty()) b.resize(static_cast<std::size_t>(rng.bounded(b.size())));  // truncate
            break;
        case 3:
            for (std::uint64_t k = rng.bounded(4) + 1; k > 0; --k) b.push_back(static_cast<std::uint8_t>(rng.bounded(256)));
            break;
        case 4:  // random header count
            if (b.size() >= kHeaderSize) {
                const std::uint64_t v = rng.chance(1, 4) ? 0xFFFF : rng.bounded(9);
                b[18] = static_cast<std::uint8_t>(v >> 8);
                b[19] = static_cast<std::uint8_t>(v);
            }
            break;
        case 5:  // block length pointing somewhere else
            if (b.size() >= kHeaderSize + 2) {
                const std::size_t at = kHeaderSize + static_cast<std::size_t>(rng.bounded(std::min<std::size_t>(b.size() - kHeaderSize - 1, 30)));
                b[at] = static_cast<std::uint8_t>(rng.bounded(3));
            }
            break;
        case 6:  // cut a piece out of the middle
            if (b.size() > 2) {
                const auto from = static_cast<std::ptrdiff_t>(rng.bounded(b.size() - 1));
                const auto to = from + 1 + static_cast<std::ptrdiff_t>(rng.bounded(static_cast<std::uint64_t>(b.size() - static_cast<std::size_t>(from))));
                b.erase(b.begin() + from, b.begin() + std::min<std::ptrdiff_t>(to, static_cast<std::ptrdiff_t>(b.size())));
            }
            break;
        default:  // duplicate the tail (repeated blocks)
            if (b.size() > kHeaderSize) {
                const Bytes tail(b.begin() + kHeaderSize, b.end());
                b.insert(b.end(), tail.begin(), tail.end());
            }
            break;
    }
}

}  // namespace

OT_TEST(fuzz_smoke_seed_corpus) {
    Tally t;
    // Empty input and every one-byte input.
    OT_CHECK(feed(t, {}) == DecodeStatus::truncated);
    for (unsigned v = 0; v < 256; ++v) OT_CHECK(feed(t, Bytes{static_cast<std::uint8_t>(v)}) == DecodeStatus::truncated);

    // Hand-written packets (heartbeat, end of session, a three-message packet) and every
    // prefix and every single-byte extension of them.
    const std::vector<Bytes> seeds = {
        {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0, 0, 0, 0, 0, 0, 0x0B, 0xB9, 0x00, 0x00},
        {0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0, 0, 0, 0, 0, 0x0F, 0x42, 0x40, 0xFF, 0xFF},
        {0x53, 0x45, 0x53, 0x53, 0x49, 0x4F, 0x4E, 0x30, 0x30, 0x31, 0, 0, 0, 1, 0, 0, 0, 5, 0, 3,
         0, 3, 0xAA, 0xBB, 0xCC, 0, 1, 0xDD, 0, 5, 1, 2, 3, 4, 5},
    };
    for (const Bytes& s : seeds) {
        OT_CHECK(feed(t, s) == DecodeStatus::ok);
        for (std::size_t n = 0; n < s.size(); ++n) feed(t, Bytes(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n)));
        for (unsigned v = 0; v < 256; v += 5) {
            Bytes e = s;
            e.push_back(static_cast<std::uint8_t>(v));
            feed(t, e);
        }
    }
    OT_CHECK(t.ok >= 3);
    OT_CHECK(t.bad_length > 0);
}

OT_TEST(fuzz_smoke_generated_and_mutated_inputs) {
    Rng rng(0x0F0CC5EEDULL);
    Tally t;
    constexpr int kIterations = 300'000;
    for (int i = 0; i < kIterations; ++i) {
        Bytes in;
        const std::uint64_t mode = rng.bounded(10);
        if (mode < 3) {
            in = random_bytes(rng, 100);
        } else {
            in = valid_packet(rng);
            if (mode >= 5) {
                for (std::uint64_t k = rng.bounded(3) + 1; k > 0; --k) mutate(rng, in);
            }
        }
        feed(t, in);
    }
    std::printf("       %llu inputs: %llu accepted (%llu with messages), %llu truncated, %llu bad_length\n",
                static_cast<unsigned long long>(t.total), static_cast<unsigned long long>(t.ok),
                static_cast<unsigned long long>(t.ok_with_messages),
                static_cast<unsigned long long>(t.truncated), static_cast<unsigned long long>(t.bad_length));
    OT_CHECK_EQ(t.total, std::uint64_t{kIterations});
    // The generator has to reach accepted, short and over-long packets in quantity; otherwise
    // the invariants inside the body would be checked on a narrow slice of the input space.
    OT_CHECK(t.ok_with_messages > 30'000);
    OT_CHECK(t.ok > 60'000);
    OT_CHECK(t.truncated > 30'000);
    OT_CHECK(t.bad_length > 10'000);
    OT_CHECK_EQ(t.other, std::uint64_t{0});  // the codec has no other outcomes to report
}

OT_TEST_MAIN()

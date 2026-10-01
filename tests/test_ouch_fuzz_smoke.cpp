// Deterministic smoke run of the OUCH fuzz body: pseudo-random bytes, structured
// messages with fixed-up fields, mutations of those, and multi-frame streams cut
// at arbitrary points. It exists so the fuzz invariants run on every test pass on
// every platform, including where libFuzzer is unavailable.

#include <cstdint>
#include <iterator>
#include <span>
#include <vector>

#include "check.hpp"
#include "optitrade/core/rng.hpp"
#include "ouch_fuzz_body.hpp"

using namespace optitrade;

namespace {

using Buf = std::vector<std::uint8_t>;

// Layout facts needed to build plausible messages: type, length, and where the
// constrained fields sit (0 = none).
struct Kind {
    char type;
    std::size_t len;
    bool inbound;
    std::size_t side;
    std::size_t state;
    std::size_t shares;
};
constexpr Kind kKinds[] = {
    {'O', 49, true, 15, 0, 16},  {'U', 47, true, 0, 0, 29},  {'X', 19, true, 0, 0, 0},
    {'A', 66, false, 23, 64, 0}, {'U', 80, false, 23, 64, 0}, {'C', 28, false, 0, 0, 0},
    {'E', 40, false, 0, 0, 0},   {'J', 24, false, 0, 0, 0},
};

std::uint8_t random_byte(Rng& g) { return static_cast<std::uint8_t>(g.bounded(256)); }

Buf random_bytes(Rng& g, std::size_t n) {
    Buf b(n);
    for (auto& x : b) x = random_byte(g);
    return b;
}

// Random content, but each constrained field is made legal most of the time so
// the deeper decoding paths are reached rather than always failing on the first check.
Buf plausible(Rng& g) {
    const Kind& k = kKinds[g.bounded(std::size(kKinds))];
    Buf b = random_bytes(g, k.len);
    b[0] = static_cast<std::uint8_t>(k.type);
    static constexpr char kSides[] = {'B', 'S', 'T', 'E'};
    static constexpr char kStates[] = {'L', 'D'};
    if (k.side != 0 && !g.chance(1, 8)) b[k.side] = static_cast<std::uint8_t>(kSides[g.bounded(4)]);
    if (k.state != 0 && !g.chance(1, 8)) b[k.state] = static_cast<std::uint8_t>(kStates[g.bounded(2)]);
    if (k.shares != 0 && !g.chance(1, 8)) {
        const std::uint32_t s = g.chance(1, 4) ? (g.chance(1, 2) ? 1u : 999'999u)
                                               : static_cast<std::uint32_t>(g.range(1, 999'999));
        for (int i = 0; i < 4; ++i) b[k.shares + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(s >> (24 - 8 * i));
    }
    return b;
}

void mutate(Rng& g, Buf& b) {
    const int edits = 1 + static_cast<int>(g.bounded(4));
    for (int i = 0; i < edits && !b.empty(); ++i) {
        switch (g.bounded(6)) {
            case 0: b[g.bounded(b.size())] = random_byte(g); break;
            case 1: b[g.bounded(b.size())] ^= static_cast<std::uint8_t>(1u << g.bounded(8)); break;
            case 2: b.pop_back(); break;
            case 3: b.push_back(random_byte(g)); break;
            case 4: b[0] = random_byte(g); break;
            default: b.erase(b.begin() + static_cast<std::ptrdiff_t>(g.bounded(b.size()))); break;
        }
    }
}

void add_frame(Buf& stream, const Buf& payload) {
    stream.push_back(static_cast<std::uint8_t>(payload.size() >> 8));
    stream.push_back(static_cast<std::uint8_t>(payload.size()));
    stream.insert(stream.end(), payload.begin(), payload.end());
}

Buf make_input(Rng& g) {
    switch (g.bounded(8)) {
        case 0: return random_bytes(g, g.bounded(120));
        case 1:
        case 2: return plausible(g);
        case 3:
        case 4: {
            Buf b = plausible(g);
            mutate(g, b);
            return b;
        }
        default: {  // a stream of frames, some mutated, cut somewhere
            Buf stream;
            const int frames = 1 + static_cast<int>(g.bounded(6));
            for (int i = 0; i < frames; ++i) {
                Buf m = plausible(g);
                if (g.chance(1, 4)) mutate(g, m);
                if (g.chance(1, 16)) m.clear();
                add_frame(stream, m);
            }
            if (g.chance(1, 3)) stream.resize(g.bounded(stream.size() + 1));
            if (g.chance(1, 8)) stream.push_back(random_byte(g));
            return stream;
        }
    }
}

}  // namespace

OT_TEST(fuzz_body_holds_on_generated_and_mutated_inputs) {
    constexpr int kIterations = 220'000;
    Rng g(0x0DDC0FFEE);
    int status_seen[5][2] = {};  // [DecodeStatus][inbound?]
    for (int i = 0; i < kIterations; ++i) {
        const Buf in = make_input(g);
        // Exactly-sized heap block, so AddressSanitizer sees every overrun.
        std::uint8_t* exact = in.empty() ? nullptr : new std::uint8_t[in.size()];
        for (std::size_t k = 0; k < in.size(); ++k) exact[k] = in[k];
        fuzz::ouch_one(exact, in.size());

        const std::span<const std::byte> bytes = std::as_bytes(std::span<const std::uint8_t>(exact, in.size()));
        ouch::NullHandler h;
        ++status_seen[static_cast<int>(ouch::decode_inbound(bytes, h))][1];
        ++status_seen[static_cast<int>(ouch::decode_outbound(bytes, h))][0];
        delete[] exact;
    }
    // The run must reach every outcome in both directions, or it proves little.
    for (int s = 0; s < 5; ++s) {
        OT_CHECK(status_seen[s][0] > 500);
        OT_CHECK(status_seen[s][1] > 500);
    }
}

OT_TEST(fuzz_body_accepts_the_empty_input) {
    fuzz::ouch_one(nullptr, 0);
    const std::uint8_t one = 'O';
    fuzz::ouch_one(&one, 1);
}

OT_TEST_MAIN()

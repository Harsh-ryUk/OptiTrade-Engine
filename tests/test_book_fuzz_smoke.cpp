// Runs the order-book fuzz body on a fixed-seed corpus so every CI run exercises
// its invariants, with sanitizers, without needing libFuzzer.
//
// Three input families: (1) 7-byte operation lists, which the body's structured
// mode turns into plausible traffic; (2) valid ITCH streams with a few damaged
// bytes, which the raw mode decodes; (3) noise. A coverage check keeps the corpus
// honest: if the generator stopped reaching a result class, a full ladder, a full
// order table or an eviction, the test fails instead of passing on empty inputs.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "book_fuzz_body.hpp"
#include "check.hpp"
#include "optitrade/core/rng.hpp"

using optitrade::Rng;
using optitrade::book::Applied;
using optitrade::fuzz::BookFuzzTally;

namespace {

using Buf = std::vector<std::uint8_t>;

std::uint8_t interesting(Rng& rng) {
    static constexpr std::uint8_t pool[] = {0, 1, 2, 3, 4, 5, 6, 7, 10, 100, 254, 255};
    return rng.chance(1, 2) ? pool[rng.bounded(sizeof pool)] : static_cast<std::uint8_t>(rng.next());
}

Buf random_ops(Rng& rng) {
    Buf b;
    const std::size_t ops = rng.bounded(28);
    for (std::size_t i = 0; i < ops; ++i) {
        std::uint8_t op[optitrade::fuzz::book_impl::kOpSize];
        for (auto& x : op) x = interesting(rng);
        // Kinds are weighted towards adds so there is something to execute and replace.
        op[0] = rng.chance(1, 3) ? static_cast<std::uint8_t>(rng.bounded(3)) : static_cast<std::uint8_t>(rng.next());
        op[1] = static_cast<std::uint8_t>(rng.bounded(3));  // few instruments: levels collide
        b.insert(b.end(), op, op + sizeof op);
    }
    if (rng.chance(1, 8)) b.push_back(interesting(rng));  // ragged tail
    return b;
}

// A valid framed stream: the body's own encoder path, run over random ops.
Buf valid_stream(Rng& rng) {
    const Buf ops = random_ops(rng);
    std::vector<std::byte> stream;
    for (std::size_t off = 0; off + optitrade::fuzz::book_impl::kOpSize <= ops.size(); off += optitrade::fuzz::book_impl::kOpSize) {
        optitrade::fuzz::book_impl::append_op(stream, ops.data() + off);
    }
    Buf out(stream.size());
    for (std::size_t i = 0; i < stream.size(); ++i) out[i] = std::to_integer<std::uint8_t>(stream[i]);
    return out;
}

void damage(Rng& rng, Buf& b) {
    const std::size_t edits = rng.bounded(4);
    for (std::size_t i = 0; i < edits && !b.empty(); ++i) {
        switch (rng.bounded(4)) {
            case 0: b[rng.bounded(b.size())] = interesting(rng); break;
            case 1: b[rng.bounded(b.size())] ^= static_cast<std::uint8_t>(1u << rng.bounded(8)); break;
            case 2: b.resize(b.size() - rng.bounded(std::min<std::size_t>(b.size(), 12))); break;
            default: b.push_back(interesting(rng)); break;
        }
    }
}

void run(const Buf& in, BookFuzzTally& tally) {
    optitrade::fuzz::book_one_tally(in.data(), in.size(), tally);
}

}  // namespace

OT_TEST(random_and_mutated_inputs_uphold_the_invariants) {
    Rng rng(0xB00CF022024ULL);
    BookFuzzTally tally;
    constexpr int kIterations = 40'000;
    for (int i = 0; i < kIterations; ++i) {
        Buf in;
        switch (rng.bounded(10)) {
            case 0:
            case 1:  // noise
                in.resize(rng.bounded(120));
                for (auto& x : in) x = static_cast<std::uint8_t>(rng.next());
                break;
            case 2:
            case 3:
            case 4:
            case 5:  // valid stream, lightly damaged
                in = valid_stream(rng);
                if (rng.chance(1, 3)) damage(rng, in);
                break;
            default:  // operation lists
                in = random_ops(rng);
                break;
        }
        run(in, tally);
    }

    const auto count = [&](Applied a) { return tally.by_result[static_cast<std::size_t>(a)]; };
    OT_CHECK(tally.messages > 700'000);
    OT_CHECK(count(Applied::ok) > 200'000);
    OT_CHECK(count(Applied::ignored) > 50'000);
    OT_CHECK(count(Applied::unknown_order) > 100'000);
    OT_CHECK(count(Applied::duplicate_order) > 100'000);
    OT_CHECK(count(Applied::invalid) > 50'000);
    OT_CHECK(count(Applied::capacity) > 6'000);
    OT_CHECK_EQ(count(Applied::unknown_symbol), std::uint64_t{0});
    OT_CHECK(tally.table_full > 2'500);
    OT_CHECK(tally.stranded > 3'500);
    OT_CHECK(tally.overflows > 8'000);
    OT_CHECK(tally.empty_ladders_after_fill > 15'000);
}

OT_TEST(degenerate_inputs) {
    optitrade::fuzz::book_one(nullptr, 0);
    const std::uint8_t one = 0x41;
    optitrade::fuzz::book_one(&one, 1);
    Buf zeros(70'000, 0);  // a long run of empty frames, and 10'000 identical ops
    optitrade::fuzz::book_one(zeros.data(), zeros.size());
    Buf ones(70'000, 0xFF);  // a frame that never completes; ops that saturate every field
    optitrade::fuzz::book_one(ones.data(), ones.size());
}

OT_TEST(a_hand_built_sequence_reaches_eviction_and_the_stranded_order_paths) {
    // The body allows two levels per side, prices 100 + (byte % 6), and byte 254 means price 0.
    //   1  add bid 100 (ref 1)                 ok
    //   2  add bid 101 (ref 2)                 ok, side full: 101, 100
    //   3  add bid 0   (ref 3)                 capacity: worse than the worst, stranded
    //   4  add bid 102 (ref 4)                 ok, evicts 100 (ref 1 is now off the ladder)
    //   5  delete ref 1                        ok, must not touch the ladder
    //   6  replace ref 2 by ref 5 at 105       ok
    //   7  execute 99 of ref 4 (holds 10)      invalid, order dropped
    //   8  cancel 4 of ref 3 (stranded)        ok
    struct Op {
        std::uint8_t kind, loc, ref, ref2, qty, px, flags;
    };
    const Op ops[] = {
        {0, 1, 0, 0, 10, 0, 0},   {0, 1, 1, 0, 10, 1, 0}, {0, 1, 2, 0, 10, 254, 0}, {0, 1, 3, 0, 10, 2, 0},
        {6, 1, 0, 0, 0, 0, 0},    {7, 1, 1, 4, 20, 5, 0}, {3, 1, 3, 0, 99, 0, 0},   {5, 1, 2, 0, 4, 0, 0},
    };
    Buf in;
    for (const Op& o : ops) {
        const std::uint8_t b[] = {o.kind, o.loc, o.ref, o.ref2, o.qty, o.px, o.flags};
        in.insert(in.end(), b, b + sizeof b);
    }
    BookFuzzTally tally;
    run(in, tally);
    const auto count = [&](Applied a) { return tally.by_result[static_cast<std::size_t>(a)]; };
    OT_CHECK(count(Applied::ok) >= 6);
    OT_CHECK(count(Applied::capacity) >= 1);
    OT_CHECK(count(Applied::invalid) >= 1);
    OT_CHECK(tally.stranded >= 1);
    OT_CHECK(tally.overflows >= 2);  // the refusal and the eviction
}

OT_TEST_MAIN()

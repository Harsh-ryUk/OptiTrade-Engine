// SpscRing: single-thread semantics (capacity rounding, full/empty, index
// wrap-around, model comparison) and two-thread transfers that check ordering,
// completeness and payload integrity. Also meant to be run under
// -fsanitize=thread, where a missing acquire/release pairing is reported as a race.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <thread>

#include "check.hpp"
#include "optitrade/core/rng.hpp"
#include "optitrade/core/spsc_ring.hpp"

using namespace optitrade;

namespace {

// One cache line of payload whose fields are all derived from the sequence number,
// so a torn or stale copy (a published index before the data) is detected.
struct Msg {
    std::uint64_t seq;
    std::uint64_t check;
    std::uint64_t filler[6];
};
static_assert(sizeof(Msg) == 64);

Msg make_msg(std::uint64_t seq) {
    Msg m{};
    m.seq = seq;
    m.check = seq * 0x9E3779B97F4A7C15ULL + 0x1234;
    for (std::uint64_t i = 0; i < 6; ++i) m.filler[i] = ~seq + i;
    return m;
}

bool msg_ok(const Msg& m, std::uint64_t expected_seq) {
    if (m.seq != expected_seq || m.check != expected_seq * 0x9E3779B97F4A7C15ULL + 0x1234) return false;
    for (std::uint64_t i = 0; i < 6; ++i)
        if (m.filler[i] != ~expected_seq + i) return false;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Single thread
// ---------------------------------------------------------------------------

OT_TEST(capacity_rounds_up_to_power_of_two_with_minimum_two) {
    struct Case {
        std::size_t requested, expected;
    };
    for (const Case& c : {Case{0, 2}, Case{1, 2}, Case{2, 2}, Case{3, 4}, Case{4, 4}, Case{5, 8}, Case{8, 8},
                          Case{9, 16}, Case{1000, 1024}, Case{1024, 1024}, Case{1025, 2048}}) {
        SpscRing<int> ring(c.requested);
        OT_CHECK_EQ(ring.capacity(), c.expected);
    }
}

OT_TEST(oversized_capacity_throws) {
    bool threw = false;
    try {
        SpscRing<char> ring(SIZE_MAX);
    } catch (const std::length_error&) {
        threw = true;
    }
    OT_CHECK(threw);
}

OT_TEST(hot_indices_live_on_separate_cache_lines) {
    static_assert(alignof(SpscRing<int>) == kCacheLine);
    static_assert(sizeof(SpscRing<int>) % kCacheLine == 0);
    // head+cached_tail, tail+cached_head and the read-only header each get a line.
    static_assert(sizeof(SpscRing<int>) >= 3 * kCacheLine);
    OT_CHECK(true);
}

OT_TEST(new_ring_is_empty) {
    SpscRing<int> ring(4);
    int out = -1;
    OT_CHECK(!ring.try_pop(out));
    OT_CHECK_EQ(out, -1);  // a failed pop must not touch the destination
    OT_CHECK_EQ(ring.size_approx(), std::size_t{0});
}

OT_TEST(fills_to_exactly_capacity_and_then_refuses) {
    SpscRing<std::uint32_t> ring(8);
    for (std::uint32_t i = 0; i < 8; ++i) {
        OT_CHECK(ring.try_push(i + 100));
        OT_CHECK_EQ(ring.size_approx(), std::size_t{i + 1});
    }
    for (int attempt = 0; attempt < 3; ++attempt) OT_CHECK(!ring.try_push(999));
    OT_CHECK_EQ(ring.size_approx(), std::size_t{8});
    for (std::uint32_t i = 0; i < 8; ++i) {
        std::uint32_t out = 0;
        OT_CHECK(ring.try_pop(out));
        OT_CHECK_EQ(out, i + 100);  // FIFO, and the refused 999 never entered
    }
    std::uint32_t out = 0;
    OT_CHECK(!ring.try_pop(out));
    OT_CHECK_EQ(ring.size_approx(), std::size_t{0});
}

// A slot must be reusable as soon as the consumer has popped it.
OT_TEST(one_pop_frees_exactly_one_slot) {
    SpscRing<int> ring(4);
    for (int i = 0; i < 4; ++i) OT_CHECK(ring.try_push(i));
    OT_CHECK(!ring.try_push(4));
    int out = 0;
    OT_CHECK(ring.try_pop(out));
    OT_CHECK_EQ(out, 0);
    OT_CHECK(ring.try_push(4));
    OT_CHECK(!ring.try_push(5));
    for (int expect = 1; expect <= 4; ++expect) {
        OT_CHECK(ring.try_pop(out));
        OT_CHECK_EQ(out, expect);
    }
    OT_CHECK(!ring.try_pop(out));
}

// Occupancy k in [0, cap] repeated enough times that every buffer position is
// used as the start of a run, i.e. the physical index wraps in every phase.
OT_TEST(wraps_around_at_every_fill_level) {
    constexpr std::size_t kCap = 8;
    SpscRing<std::uint64_t> ring(kCap);
    std::uint64_t next_in = 0, next_out = 0;
    for (std::size_t round = 0; round < 3 * kCap; ++round) {
        for (std::size_t k = 0; k <= kCap; ++k) {
            for (std::size_t i = 0; i < k; ++i) OT_CHECK(ring.try_push(next_in++));
            if (k == kCap)
                OT_CHECK(!ring.try_push(0xBAD));  // full: refused, so not part of the stream
            else
                OT_CHECK(ring.try_push(next_in++));  // one more still fits
            std::uint64_t out = 0;
            while (ring.try_pop(out)) OT_CHECK_EQ(out, next_out++);
            OT_CHECK_EQ(next_out, next_in);
            OT_CHECK_EQ(ring.size_approx(), std::size_t{0});
        }
    }
}

OT_TEST(structured_payload_copies_whole_object) {
    SpscRing<Msg> ring(4);
    for (std::uint64_t i = 0; i < 1000; ++i) {
        OT_CHECK(ring.try_push(make_msg(i)));
        OT_CHECK(ring.try_push(make_msg(i + 1'000'000)));
        Msg a{}, b{};
        OT_CHECK(ring.try_pop(a));
        OT_CHECK(ring.try_pop(b));
        OT_CHECK(msg_ok(a, i));
        OT_CHECK(msg_ok(b, i + 1'000'000));
    }
}

OT_TEST(bool_elements_are_independent_bytes) {
    SpscRing<bool> ring(4);
    for (int i = 0; i < 4; ++i) OT_CHECK(ring.try_push(i % 2 == 1));
    for (int i = 0; i < 4; ++i) {
        bool out = (i % 2 == 0);  // start with the wrong value
        OT_CHECK(ring.try_pop(out));
        OT_CHECK_EQ(out, i % 2 == 1);
    }
}

// Random push/pop sequences with phases biased towards full and towards empty,
// compared with a std::deque bounded to the ring capacity.
OT_TEST(random_operations_match_deque_model) {
    for (std::size_t requested : {1u, 2u, 3u, 5u, 16u, 64u}) {
        SpscRing<std::uint64_t> ring(requested);
        const std::size_t cap = ring.capacity();
        std::deque<std::uint64_t> model;
        Rng rng(1000 + requested);
        std::uint64_t counter = 0;
        for (std::size_t op = 0; op < 200'000; ++op) {
            const std::uint64_t push_pct = ((op / 700) % 3 == 0) ? 80 : ((op / 700) % 3 == 1) ? 20 : 50;
            if (rng.bounded(100) < push_pct) {
                const bool expect = model.size() < cap;
                const bool got = ring.try_push(counter);
                if (got != expect) {
                    OT_CHECK_EQ(got, expect);
                    return;
                }
                if (got) model.push_back(counter++);
            } else {
                std::uint64_t out = ~std::uint64_t{0};
                const bool got = ring.try_pop(out);
                if (got != !model.empty()) {
                    OT_CHECK_EQ(got, !model.empty());
                    return;
                }
                if (got) {
                    if (out != model.front()) {
                        OT_CHECK_EQ(out, model.front());
                        return;
                    }
                    model.pop_front();
                }
            }
            if (ring.size_approx() != model.size()) {
                OT_CHECK_EQ(ring.size_approx(), model.size());
                return;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Two threads
// ---------------------------------------------------------------------------

namespace {

struct Deadline {
    std::chrono::steady_clock::time_point end;
    explicit Deadline(std::chrono::seconds s) : end(std::chrono::steady_clock::now() + s) {}
    bool expired() const { return std::chrono::steady_clock::now() > end; }
};

struct TransferResult {
    std::uint64_t received{0};
    bool corrupt{false};
    bool timed_out{false};
    bool occupancy_exceeded_capacity{false};
};

// Producer (calling thread) pushes `count` items made by make(seq); a consumer
// thread checks each with ok(item, seq). A monitor thread hammers size_approx().
// Spin loops are bounded by a deadline so a broken ring fails instead of hanging.
template <class T, class Make, class Ok>
TransferResult transfer(std::size_t capacity, std::uint64_t count, Make make, Ok ok) {
    SpscRing<T> ring(capacity);
    TransferResult result;
    std::atomic<bool> stop{false};
    std::atomic<bool> monitor_done{false};
    std::atomic<bool> occupancy_bad{false};
    const Deadline deadline(std::chrono::seconds(120));

    // Samples far more often than it yields: the interesting window for a wrapped
    // or overshooting occupancy is a few nanoseconds wide.
    std::thread monitor([&] {
        std::uint64_t samples = 0;
        while (!monitor_done.load(std::memory_order_acquire)) {
            if (ring.size_approx() > ring.capacity()) occupancy_bad.store(true, std::memory_order_relaxed);
            if ((++samples & 0xFF) == 0) std::this_thread::yield();
        }
    });

    std::thread consumer([&] {
        T item{};
        std::uint64_t spins = 0;
        while (result.received < count) {
            if (ring.try_pop(item)) {
                if (!ok(item, result.received)) {
                    result.corrupt = true;
                    stop.store(true, std::memory_order_relaxed);
                    return;
                }
                ++result.received;
                spins = 0;
            } else {
                ++spins;
                if ((spins & 1023) == 0) std::this_thread::yield();
                if ((spins & 0xFFFFF) == 0 && (stop.load(std::memory_order_relaxed) || deadline.expired())) {
                    result.timed_out = true;
                    stop.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        }
    });

    std::uint64_t spins = 0;
    bool aborted = false;
    for (std::uint64_t seq = 0; seq < count && !aborted; ++seq) {
        const T item = make(seq);
        while (!ring.try_push(item)) {
            ++spins;
            if ((spins & 1023) == 0) std::this_thread::yield();
            if ((spins & 0xFFFFF) == 0 && (stop.load(std::memory_order_relaxed) || deadline.expired())) {
                stop.store(true, std::memory_order_relaxed);
                aborted = true;
                break;
            }
        }
    }
    consumer.join();
    monitor_done.store(true, std::memory_order_release);
    monitor.join();
    result.occupancy_exceeded_capacity = occupancy_bad.load();
    return result;
}

}  // namespace

// The headline test: five million 64-byte items through a 1024-slot ring. The
// consumer verifies strict order, payload integrity and completeness.
OT_TEST(two_threads_five_million_sequenced_items) {
    constexpr std::uint64_t kCount = 5'000'000;
    const TransferResult r = transfer<Msg>(1024, kCount, make_msg, msg_ok);
    OT_CHECK(!r.corrupt);
    OT_CHECK(!r.timed_out);
    OT_CHECK_EQ(r.received, kCount);
    OT_CHECK(!r.occupancy_exceeded_capacity);
}

// Two slots: the ring is full or empty almost every time, so the cached-index
// refresh paths on both sides run constantly.
OT_TEST(two_threads_minimal_ring) {
    constexpr std::uint64_t kCount = 2'000'000;
    const TransferResult r = transfer<std::uint64_t>(
        2, kCount, [](std::uint64_t seq) { return seq ^ 0xA5A5A5A5A5A5A5A5ULL; },
        [](std::uint64_t v, std::uint64_t seq) { return v == (seq ^ 0xA5A5A5A5A5A5A5A5ULL); });
    OT_CHECK(!r.corrupt);
    OT_CHECK(!r.timed_out);
    OT_CHECK_EQ(r.received, kCount);
    OT_CHECK(!r.occupancy_exceeded_capacity);
}

// Adjacent one-byte elements are written and read concurrently; this is only free of
// data races if the buffer is an array of real bools (not a packed vector<bool>).
OT_TEST(two_threads_bool_elements) {
    constexpr std::uint64_t kCount = 1'000'000;
    auto pattern = [](std::uint64_t seq) { return (((seq * 2654435761u) >> 7) & 1u) != 0; };
    const TransferResult r = transfer<bool>(
        8, kCount, [&](std::uint64_t seq) { return pattern(seq); },
        [&](bool v, std::uint64_t seq) { return v == pattern(seq); });
    OT_CHECK(!r.corrupt);
    OT_CHECK(!r.timed_out);
    OT_CHECK_EQ(r.received, kCount);
}

OT_TEST_MAIN()

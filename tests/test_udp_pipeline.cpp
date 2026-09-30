// End-to-end tests for the UDP demo pipeline (sender thread -> loopback UDP -> I/O thread ->
// SpscRing -> engine thread).
//
// The tests run the pipeline code of apps/ot_udp_demo.cpp itself: that file is included here with
// its main() renamed, so there is one implementation and no copy to drift out of step.
//
// Loopback UDP is lossy under bursts, so every scenario is sized to be immune to that. Each run
// moves a few thousand short messages, at most ~150 KB in total, which fits in the receive buffer
// several times over even if the I/O thread never got scheduled; the sender retries a full send
// buffer instead of giving up; and the assertions are exact equalities that follow from the
// construction (what was sent, what was skipped on purpose, what the ring can hold), never
// timing-dependent inequalities. Packets are counted, not predicted, wherever the pacing rate
// decides where a packet ends.

#include <chrono>
#include <cstdint>

#define main ot_udp_demo_main
#include "../apps/ot_udp_demo.cpp"
#undef main

#include "check.hpp"

namespace {

using ot_udp_demo::Options;
using ot_udp_demo::Report;

// Generous: a sanitizer build is many times slower than a release one, and a hang must still end.
constexpr std::uint64_t kTimeoutMs = 30'000;

Options base_options() {
    Options o;
    o.seed = 7;
    o.symbols = 4;
    o.messages = 3'000;
    o.rate = 200'000;  // 15 ms of traffic
    o.timeout_ms = kTimeoutMs;
    return o;
}

// The invariants of a run that ended normally, whatever losses it was set up to have.
void check_clean_shutdown(const Report& r) {
    OT_CHECK(r.setup_ok);
    OT_CHECK(!r.timed_out);
    OT_CHECK(!r.recv_failed);
    OT_CHECK_EQ(r.send_errors, 0u);
    OT_CHECK_EQ(r.malformed, 0u);
    OT_CHECK_EQ(r.oversize_datagrams, 0u);
    OT_CHECK_EQ(r.oversize_msgs, 0u);
    OT_CHECK_EQ(r.end_of_session, 1u);
    // Every message the I/O thread accepted either reached the ring or was counted as dropped,
    // and everything in the ring reached the engine.
    OT_CHECK_EQ(r.ring_pushed + r.ring_drops, r.msgs_received);
    OT_CHECK_EQ(r.msgs_processed, r.ring_pushed);
    OT_CHECK_EQ(static_cast<std::uint64_t>(r.latency_ns.size()), r.msgs_processed);
    OT_CHECK_EQ(r.stats.itch_messages + r.stats.itch_skipped, r.msgs_processed);
    // Each order the gateway swallowed was closed through the OUCH path.
    OT_CHECK_EQ(r.gateway_orders, r.stats.orders_sent);
    OT_CHECK_EQ(r.stats.ouch_reports, r.stats.orders_sent);
    OT_CHECK_EQ(r.stats.ouch_errors, 0u);
    // One on_feed_gap per loss event, and trading stops exactly when there was one.
    OT_CHECK_EQ(r.stats.feed_gaps, r.loss_events);
    OT_CHECK_EQ(r.trading_enabled, r.loss_events == 0);
    OT_CHECK_EQ(r.packets_sent + r.dropped_packets, r.data_packets + r.heartbeats_sent + 1);  // +1 end of session
}

OT_TEST(lossless_run_delivers_everything) {
    Report r;
    OT_CHECK(ot_udp_demo::run(base_options(), r));
    check_clean_shutdown(r);

    OT_CHECK(r.feed_messages > 3'000);
    OT_CHECK_EQ(r.msgs_sent, r.feed_messages);
    OT_CHECK_EQ(r.dropped_packets, 0u);
    OT_CHECK_EQ(r.packets_received, r.packets_sent);
    OT_CHECK_EQ(r.msgs_received, r.feed_messages);
    OT_CHECK_EQ(r.msgs_processed, r.feed_messages);
    OT_CHECK_EQ(r.seq_gaps, 0u);
    OT_CHECK_EQ(r.seq_missing, 0u);
    OT_CHECK_EQ(r.seq_duplicates, 0u);
    OT_CHECK_EQ(r.ring_drops, 0u);
    OT_CHECK_EQ(r.loss_events, 0u);

    // Several messages share a datagram, and the sender emitted heartbeats.
    OT_CHECK(r.data_packets * 4 < r.msgs_sent);
    OT_CHECK(r.heartbeats_sent > 0);

    // The engine saw the whole clean feed: every message decoded, none refused by the books, the
    // strategy stayed enabled and traded.
    OT_CHECK_EQ(r.stats.itch_messages, r.feed_messages);
    OT_CHECK_EQ(r.stats.itch_skipped, 0u);
    OT_CHECK_EQ(r.stats.book_errors, 0u);
    OT_CHECK(r.stats.book_updates > 2'000);
    OT_CHECK_EQ(r.stats.feed_gaps, 0u);
    OT_CHECK(r.trading_enabled);
    OT_CHECK(r.stats.orders_sent > 0);
}

// The decision path is a pure function of the message sequence: two runs over the same seed must
// agree on every engine counter, whatever the thread scheduling did.
OT_TEST(engine_outcome_is_independent_of_scheduling) {
    Report a, b;
    Options o = base_options();
    o.rate = 0;  // as fast as possible: the most different interleaving from the paced run
    OT_CHECK(ot_udp_demo::run(o, a));
    OT_CHECK(ot_udp_demo::run(base_options(), b));
    check_clean_shutdown(a);
    check_clean_shutdown(b);
    OT_CHECK_EQ(a.msgs_processed, b.msgs_processed);
    OT_CHECK_EQ(a.stats.itch_messages, b.stats.itch_messages);
    OT_CHECK_EQ(a.stats.book_updates, b.stats.book_updates);
    OT_CHECK_EQ(a.stats.orders_sent, b.stats.orders_sent);
    OT_CHECK_EQ(a.stats.fills, b.stats.fills);
    OT_CHECK_EQ(a.stats.risk_rejects, b.stats.risk_rejects);
}

OT_TEST(dropped_packets_are_counted_as_exact_gaps) {
    Report clean;
    OT_CHECK(ot_udp_demo::run(base_options(), clean));

    Options o = base_options();
    o.drop_every = 5;
    Report r;
    OT_CHECK(ot_udp_demo::run(o, r));
    check_clean_shutdown(r);

    // Every fifth data packet was skipped on purpose; each one is a separate hole that the next
    // packet (data, heartbeat or end of session) reveals, so gaps == skipped packets exactly.
    OT_CHECK(r.dropped_packets >= 3);
    OT_CHECK_EQ(r.dropped_packets, r.data_packets / 5);
    OT_CHECK_EQ(r.seq_gaps, r.dropped_packets);
    OT_CHECK_EQ(r.seq_missing, r.dropped_msgs);
    OT_CHECK_EQ(r.seq_duplicates, 0u);
    OT_CHECK_EQ(r.msgs_sent + r.dropped_msgs, r.feed_messages);
    OT_CHECK_EQ(r.msgs_received, r.msgs_sent);
    OT_CHECK_EQ(r.ring_drops, 0u);

    // The engine was told once per gap and stopped trading; it never traded more than a clean run.
    OT_CHECK_EQ(r.loss_events, r.seq_gaps);
    OT_CHECK_EQ(r.stats.feed_gaps, r.seq_gaps);
    OT_CHECK(!r.trading_enabled);
    OT_CHECK(r.stats.orders_sent < clean.stats.orders_sent);
    OT_CHECK_EQ(r.msgs_processed, r.msgs_received);  // the books keep applying messages while halted
}

// Loss of the very last data packet leaves no later data packet to expose it; only the
// end-of-session packet (which carries the next sequence number) can. With rate 0 the packet
// boundaries are fixed by the message sizes, so a clean run tells us how many data packets there
// are and the next run skips exactly the last one.
OT_TEST(lost_tail_is_revealed_by_end_of_session) {
    Options o = base_options();
    o.rate = 0;
    Report clean;
    OT_CHECK(ot_udp_demo::run(o, clean));
    check_clean_shutdown(clean);
    OT_CHECK(clean.data_packets >= 2);

    o.drop_every = clean.data_packets;
    Report r;
    OT_CHECK(ot_udp_demo::run(o, r));
    check_clean_shutdown(r);
    OT_CHECK_EQ(r.data_packets, clean.data_packets);
    OT_CHECK_EQ(r.dropped_packets, 1u);
    OT_CHECK(r.dropped_msgs > 0);
    OT_CHECK_EQ(r.seq_gaps, 1u);
    OT_CHECK_EQ(r.seq_missing, r.dropped_msgs);
    OT_CHECK_EQ(r.msgs_received, r.feed_messages - r.dropped_msgs);
    OT_CHECK_EQ(r.stats.feed_gaps, 1u);
    OT_CHECK(!r.trading_enabled);
}

// A ring that cannot keep up must never stall the I/O thread: the surplus is counted and dropped,
// and the engine is told. Holding the engine back until the I/O thread has finished makes this
// exact: the ring takes its 64 slots and everything after that is dropped.
OT_TEST(full_ring_drops_and_signals_a_gap) {
    Options o = base_options();
    o.rate = 0;
    o.ring_capacity = 64;
    o.hold_engine_until_io_done = true;
    Report r;
    OT_CHECK(ot_udp_demo::run(o, r));
    check_clean_shutdown(r);

    OT_CHECK_EQ(r.seq_gaps, 0u);  // the network was fine; the loss is downstream of it
    OT_CHECK_EQ(r.msgs_received, r.feed_messages);
    OT_CHECK_EQ(r.ring_pushed, 64u);
    OT_CHECK_EQ(r.ring_drops, r.feed_messages - 64);
    OT_CHECK_EQ(r.loss_events, 1u);  // one contiguous run of drops is one event
    OT_CHECK_EQ(r.msgs_processed, 64u);
    OT_CHECK_EQ(r.stats.feed_gaps, 1u);
    OT_CHECK(!r.trading_enabled);
}

// A run that cannot finish in time (2 messages per second) is aborted by the timeout, every
// thread is joined and the report says so.
OT_TEST(timeout_aborts_and_joins_all_threads) {
    Options o = base_options();
    o.messages = 500;
    o.rate = 2;
    o.timeout_ms = 300;
    const auto t0 = std::chrono::steady_clock::now();
    Report r;
    OT_CHECK(ot_udp_demo::run(o, r));  // returning at all proves the joins completed
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    OT_CHECK(r.timed_out);
    OT_CHECK(elapsed < std::chrono::seconds(10));
    OT_CHECK(r.msgs_sent < r.feed_messages);
}

OT_TEST(percentiles_are_nearest_rank) {
    std::vector<std::uint64_t> v;
    for (std::uint64_t i = 1; i <= 1000; ++i) v.push_back(i);
    OT_CHECK_EQ(ot_udp_demo::percentile(v, 500), 500u);
    OT_CHECK_EQ(ot_udp_demo::percentile(v, 990), 990u);
    OT_CHECK_EQ(ot_udp_demo::percentile(v, 999), 999u);
    OT_CHECK_EQ(ot_udp_demo::percentile(v, 1000), 1000u);
    OT_CHECK_EQ(ot_udp_demo::percentile(v, 1), 1u);
    OT_CHECK_EQ(ot_udp_demo::percentile({}, 500), 0u);
    OT_CHECK_EQ(ot_udp_demo::percentile({42}, 999), 42u);
}

}  // namespace

OT_TEST_MAIN()

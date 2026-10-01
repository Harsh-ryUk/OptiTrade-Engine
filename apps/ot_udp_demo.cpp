// ot_udp_demo: MoldUDP64 market data over loopback UDP into the trading engine, as a genuine
// three-thread pipeline.
//
//   sender thread                  I/O thread                          engine thread
//   -------------                  ----------                          -------------
//   pre-generated synthetic ITCH   UdpSocket::recv                     SpscRing::try_pop
//   -> PacketBuilder (many msgs    -> mold64::for_each_message         -> Engine::on_itch
//      per packet, real sequence   -> SequenceTracker (gap detection)     (ImbalanceTaker, discarding
//      numbers, heartbeats, EOS)   -> SpscRing::try_push (never waits)     gateway)
//   -> UdpSocket::send_to          count and drop when the ring is full -> on_feed_gap on any loss
//
// Loss handling. Anything that makes the book untrustworthy is a "loss event": a sequence gap
// seen by the tracker, a session change, an oversized message, or a run of messages the I/O
// thread had to drop because the ring was full. The I/O thread keeps a running loss counter
// (the epoch) and stamps it into every slot it pushes. The engine thread compares the stamp
// with the last one it has seen and calls Engine::on_feed_gap once per new event before it
// touches the first message published after the loss. The signal therefore travels in the data
// path, in order, and cannot itself be lost to a full ring. Losses after the last delivered
// message are picked up from the final epoch once the I/O thread has finished.
//
// After a gap the engine stays halted: the demo has no snapshot channel to rebuild the books
// from, so calling resume_trading() would trade on stale state. Engine::on_feed_gap only
// cancels working orders and stops the strategy callbacks and new submits; the books keep
// applying messages.
//
// Order flow. The discarding gateway drops every order on the floor, but a strategy that never
// hears back would leave each of its orders in flight and stall after one order per
// instrument. The engine thread therefore acknowledges every order it captured with an IOC
// "canceled" report after the message that produced it, which closes the order through the
// real OUCH decode path and the order manager and keeps the strategy trading.
//
// Latency. Each message is stamped with steady_clock when its datagram leaves recv(). The
// engine thread stamps again when Engine::on_itch has returned (before the acknowledgement
// above). The difference is receive-to-decision: it includes the ring hand-off, any queueing
// behind earlier messages of the same datagram and the strategy decision itself. Messages of
// one datagram share a receive stamp, so later messages in a big packet look slower; that is
// the honest cost of packet-level batching. Two clock reads per message are part of the number.
//
// Shutdown. The sender finishes with an end-of-session packet and raises `sender_done`. The I/O
// thread stops on that packet, or when the sender is done and the socket is empty (the packet
// itself may have been lost). It then raises `io_done`; the engine thread drains the ring and
// exits. Every wait loop also polls `abort`, which the main thread sets when the run exceeds its
// timeout, so a wedged run is reported as timed out instead of hanging.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include "optitrade/core/endian.hpp"
#include "optitrade/core/spsc_ring.hpp"
#include "optitrade/engine/engine.hpp"
#include "optitrade/itch/messages.hpp"
#include "optitrade/net/mold64.hpp"
#include "optitrade/net/udp.hpp"
#include "optitrade/ouch/codec.hpp"
#include "optitrade/sim/synthetic_market.hpp"
#include "optitrade/strategy/imbalance_taker.hpp"

namespace ot_udp_demo {

using namespace optitrade;
using Clock = std::chrono::steady_clock;

struct Options {
    std::uint64_t seed{1};
    std::uint64_t symbols{8};
    std::uint64_t messages{200'000};  // synthetic flow messages, after the fixed preamble
    std::uint64_t rate{100'000};      // messages per second, 0 = as fast as possible
    std::uint64_t drop_every{0};      // skip every Nth data packet, 0 = never (otherwise >= 2)
    std::uint64_t timeout_ms{0};      // 0 = derived from the message count and rate
    std::size_t ring_capacity{1u << 16};
    // Test hook, not exposed on the command line: the engine thread does not start consuming until
    // the I/O thread has finished, which makes a full ring (and so a ring-full drop) deterministic.
    bool hold_engine_until_io_done{false};
};

inline constexpr std::uint64_t kMaxSymbols = 1024;

struct Report {
    bool setup_ok{false};
    bool timed_out{false};
    bool recv_failed{false};

    std::uint64_t feed_messages{};      // messages in the pre-generated feed
    // Sender.
    std::uint64_t msgs_sent{};          // messages inside packets handed to the kernel
    std::uint64_t packets_sent{};       // data + heartbeat + end-of-session datagrams on the wire
    std::uint64_t heartbeats_sent{};
    std::uint64_t data_packets{};       // data packets built, including deliberately dropped ones
    std::uint64_t dropped_packets{};    // skipped on purpose (--drop-every)
    std::uint64_t dropped_msgs{};
    std::uint64_t send_errors{};
    // I/O thread.
    std::uint64_t packets_received{};   // valid datagrams
    std::uint64_t msgs_received{};      // messages taken out of valid data packets
    std::uint64_t malformed{};          // datagrams rejected by the MoldUDP64 parser
    std::uint64_t oversize_datagrams{}; // larger than the receive buffer
    std::uint64_t oversize_msgs{};      // longer than a ring slot
    std::uint64_t end_of_session{};
    std::uint64_t seq_gaps{};
    std::uint64_t seq_missing{};
    std::uint64_t seq_duplicates{};
    std::uint64_t ring_pushed{};
    std::uint64_t ring_drops{};         // messages dropped because the ring was full
    std::uint64_t loss_events{};        // gaps + drop runs + session changes + oversize messages
    // Engine thread.
    std::uint64_t msgs_processed{};
    std::uint64_t gateway_orders{};
    std::uint64_t gateway_cancels{};
    engine::Stats stats{};
    bool trading_enabled{true};
    std::vector<std::uint64_t> latency_ns;  // one sample per processed message
};

namespace detail {

constexpr std::size_t kMaxMsg = 47;  // largest ITCH message here is 44 bytes; leaves a 64-byte slot
static_assert(itch::kMaxMessageLength <= kMaxMsg);

// One cache line, fixed size, trivially copyable: what travels through the ring.
struct Slot {
    std::uint64_t rx_ns{};
    std::uint32_t loss_epoch{};
    std::uint8_t len{};
    std::byte data[kMaxMsg]{};
};
static_assert(sizeof(Slot) == 64);

struct FeedMsg {
    std::uint8_t len{};
    std::byte data[kMaxMsg]{};
};

constexpr std::array<char, net::mold64::kSessionSize> kSession{'O', 'P', 'T', 'I', 'T', 'R', 'A', 'D', 'E', ' '};
constexpr std::size_t kPacketBytes = 1200;          // stays inside a 1500-byte Ethernet MTU
constexpr std::uint64_t kHeartbeatEvery = 16;       // data packets between heartbeats
constexpr std::uint64_t kMaxBatchNs = 500'000;      // never hold a message longer than this to fill a packet
constexpr std::size_t kPendingAcks = 64;

inline std::uint64_t now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

// Order sink that discards everything but remembers new orders so the engine thread can close
// them (see the file comment). Touched by the engine thread only.
class DiscardGateway final : public oms::OrderGateway {
public:
    struct Pending {
        ouch::Token token;
        Qty shares{};
    };
    bool send(const ouch::EnterOrder& m, Nanos) override {
        ++orders;
        if (n < pending.size()) pending[n++] = {m.token, m.shares};
        return true;
    }
    bool send(const ouch::CancelOrder&, Nanos) override { return ++cancels != 0; }
    bool send(const ouch::ReplaceOrder&, Nanos) override { return ++replaces != 0; }

    std::array<Pending, kPendingAcks> pending{};
    std::size_t n{0};
    std::uint64_t orders{0}, cancels{0}, replaces{0};
};

struct Shared {
    std::atomic<bool> abort{false};
    std::atomic<bool> sender_done{false};
    std::atomic<bool> io_done{false};
    std::atomic<bool> engine_done{false};
    std::uint32_t final_epoch{0};  // written by the I/O thread before io_done, read after it
};

using Engine = engine::Engine<strategy::ImbalanceTaker>;

inline std::vector<FeedMsg> generate_feed(const Options& o) {
    sim::SyntheticConfig cfg;
    cfg.seed = o.seed;
    cfg.symbols = static_cast<std::uint16_t>(o.symbols);
    cfg.messages = o.messages;
    sim::SyntheticMarket market(cfg);
    std::vector<FeedMsg> feed;
    feed.reserve(o.messages + 1 + 13 * o.symbols + 16);
    std::array<std::byte, 64> buf{};
    std::size_t len = 0;
    Nanos ts = 0;
    while (market.next(buf, len, ts)) {
        FeedMsg m;
        m.len = static_cast<std::uint8_t>(len);
        std::memcpy(m.data, buf.data(), len);
        feed.push_back(m);
    }
    return feed;
}

// Waits until `due` without burning a core for long gaps. Sleep granularity is coarse (~100 us on
// a desktop kernel), so the last stretch is a yield loop.
inline void wait_until(Clock::time_point due, const std::atomic<bool>& abort) {
    for (;;) {
        if (abort.load(std::memory_order_relaxed)) return;
        const auto now = Clock::now();
        if (now >= due) return;
        if (due - now > std::chrono::microseconds(300)) {
            std::this_thread::sleep_for(due - now - std::chrono::microseconds(200));
        } else {
            std::this_thread::yield();
        }
    }
}

// A full send buffer is reported as failure by send_to; retry briefly rather than turn our own
// backpressure into packet loss that the receiver would then have to explain.
inline bool transmit(net::UdpSocket& s, std::uint16_t port, std::span<const std::byte> pkt,
                     const std::atomic<bool>& abort) {
    for (int attempt = 0; attempt < 2000; ++attempt) {
        if (s.send_to("127.0.0.1", port, pkt)) return true;
        if (abort.load(std::memory_order_relaxed)) return false;
        std::this_thread::yield();
    }
    return false;
}

inline void sender_main(Shared& sh, Report& r, const std::vector<FeedMsg>& feed, const Options& o,
                        net::UdpSocket& sock, std::uint16_t port) {
    namespace mold = net::mold64;
    std::array<std::byte, kPacketBytes> buf{};
    std::array<std::byte, mold::kHeaderSize> ctl{};
    std::uint64_t next_seq = 1;
    mold::PacketBuilder pb(buf, kSession, next_seq);
    Clock::time_point first_due{};
    std::uint64_t since_heartbeat = 0;

    auto send_control = [&](std::size_t n, bool heartbeat) {
        if (transmit(sock, port, std::span<const std::byte>(ctl.data(), n), sh.abort)) {
            ++r.packets_sent;
            if (heartbeat) ++r.heartbeats_sent;
        } else {
            ++r.send_errors;
        }
    };
    auto flush = [&] {
        if (pb.count() == 0) return;
        const std::span<const std::byte> pkt = pb.finish();
        const std::uint16_t count = pb.count();
        ++r.data_packets;
        if (o.drop_every != 0 && r.data_packets % o.drop_every == 0) {
            ++r.dropped_packets;
            r.dropped_msgs += count;
        } else if (transmit(sock, port, pkt, sh.abort)) {
            ++r.packets_sent;
            r.msgs_sent += count;
        } else {
            ++r.send_errors;
        }
        next_seq += count;
        pb = mold::PacketBuilder(buf, kSession, next_seq);
        if (++since_heartbeat >= kHeartbeatEvery) {
            since_heartbeat = 0;
            send_control(mold::write_heartbeat(ctl, kSession, next_seq), true);
        }
    };

    const Clock::time_point start = Clock::now();
    for (std::uint64_t i = 0; i < feed.size(); ++i) {
        if (sh.abort.load(std::memory_order_relaxed)) break;
        Clock::time_point due = start;
        if (o.rate != 0) {
            due = start + std::chrono::nanoseconds(i * 1'000'000'000ULL / o.rate);
            // Do not hold the early messages of a packet back while waiting for the rest of it.
            if (pb.count() != 0 && due - first_due > std::chrono::nanoseconds(kMaxBatchNs)) flush();
            wait_until(due, sh.abort);
        }
        const std::span<const std::byte> msg(feed[i].data, feed[i].len);
        if (!pb.add(msg)) {
            flush();
            pb.add(msg);  // an empty packet always has room for one message
        }
        if (pb.count() == 1) first_due = due;
    }
    flush();
    send_control(mold::write_end_of_session(ctl, kSession, next_seq), false);
    sh.sender_done.store(true, std::memory_order_release);
}

inline void io_main(Shared& sh, Report& r, net::UdpSocket& sock, SpscRing<Slot>& ring) {
    namespace mold = net::mold64;
    mold::SequenceTracker tracker;
    std::array<std::byte, 2048> buf{};
    std::uint32_t epoch = 0;
    bool in_drop_run = false;
    bool eos = false;

    auto push = [&](std::uint64_t rx, std::span<const std::byte> msg) {
        ++r.msgs_received;
        if (msg.empty() || msg.size() > kMaxMsg) {
            ++r.oversize_msgs;
            ++r.loss_events;
            ++epoch;
            return;
        }
        Slot s;
        s.rx_ns = rx;
        s.loss_epoch = epoch;
        s.len = static_cast<std::uint8_t>(msg.size());
        std::memcpy(s.data, msg.data(), msg.size());
        if (ring.try_push(s)) {
            ++r.ring_pushed;
            in_drop_run = false;
        } else {
            // Never wait for the consumer: a stalled engine must not back the kernel buffer up.
            // The first drop of a run is one loss event; later messages carry the new epoch.
            ++r.ring_drops;
            if (!in_drop_run) {
                in_drop_run = true;
                ++r.loss_events;
                ++epoch;
            }
        }
    };

    while (!sh.abort.load(std::memory_order_relaxed)) {
        // Read the flag before recv: everything the sender put on the wire before raising it is
        // already in the socket buffer, so an empty read after seeing it means we are drained.
        const bool sender_finished = sh.sender_done.load(std::memory_order_acquire);
        const std::ptrdiff_t n = sock.recv(buf, 1000);
        if (n < 0) {
            if (errno == EMSGSIZE) {
                ++r.oversize_datagrams;
                continue;
            }
            r.recv_failed = true;
            break;
        }
        if (n == 0) {
            if (sender_finished) break;
            continue;
        }
        const std::uint64_t rx = now_ns();
        const std::span<const std::byte> pkt(buf.data(), static_cast<std::size_t>(n));

        mold::Header hdr;
        mold::SequenceCheck chk;
        bool tracked = false;
        std::uint64_t skip_below = 0;  // messages already seen (duplicate packet) are not redelivered
        auto track = [&] {
            tracked = true;
            chk = tracker.on_packet(hdr);
            if (chk.result == mold::SequenceCheck::Result::gap ||
                chk.result == mold::SequenceCheck::Result::session_changed) {
                ++r.loss_events;
                ++epoch;
            } else if (chk.result == mold::SequenceCheck::Result::duplicate) {
                skip_below = chk.expected;
            }
        };
        // Tracking happens on the first message (or after the walk for control packets) so the
        // tracker never sees a packet the parser rejected, yet the epoch is bumped before any
        // message of a post-gap packet is pushed.
        const DecodeStatus st = mold::for_each_message(pkt, hdr, [&](std::uint64_t seq, std::span<const std::byte> msg) {
            if (!tracked) track();
            if (seq >= skip_below) push(rx, msg);
        });
        if (st != DecodeStatus::ok) {
            ++r.malformed;
            continue;
        }
        if (!tracked) track();
        ++r.packets_received;
        if (hdr.kind == mold::Kind::end_of_session) {
            ++r.end_of_session;
            eos = true;
        }
        if (eos) break;
    }
    r.seq_gaps = tracker.gaps();
    r.seq_missing = tracker.missing_messages();
    r.seq_duplicates = tracker.duplicates();
    sh.final_epoch = epoch;
    sh.io_done.store(true, std::memory_order_release);
}

inline void engine_main(Shared& sh, Report& r, SpscRing<Slot>& ring, Engine& eng, DiscardGateway& gw,
                        bool hold_until_io_done) {
    std::uint32_t seen_epoch = 0;
    Nanos now = 0;
    std::size_t n_lat = 0;

    auto handle_losses = [&](std::uint32_t epoch) {
        while (seen_epoch < epoch) {
            eng.on_feed_gap(now);
            ++seen_epoch;
        }
    };
    auto process = [&](const Slot& s) {
        if (s.len >= 11) now = be::load48(s.data + 5);  // the message's own ITCH timestamp
        handle_losses(s.loss_epoch);
        eng.on_itch(std::span<const std::byte>(s.data, s.len), now);
        const std::uint64_t done = now_ns();
        if (n_lat < r.latency_ns.size()) r.latency_ns[n_lat++] = done - s.rx_ns;
        ++r.msgs_processed;
        // Close what the strategy just sent (see the file comment).
        for (std::size_t i = 0; i < gw.n; ++i) {
            ouch::Canceled c;
            c.ts = now;
            c.token = gw.pending[i].token;
            c.decrement = gw.pending[i].shares;
            c.reason = 'I';
            std::array<std::byte, 64> wire{};
            const std::size_t len = ouch::encode(c, wire);
            eng.on_ouch(std::span<const std::byte>(wire.data(), len), now);
        }
        gw.n = 0;
    };

    while (hold_until_io_done && !sh.io_done.load(std::memory_order_acquire) &&
           !sh.abort.load(std::memory_order_relaxed)) {
        std::this_thread::yield();
    }

    Slot s;
    while (!sh.abort.load(std::memory_order_relaxed)) {
        if (ring.try_pop(s)) {
            process(s);
            continue;
        }
        if (sh.io_done.load(std::memory_order_acquire)) {
            // Everything the I/O thread pushed is visible now; drain what is left, then finish.
            while (ring.try_pop(s)) process(s);
            handle_losses(sh.final_epoch);
            break;
        }
        std::this_thread::yield();
    }
    r.latency_ns.resize(n_lat);
    sh.engine_done.store(true, std::memory_order_release);
}

}  // namespace detail

// Runs the whole pipeline once and fills `r`. Returns false if the socket could not be set up.
inline bool run(const Options& o, Report& r) {
    using namespace detail;
    r = Report{};

    // Receiver first, on an ephemeral port, so the sender can never fire at an unbound port.
    net::UdpSocket rx = net::UdpSocket::receiver("127.0.0.1", 0, 8 << 20);
    net::UdpSocket tx = net::UdpSocket::sender();
    if (!rx.valid() || !tx.valid()) return false;
    const std::uint16_t port = rx.local_port();
    if (port == 0) return false;

    const std::vector<FeedMsg> feed = generate_feed(o);
    r.feed_messages = feed.size();
    r.latency_ns.assign(feed.size(), 0);

    DiscardGateway gw;
    engine::Config ec;
    ec.books.max_orders = static_cast<std::size_t>(o.symbols) * 512 + 4096;
    ec.books.max_levels_per_side = 128;
    ec.books.max_symbols = static_cast<std::size_t>(o.symbols) + 8;
    ec.oms.max_orders = 1u << 16;
    ec.max_locates = static_cast<std::size_t>(o.symbols) + 1;  // locates are 1-based
    strategy::ImbalanceTakerConfig sc;
    sc.max_locates = ec.max_locates;
    auto eng = std::make_unique<Engine>(ec, gw, strategy::ImbalanceTaker(sc));
    SpscRing<Slot> ring(o.ring_capacity);

    Shared sh;
    std::uint64_t timeout_ms = o.timeout_ms;
    if (timeout_ms == 0) timeout_ms = 10'000 + (o.rate != 0 ? 2 * 1000 * feed.size() / o.rate : 0);

    // Consumer first, producer last: nothing is ever sent before every stage is listening.
    std::thread engine_thread([&] { engine_main(sh, r, ring, *eng, gw, o.hold_engine_until_io_done); });
    std::thread io_thread([&] { io_main(sh, r, rx, ring); });
    std::thread sender_thread([&] { sender_main(sh, r, feed, o, tx, port); });

    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!sh.engine_done.load(std::memory_order_acquire)) {
        if (Clock::now() >= deadline) {
            r.timed_out = true;
            sh.abort.store(true, std::memory_order_relaxed);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    sender_thread.join();
    io_thread.join();
    engine_thread.join();

    r.stats = eng->stats();
    r.trading_enabled = eng->trading_enabled();
    r.gateway_orders = gw.orders;
    r.gateway_cancels = gw.cancels;
    r.setup_ok = true;
    return true;
}

// Exact nearest-rank percentile of a sorted sample: the smallest value with at least
// per_mille/1000 of the samples at or below it. Integer arithmetic, so no rounding surprises.
inline std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, std::uint64_t per_mille) {
    if (sorted.empty()) return 0;
    const std::uint64_t n = sorted.size();
    const std::uint64_t rank = std::clamp<std::uint64_t>((per_mille * n + 999) / 1000, 1, n);
    return sorted[rank - 1];
}

inline void print_report(const Options& o, const Report& r, std::FILE* out) {
    auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    std::fprintf(out, "ot_udp_demo  seed=%llu symbols=%llu messages=%llu rate=%llu/s drop_every=%llu\n",
                 u(o.seed), u(o.symbols), u(o.messages), u(o.rate), u(o.drop_every));
    std::fprintf(out, "\nsender\n");
    std::fprintf(out, "  feed messages        %llu\n", u(r.feed_messages));
    std::fprintf(out, "  messages sent        %llu\n", u(r.msgs_sent));
    std::fprintf(out, "  packets sent         %llu  (%llu heartbeats)\n", u(r.packets_sent), u(r.heartbeats_sent));
    std::fprintf(out, "  data packets built   %llu\n", u(r.data_packets));
    std::fprintf(out, "  skipped on purpose   %llu packets, %llu messages\n", u(r.dropped_packets), u(r.dropped_msgs));
    std::fprintf(out, "  send errors          %llu\n", u(r.send_errors));
    std::fprintf(out, "\nreceiver (I/O thread)\n");
    std::fprintf(out, "  packets received     %llu  (%llu end-of-session)\n", u(r.packets_received), u(r.end_of_session));
    std::fprintf(out, "  messages received    %llu\n", u(r.msgs_received));
    std::fprintf(out, "  sequence gaps        %llu  (%llu messages missing, %llu duplicates)\n", u(r.seq_gaps),
                 u(r.seq_missing), u(r.seq_duplicates));
    std::fprintf(out, "  malformed datagrams  %llu  (oversize %llu, oversize messages %llu)\n", u(r.malformed),
                 u(r.oversize_datagrams), u(r.oversize_msgs));
    std::fprintf(out, "  ring pushed          %llu\n", u(r.ring_pushed));
    std::fprintf(out, "  ring-full drops      %llu\n", u(r.ring_drops));
    std::fprintf(out, "  loss events          %llu\n", u(r.loss_events));
    std::fprintf(out, "\nengine thread\n");
    std::fprintf(out, "  messages processed   %llu\n", u(r.msgs_processed));
    std::fprintf(out, "  itch %llu  skipped %llu  book updates %llu  book errors %llu\n", u(r.stats.itch_messages),
                 u(r.stats.itch_skipped), u(r.stats.book_updates), u(r.stats.book_errors));
    std::fprintf(out, "  orders sent %llu  risk rejects %llu  fills %llu  ouch reports %llu\n", u(r.stats.orders_sent),
                 u(r.stats.risk_rejects), u(r.stats.fills), u(r.stats.ouch_reports));
    std::fprintf(out, "  feed gaps handled    %llu  (trading %s)\n", u(r.stats.feed_gaps),
                 r.trading_enabled ? "enabled" : "halted");

    std::vector<std::uint64_t> sorted = r.latency_ns;
    std::sort(sorted.begin(), sorted.end());
    std::fprintf(out, "\nreceive-to-decision latency (steady_clock, %zu samples, ns)\n", sorted.size());
    if (sorted.empty()) {
        std::fprintf(out, "  no samples\n");
    } else {
        std::uint64_t sum = 0;
        for (std::uint64_t v : sorted) sum += v;
        std::fprintf(out, "  min %llu  p50 %llu  p90 %llu  p99 %llu  p99.9 %llu  max %llu  mean %llu\n", u(sorted.front()),
                     u(percentile(sorted, 500)), u(percentile(sorted, 900)), u(percentile(sorted, 990)),
                     u(percentile(sorted, 999)), u(sorted.back()), u(sum / sorted.size()));
    }
    if (r.timed_out) std::fprintf(out, "\nWARNING: run timed out and was aborted\n");
    if (r.recv_failed) std::fprintf(out, "\nWARNING: receive failed with a socket error\n");
}

}  // namespace ot_udp_demo

namespace {

void usage(std::FILE* out) {
    std::fputs(
        "usage: ot_udp_demo [--messages N] [--symbols N] [--rate N] [--drop-every N] [--seed N]\n"
        "\n"
        "MoldUDP64 over UDP loopback: a sender thread paces a synthetic ITCH feed into packets, an\n"
        "I/O thread checks sequencing and hands messages to the engine thread through a lock-free\n"
        "ring, and the engine (imbalance taker, orders discarded) decides on each one.\n"
        "\n"
        "  --messages N     synthetic flow messages after the preamble (default 200000)\n"
        "  --symbols N      instruments, 1..1024 (default 8)\n"
        "  --rate N         messages per second, 0 = as fast as possible (default 100000)\n"
        "  --drop-every N   skip every Nth data packet to simulate loss, N >= 2 (default 0 = off)\n"
        "  --seed N         random seed (default 1)\n"
        "  --help           show this text\n"
        "\n"
        "Prints packets and messages sent and received, sequence gaps, ring-full drops, engine\n"
        "counters and receive-to-decision latency percentiles. Exits 1 if the run timed out or a\n"
        "socket failed, 2 on a usage error.\n",
        out);
}

bool parse_u64(const char* text, std::uint64_t max, std::uint64_t& out) {
    if (text[0] < '0' || text[0] > '9') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || v > max) return false;
    out = v;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    ot_udp_demo::Options o;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            usage(stdout);
            return 0;
        }
        std::uint64_t* target = nullptr;
        std::uint64_t max = ~std::uint64_t{0};
        std::uint64_t min = 0;
        if (std::strcmp(arg, "--messages") == 0) {
            target = &o.messages, max = 100'000'000, min = 1;
        } else if (std::strcmp(arg, "--symbols") == 0) {
            target = &o.symbols, max = ot_udp_demo::kMaxSymbols, min = 1;
        } else if (std::strcmp(arg, "--rate") == 0) {
            target = &o.rate, max = 1'000'000'000;
        } else if (std::strcmp(arg, "--drop-every") == 0) {
            target = &o.drop_every, max = 1'000'000'000;
        } else if (std::strcmp(arg, "--seed") == 0) {
            target = &o.seed;
        } else {
            std::fprintf(stderr, "ot_udp_demo: unknown argument '%s' (try --help)\n", arg);
            return 2;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "ot_udp_demo: %s needs a value\n", arg);
            return 2;
        }
        std::uint64_t v = 0;
        if (!parse_u64(argv[++i], max, v) || v < min || (target == &o.drop_every && v == 1)) {
            std::fprintf(stderr, "ot_udp_demo: bad value '%s' for %s\n", argv[i], arg);
            return 2;
        }
        *target = v;
    }

    ot_udp_demo::Report r;
    if (!ot_udp_demo::run(o, r)) {
        std::fprintf(stderr, "ot_udp_demo: could not open loopback UDP sockets\n");
        return 1;
    }
    ot_udp_demo::print_report(o, r, stdout);
    return (r.timed_out || r.recv_failed) ? 1 : 0;
}

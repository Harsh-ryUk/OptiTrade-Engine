// ot_bench: tick-to-decision latency benchmark.
//
// What is measured
//   The wall time of one Engine::on_itch() call for one pre-generated ITCH message: decode,
//   order book update, risk mark, the strategy callback, and (when the strategy trades) the
//   pre-trade risk check, order construction and a gateway send. Nothing else is inside the
//   timed region. The gateway accepts and discards; it only queues a copy of the order so
//   the exchange replies below can be produced outside the timed region.
//
// Method
//   1. Generate the whole feed in memory first (SyntheticMarket, seeded), so generation and
//      file/socket I/O are never timed. The feed is: fixed preamble (system event, stock
//      directories, initial depth), then `--warmup` flow messages, then `--messages`
//      measured flow messages. Every buffer (feed, sample arrays, order outbox) is sized
//      before any timing starts; nothing allocates while the clock runs.
//   2. Every pass runs on a fresh engine (a feed can only be applied once: order references
//      are unique). Preamble and warmup messages are applied first, untimed, so the timed
//      region starts on populated books with warm caches and branch predictors. Only the
//      last `--messages` messages are timed.
//   3. Pass 0, warm-up: the whole feed on a scratch engine; results discarded. It faults in
//      the feed pages and warms the instruction cache before anything is recorded.
//   4. Pass 1, throughput: the measured region timed once as a whole (two clock reads
//      total). Reports mean ns/message and messages/s. Exchange replies (below) are
//      processed inside this pass, so it is a whole-event-loop rate, slightly below
//      1 / mean(service time).
//   5. Pass 2, closed-loop latency: each on_itch() call timed individually, back to back.
//      Messages arrive as fast as the engine finishes the previous one; per-call service
//      time is the metric. Because the offered load adapts to the engine, a stall delays
//      the following messages without being charged to them (coordinated omission), so
//      this pass reports service time only.
//   6. Pass 3, open-loop latency (only with --rate EPS > 0): message i is scheduled at
//      t0 + i * (1e9 / EPS) ns, independent of how long earlier messages took. The driver
//      spins until the scheduled instant, then times the call. Two figures per message:
//        service  = end - start      (what the engine spent on it)
//        response = end - intended   (what a sender on that schedule experiences)
//      If the engine stalls, every message queued behind the stall starts late and its
//      response time includes the wait, so the stall is charged to all of them. That is the
//      coordinated-omission-safe view. If the offered rate exceeds capacity the backlog
//      grows for the whole pass; the report then shows the final backlog and a warning.
//      The driver's own work between two calls (timer read, schedule arithmetic, reply
//      handling) is not part of service time but does delay the next start, so under
//      overload the backlog grows faster than (service - period) alone would suggest.
//   7. Percentiles are exact, taken from the sorted samples (nearest rank: the smallest
//      value with at least p of the samples at or below it). Sample arrays are sorted after
//      timing, never during.
//
// Exchange replies. A strategy that never hears back stalls after its first order (one
// outstanding order per instrument, quotes never become live), so a bare null gateway would
// benchmark an idle strategy. By default the harness therefore answers each queued order
// after the timed call returns: IOC orders are accepted and fully executed at their limit
// price, day orders are accepted and rest, cancels and replaces are confirmed. These
// replies go through Engine::on_ouch() outside the timed region (in open-loop mode the time
// they take delays the next start like any other work between messages). The only cost
// inside the timed region is the copy of the order into the outbox (tens of bytes). Use
// --no-reports for a pure null gateway; the strategies then trade a handful of orders.
//
// Timer
//   x86-64: `lfence; rdtsc; lfence`, calibrated against std::chrono::steady_clock over
//   ~50 ms at start-up. This assumes an invariant TSC (constant rate across cores and
//   P-states), true on every x86-64 CPU of the last decade; the banner prints the measured
//   frequency so a wrong value is visible. Other architectures: steady_clock directly.
//   Each timed call contains two timer reads, so the reported figures include one read of
//   overhead. The read floor (back-to-back read pairs) and the timer's smallest step are
//   measured and printed; compare tail figures against them. On Apple Silicon the system
//   counter behind steady_clock ticks at 24 MHz, i.e. steps of ~41.7 ns, which quantises
//   every per-call sample to that grid.
//
// Results cannot be optimised away: the engine escapes into an empty asm statement with a
// memory clobber around every call, statuses are folded into a checksum that is printed,
// and the engine counters are compared across passes (identical input must give identical
// counters; a difference is reported as an error).
//
// Output: an environment banner, a percentile table, and with --csv FILE a machine-readable
// file (header: strategy,pass,metric,count,mean_ns,p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,
// max_ns,msgs_per_s; one row per measured series).
//
// CPU pinning: --cpu N uses sched_setaffinity on Linux. macOS has no thread-affinity API
// that can pin to a core; the option is accepted there, reported as unsupported and
// ignored. For stable tails on Linux also isolate the core (isolcpus, nohz_full) and fix
// the frequency governor; the tool does not do that for you.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#define OT_BENCH_RDTSC 1
#endif
#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#include <sys/utsname.h>

#include "optitrade/core/flat_hash_map.hpp"
#include "optitrade/engine/engine.hpp"
#include "optitrade/itch/messages.hpp"
#include "optitrade/ouch/codec.hpp"
#include "optitrade/sim/synthetic_market.hpp"
#include "optitrade/strategy/ema_cross.hpp"
#include "optitrade/strategy/imbalance_taker.hpp"
#include "optitrade/strategy/microprice_maker.hpp"

namespace {

using namespace optitrade;

constexpr int kExitUsage = 2;
// A message counts as "started late" when the engine was still busy (or descheduled) for
// longer than this after the message was due. Well above the timer step and spin-exit jitter.
constexpr double kLateNs = 1000.0;

// ---- small utilities ---------------------------------------------------------------------------

// Keeps the compiler from deleting or hoisting the measured work.
template <class T>
inline void keep(const T& v) noexcept {
    asm volatile("" : : "g"(&v) : "memory");
}

// ---- timer -------------------------------------------------------------------------------------

class Timer {
public:
    // Ticks of the underlying counter. Monotonic within one thread on one core.
    static std::uint64_t ticks() noexcept {
#ifdef OT_BENCH_RDTSC
        // lfence on both sides stops the CPU from moving the counter read across the work
        // being timed; without it out-of-order execution smears the measurement.
        _mm_lfence();
        const std::uint64_t t = __rdtsc();
        _mm_lfence();
        return t;
#else
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
#endif
    }

    // Measures the tick rate against steady_clock. A no-op for the steady_clock timer.
    void calibrate() {
#ifdef OT_BENCH_RDTSC
        using clock = std::chrono::steady_clock;
        const auto w0 = clock::now();
        const std::uint64_t t0 = ticks();
        while (clock::now() - w0 < std::chrono::milliseconds(50)) {
        }
        const auto w1 = clock::now();
        const std::uint64_t t1 = ticks();
        const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(w1 - w0).count());
        ticks_per_ns_ = static_cast<double>(t1 - t0) / ns;
#endif
    }

    double ticks_per_ns() const noexcept { return ticks_per_ns_; }
    double to_ns(std::uint64_t t) const noexcept { return static_cast<double>(t) / ticks_per_ns_; }
    std::uint64_t from_ns(double ns) const noexcept { return static_cast<std::uint64_t>(ns * ticks_per_ns_ + 0.5); }

    const char* description() const noexcept {
#ifdef OT_BENCH_RDTSC
        return "rdtsc + lfence, calibrated against steady_clock";
#else
        return "std::chrono::steady_clock";
#endif
    }

private:
    double ticks_per_ns_{1.0};
};

// ---- statistics --------------------------------------------------------------------------------

struct Series {
    std::vector<std::uint64_t> v;  // ticks; sorted in place by finish()
    double mean_ticks{};

    void finish() {
        long double sum = 0;
        for (const std::uint64_t x : v) sum += static_cast<long double>(x);
        mean_ticks = v.empty() ? 0.0 : static_cast<double>(sum / static_cast<long double>(v.size()));
        std::sort(v.begin(), v.end());
    }

    // Nearest rank: smallest sample with at least num/10000 of the samples at or below it.
    std::uint64_t percentile(std::uint64_t num) const noexcept {
        if (v.empty()) return 0;
        std::uint64_t rank = (num * v.size() + 9999) / 10000;
        rank = std::clamp<std::uint64_t>(rank, 1, v.size());
        return v[rank - 1];
    }
    std::uint64_t max() const noexcept { return v.empty() ? 0 : v.back(); }
};

struct Row {
    std::string label;   // e.g. "closed-loop service"
    std::string pass;    // csv pass name
    std::string metric;  // csv metric name
    const Series* series;
};

// ---- options -----------------------------------------------------------------------------------

enum class Which { imbalance, maker, ema, all };

struct Options {
    Which which{Which::imbalance};
    std::uint64_t symbols{8};
    std::uint64_t messages{1'000'000};
    std::uint64_t warmup{100'000};
    std::uint64_t rate{0};
    std::uint64_t seed{1};
    bool cpu_set{false};
    std::uint64_t cpu{0};
    const char* csv{nullptr};
    bool reports{true};
};

void usage(std::FILE* out) {
    std::fputs(
        "usage: ot_bench [--strategy imbalance|maker|ema|all] [--symbols N] [--messages N]\n"
        "                [--warmup N] [--rate EPS] [--cpu N] [--csv FILE] [--seed N]\n"
        "                [--no-reports]\n"
        "\n"
        "Measures Engine::on_itch() latency (book update + strategy + risk + order send) on a\n"
        "seeded synthetic feed that is generated in memory before any timing starts.\n"
        "\n"
        "  --strategy S    imbalance (default), maker, ema, or all three in turn\n"
        "  --symbols N     instruments in the feed, 1..4096 (default 8)\n"
        "  --messages N    measured messages, >= 1 (default 1000000)\n"
        "  --warmup N      flow messages applied untimed before the measured ones (default 100000)\n"
        "  --rate EPS      open loop: offer EPS messages/s on a fixed schedule and report\n"
        "                  service and response time; 0 = closed loop only (default)\n"
        "  --cpu N         pin the thread to CPU N (Linux; unsupported and ignored on macOS)\n"
        "  --csv FILE      also write the results as CSV (overwritten)\n"
        "  --seed N        feed seed (default 1)\n"
        "  --no-reports    pure null gateway: do not answer orders with exchange reports\n"
        "  --help          show this text\n"
        "\n"
        "Method and caveats are documented at the top of apps/ot_bench.cpp.\n",
        out);
}

// Whole-string unsigned decimal in [0, max]; rejects signs, blanks, junk and overflow.
bool parse_u64(const char* text, std::uint64_t max, std::uint64_t& out) {
    if (text[0] < '0' || text[0] > '9') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || v > max) return false;
    out = v;
    return true;
}

// Returns -1 to continue, otherwise the process exit code.
int parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            usage(stdout);
            return 0;
        }
        if (std::strcmp(arg, "--no-reports") == 0) {
            o.reports = false;
            continue;
        }
        const bool is_strategy = std::strcmp(arg, "--strategy") == 0;
        const bool is_symbols = std::strcmp(arg, "--symbols") == 0;
        const bool is_messages = std::strcmp(arg, "--messages") == 0;
        const bool is_warmup = std::strcmp(arg, "--warmup") == 0;
        const bool is_rate = std::strcmp(arg, "--rate") == 0;
        const bool is_cpu = std::strcmp(arg, "--cpu") == 0;
        const bool is_csv = std::strcmp(arg, "--csv") == 0;
        const bool is_seed = std::strcmp(arg, "--seed") == 0;
        if (!is_strategy && !is_symbols && !is_messages && !is_warmup && !is_rate && !is_cpu && !is_csv &&
            !is_seed) {
            std::fprintf(stderr, "ot_bench: unknown argument '%s' (try --help)\n", arg);
            return kExitUsage;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "ot_bench: %s needs a value\n", arg);
            return kExitUsage;
        }
        const char* value = argv[++i];
        std::uint64_t n = 0;
        bool ok = true;
        if (is_strategy) {
            if (std::strcmp(value, "imbalance") == 0) o.which = Which::imbalance;
            else if (std::strcmp(value, "maker") == 0) o.which = Which::maker;
            else if (std::strcmp(value, "ema") == 0) o.which = Which::ema;
            else if (std::strcmp(value, "all") == 0) o.which = Which::all;
            else ok = false;
        } else if (is_csv) {
            o.csv = value;
            ok = value[0] != '\0';
        } else if (is_symbols) {
            ok = parse_u64(value, 4096, n) && n >= 1;
            o.symbols = n;
        } else if (is_messages) {
            ok = parse_u64(value, std::uint64_t{1} << 32, n) && n >= 1;
            o.messages = n;
        } else if (is_warmup) {
            ok = parse_u64(value, std::uint64_t{1} << 32, n);
            o.warmup = n;
        } else if (is_rate) {
            ok = parse_u64(value, 1'000'000'000, n);
            o.rate = n;
        } else if (is_cpu) {
            ok = parse_u64(value, 4095, n);
            o.cpu = n;
            o.cpu_set = true;
        } else if (is_seed) {
            ok = parse_u64(value, UINT64_MAX, n);
            o.seed = n;
        }
        if (!ok) {
            std::fprintf(stderr, "ot_bench: invalid value '%s' for %s\n", value, arg);
            return kExitUsage;
        }
    }
    return -1;
}

// ---- pre-generated feed ------------------------------------------------------------------------

struct Feed {
    std::vector<std::byte> bytes;
    std::vector<std::uint32_t> offset;  // offset[i]..offset[i+1] is message i
    std::vector<Nanos> ts;
    std::size_t prefix{};  // messages applied untimed (preamble + warmup)
    std::size_t preamble{};
    std::uint64_t type_count[256]{};  // over the measured region

    std::size_t size() const noexcept { return ts.size(); }
    std::size_t measured() const noexcept { return size() - prefix; }
    std::span<const std::byte> msg(std::size_t i) const noexcept {
        return {bytes.data() + offset[i], offset[i + 1] - offset[i]};
    }
};

bool build_feed(const Options& o, Feed& f) {
    sim::SyntheticConfig sc;
    sc.seed = o.seed;
    sc.symbols = static_cast<std::uint16_t>(o.symbols);
    sc.messages = o.warmup + o.messages;
    sim::SyntheticMarket market(sc);

    // Upper bound on the count: preamble = 1 + symbols + symbols * 24 (see synthetic_market.hpp).
    const std::size_t expect = 1 + o.symbols + o.symbols * 24 + sc.messages;
    f.bytes.reserve(expect * itch::kMaxMessageLength);
    f.offset.reserve(expect + 1);
    f.ts.reserve(expect);
    std::byte buf[itch::kMaxMessageLength];
    std::size_t len = 0;
    Nanos ts = 0;
    f.offset.push_back(0);
    while (market.next(buf, len, ts)) {
        f.bytes.insert(f.bytes.end(), buf, buf + len);
        if (f.bytes.size() > UINT32_MAX) return false;  // offsets are 32 bit
        f.offset.push_back(static_cast<std::uint32_t>(f.bytes.size()));
        f.ts.push_back(ts);
    }
    if (f.ts.size() < o.messages + 1) return false;
    f.preamble = f.ts.size() - static_cast<std::size_t>(sc.messages);
    f.prefix = f.ts.size() - static_cast<std::size_t>(o.messages);
    for (std::size_t i = f.prefix; i < f.ts.size(); ++i) ++f.type_count[static_cast<unsigned char>(f.bytes[f.offset[i]])];
    return true;
}

// ---- gateway that discards, plus the reply generator -------------------------------------------

// Accepts every order. When reports are enabled it also keeps a copy of the message so the
// harness can answer it after the timed call has returned.
class BenchGateway final : public oms::OrderGateway {
public:
    enum class Kind : std::uint8_t { enter, cancel, replace };
    struct Out {
        Kind kind{Kind::enter};
        ouch::EnterOrder enter;
        ouch::CancelOrder cancel;
        ouch::ReplaceOrder replace;
    };
    static constexpr std::size_t kOutbox = 256;

    explicit BenchGateway(bool keep_copies) : keep_(keep_copies), box_(keep_copies ? kOutbox : 0) {}

    bool send(const ouch::EnterOrder& m, Nanos) override {
        ++enters;
        if (keep_) push(Kind::enter).enter = m;
        return true;
    }
    bool send(const ouch::CancelOrder& m, Nanos) override {
        ++cancels;
        if (keep_) push(Kind::cancel).cancel = m;
        return true;
    }
    bool send(const ouch::ReplaceOrder& m, Nanos) override {
        ++replaces;
        if (keep_) push(Kind::replace).replace = m;
        return true;
    }

    bool pending() const noexcept { return head_ != tail_; }
    const Out& front() const noexcept { return box_[head_ % kOutbox]; }
    void pop() noexcept { ++head_; }

    std::uint64_t enters{}, cancels{}, replaces{};
    std::uint64_t dropped{};  // outbox overflow: order sent but never answered

private:
    Out& push(Kind k) noexcept {
        if (tail_ - head_ == kOutbox) {  // cannot happen with one drain per message; counted, not fatal
            ++dropped;
            ++head_;
        }
        Out& o = box_[tail_ % kOutbox];
        ++tail_;
        o.kind = k;
        return o;
    }

    bool keep_;
    std::vector<Out> box_;
    std::uint64_t head_{0}, tail_{0};
};

// Turns queued orders into the replies an exchange would send. Kept apart from the gateway so
// that everything here runs outside the timed region.
template <class Eng>
class ExchangeReplies {
public:
    explicit ExchangeReplies(std::size_t max_live) : live_(max_live) {}

    void pump(BenchGateway& gw, Eng& eng, Nanos now) noexcept {
        // A reply can make the strategy send again (for example a fill triggers an exit), so
        // drain until quiet. The bound is a guard against a strategy that loops on replies.
        for (int rounds = 0; gw.pending() && rounds < 64; ++rounds) {
            while (gw.pending()) {
                answer(gw.front(), eng, now);
                gw.pop();
            }
        }
    }

    std::uint64_t replies{};

private:
    struct Live {
        Qty qty{};
        Side side{};
        Symbol stock;
        Price price{};
    };

    void deliver(Eng& eng, std::size_t n, Nanos now) noexcept {
        ++replies;
        keep(eng.on_ouch(std::span<const std::byte>(wire_, n), now));
    }

    void answer(const BenchGateway::Out& o, Eng& eng, Nanos now) noexcept {
        switch (o.kind) {
        case BenchGateway::Kind::enter: {
            const ouch::EnterOrder& e = o.enter;
            ouch::Accepted a;
            a.ts = now;
            a.token = e.token;
            a.side = e.side;
            a.shares = e.shares;
            a.stock = e.stock;
            a.price = e.price;
            a.time_in_force = e.time_in_force;
            a.ref = ++exch_ref_;
            deliver(eng, ouch::encode(a, wire_), now);
            if (e.time_in_force == ouch::kTifIoc) {
                ouch::Executed x;
                x.ts = now;
                x.token = e.token;
                x.shares = e.shares;
                x.price = e.price;
                x.match = ++match_;
                deliver(eng, ouch::encode(x, wire_), now);
            } else if (const auto id = e.token.to_id()) {
                live_.insert(*id, Live{e.shares, e.side, e.stock, e.price});
            }
            break;
        }
        case BenchGateway::Kind::cancel: {
            const auto id = o.cancel.token.to_id();
            const Live* l = id ? live_.find(*id) : nullptr;
            if (l == nullptr) break;
            ouch::Canceled c;
            c.ts = now;
            c.token = o.cancel.token;
            c.decrement = l->qty;
            live_.erase(*id);
            deliver(eng, ouch::encode(c, wire_), now);
            break;
        }
        case BenchGateway::Kind::replace: {
            const ouch::ReplaceOrder& r = o.replace;
            const auto old_id = r.existing.to_id();
            const Live* l = old_id ? live_.find(*old_id) : nullptr;
            const auto new_id = r.replacement.to_id();
            if (l == nullptr || !new_id) break;
            ouch::Replaced rp;
            rp.a.ts = now;
            rp.a.token = r.replacement;
            rp.a.side = l->side;
            rp.a.shares = r.shares;
            rp.a.stock = l->stock;
            rp.a.price = r.price;
            rp.a.time_in_force = r.time_in_force;
            rp.a.ref = ++exch_ref_;
            rp.previous = r.existing;
            const Live moved{r.shares, l->side, l->stock, r.price};
            live_.erase(*old_id);
            live_.insert(*new_id, moved);
            deliver(eng, ouch::encode(rp, wire_), now);
            break;
        }
        }
    }

    FlatHashMap<std::uint64_t, Live> live_;
    std::byte wire_[128]{};
    std::uint64_t exch_ref_{0}, match_{0};
};

// ---- one engine and its harness ----------------------------------------------------------------

struct Counters {
    engine::Stats s;
    std::uint64_t enters{}, cancels{}, replaces{}, replies{}, dropped{};
    bool operator==(const Counters& o) const noexcept {
        return s.itch_messages == o.s.itch_messages && s.itch_skipped == o.s.itch_skipped &&
               s.book_updates == o.s.book_updates && s.book_errors == o.s.book_errors &&
               s.ouch_reports == o.s.ouch_reports && s.ouch_errors == o.s.ouch_errors &&
               s.orders_sent == o.s.orders_sent && s.risk_rejects == o.s.risk_rejects && s.fills == o.s.fills &&
               enters == o.enters && cancels == o.cancels && replaces == o.replaces && replies == o.replies &&
               dropped == o.dropped;
    }
};

Counters diff(const Counters& a, const Counters& b) {
    Counters d;
    d.s.itch_messages = a.s.itch_messages - b.s.itch_messages;
    d.s.itch_skipped = a.s.itch_skipped - b.s.itch_skipped;
    d.s.book_updates = a.s.book_updates - b.s.book_updates;
    d.s.book_errors = a.s.book_errors - b.s.book_errors;
    d.s.ouch_reports = a.s.ouch_reports - b.s.ouch_reports;
    d.s.ouch_errors = a.s.ouch_errors - b.s.ouch_errors;
    d.s.orders_sent = a.s.orders_sent - b.s.orders_sent;
    d.s.risk_rejects = a.s.risk_rejects - b.s.risk_rejects;
    d.s.fills = a.s.fills - b.s.fills;
    d.enters = a.enters - b.enters;
    d.cancels = a.cancels - b.cancels;
    d.replaces = a.replaces - b.replaces;
    d.replies = a.replies - b.replies;
    d.dropped = a.dropped - b.dropped;
    return d;
}

template <class S>
class Rig {
public:
    using Eng = engine::Engine<S>;

    Rig(const Options& o, const Feed& feed, const typename S::Config& scfg)
        : feed_(feed),
          gw_(o.reports),
          replies_(o.reports ? static_cast<std::size_t>(o.symbols) * 4 + 64 : 1),
          reports_(o.reports) {
        engine::Config ec;
        ec.max_locates = o.symbols + 1;  // synthetic locates are 1..symbols
        // The synthetic generator keeps at most 128 orders per side per symbol live; the table
        // gets 4x that plus headroom. Levels: prices sit within a few ticks of fair value.
        ec.books.max_orders = static_cast<std::size_t>(o.symbols) * 2 * 128 * 4 + 4096;
        ec.books.max_levels_per_side = 64;
        // The order manager keeps every order of the session, so size it for the whole run.
        ec.oms.max_orders = std::size_t{1} << 20;
        eng_ = std::make_unique<Eng>(ec, gw_, S(scfg));
    }

    Eng& engine() noexcept { return *eng_; }

    Counters counters() const noexcept {
        Counters c;
        c.s = eng_->stats();
        c.enters = gw_.enters;
        c.cancels = gw_.cancels;
        c.replaces = gw_.replaces;
        c.replies = replies_.replies;
        c.dropped = gw_.dropped;
        return c;
    }

    // Untimed: apply messages [0, n).
    void apply_prefix(std::size_t n) noexcept {
        for (std::size_t i = 0; i < n; ++i) step_untimed(i);
    }

    void step_untimed(std::size_t i) noexcept {
        checksum_ += static_cast<std::uint64_t>(eng_->on_itch(feed_.msg(i), feed_.ts[i]));
        if (gw_.pending()) replies_.pump(gw_, *eng_, feed_.ts[i]);
    }

    // Whole measured region in two clock reads. Returns elapsed ticks.
    std::uint64_t throughput() noexcept {
        const std::size_t n = feed_.size();
        std::uint64_t sum = 0;
        const std::uint64_t t0 = Timer::ticks();
        for (std::size_t i = feed_.prefix; i < n; ++i) {
            sum += static_cast<std::uint64_t>(eng_->on_itch(feed_.msg(i), feed_.ts[i]));
            if (gw_.pending()) replies_.pump(gw_, *eng_, feed_.ts[i]);
        }
        const std::uint64_t t1 = Timer::ticks();
        keep(sum);
        checksum_ += sum;
        return t1 - t0;
    }

    // Each call timed on its own, back to back.
    void closed_loop(Series& service) noexcept {
        const std::size_t n = feed_.size();
        std::size_t k = 0;
        for (std::size_t i = feed_.prefix; i < n; ++i, ++k) {
            const std::span<const std::byte> m = feed_.msg(i);
            const Nanos ts = feed_.ts[i];
            const std::uint64_t t0 = Timer::ticks();
            const DecodeStatus st = eng_->on_itch(m, ts);
            const std::uint64_t t1 = Timer::ticks();
            keep(st);
            checksum_ += static_cast<std::uint64_t>(st);
            service.v[k] = t1 - t0;
            if (gw_.pending()) replies_.pump(gw_, *eng_, ts);
        }
    }

    // Message k is due at t0 + k * period. Returns the backlog (ticks the last message
    // started after its intended time).
    std::uint64_t open_loop(const Timer& timer, double period_ns, Series& service, Series& response,
                            std::uint64_t& late_starts, std::uint64_t& max_lag) noexcept {
        const std::size_t n = feed_.size();
        const std::uint64_t t0 = Timer::ticks() + timer.from_ns(200'000);  // lead-in before message 0
        std::size_t k = 0;
        std::uint64_t lag = 0;
        const std::uint64_t late_ticks = timer.from_ns(kLateNs);
        for (std::size_t i = feed_.prefix; i < n; ++i, ++k) {
            const std::span<const std::byte> m = feed_.msg(i);
            const Nanos ts = feed_.ts[i];
            const std::uint64_t intended = t0 + timer.from_ns(static_cast<double>(k) * period_ns);
            std::uint64_t start = Timer::ticks();
            while (start < intended) start = Timer::ticks();  // spin: sleeping would add jitter
            const DecodeStatus st = eng_->on_itch(m, ts);
            const std::uint64_t end = Timer::ticks();
            keep(st);
            checksum_ += static_cast<std::uint64_t>(st);
            service.v[k] = end - start;
            response.v[k] = end - intended;
            lag = start - intended;
            if (lag > late_ticks) ++late_starts;
            max_lag = std::max(max_lag, lag);
            if (gw_.pending()) replies_.pump(gw_, *eng_, ts);
        }
        return lag;
    }

    std::uint64_t checksum() const noexcept { return checksum_; }
    bool reports() const noexcept { return reports_; }

private:
    const Feed& feed_;
    BenchGateway gw_;
    ExchangeReplies<Eng> replies_;
    bool reports_;
    std::unique_ptr<Eng> eng_;
    std::uint64_t checksum_{0};
};

// ---- environment banner ------------------------------------------------------------------------

std::string cpu_model() {
#if defined(__APPLE__)
    char buf[256] = {};
    std::size_t len = sizeof buf;
    if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0 && buf[0] != '\0') return buf;
#elif defined(__linux__)
    if (std::FILE* f = std::fopen("/proc/cpuinfo", "r")) {
        char line[512];
        while (std::fgets(line, sizeof line, f) != nullptr) {
            if (std::strncmp(line, "model name", 10) == 0) {
                if (const char* c = std::strchr(line, ':')) {
                    std::string s(c + 1);
                    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
                    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
                    std::fclose(f);
                    return s;
                }
            }
        }
        std::fclose(f);
    }
#endif
    return "unknown";
}

std::string build_flags() {
    std::string s;
#ifdef __OPTIMIZE__
    s += "optimized";
#else
    s += "NOT optimized (no -O)";
#endif
#ifdef NDEBUG
    s += ", NDEBUG";
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
    s += ", ADDRESS SANITIZER";
#endif
#if __has_feature(undefined_behavior_sanitizer)
    s += ", UB SANITIZER";
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
    s += ", ADDRESS SANITIZER";
#endif
#if defined(__AVX2__)
    s += ", avx2";
#endif
#if defined(__ARM_NEON)
    s += ", neon";
#endif
    return s;
}

bool sanitized() {
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
    return true;
#endif
#endif
    return false;
}

// Returns a human-readable pinning status.
std::string pin_thread(const Options& o) {
    if (!o.cpu_set) return "no (scheduler decides)";
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (o.cpu >= CPU_SETSIZE) return "FAILED (cpu index out of range)";
    CPU_SET(static_cast<int>(o.cpu), &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0) {
        return std::string("FAILED (") + std::strerror(errno) + ")";
    }
    return "yes, cpu " + std::to_string(o.cpu) + " (running on " + std::to_string(sched_getcpu()) + ")";
#else
    return "no (--cpu is unsupported on this OS and was ignored)";
#endif
}

struct FloorStats {
    double min_ns{}, p50_ns{}, p99_ns{}, max_ns{}, step_ns{};
};

// Back-to-back timer read pairs: what an empty timed region costs.
FloorStats measure_floor(const Timer& timer) {
    constexpr std::size_t kN = 200'000;
    std::vector<std::uint64_t> d(kN);
    std::uint64_t smallest_step = UINT64_MAX;
    std::uint64_t prev = Timer::ticks();
    for (std::size_t i = 0; i < kN; ++i) {
        const std::uint64_t a = Timer::ticks();
        const std::uint64_t b = Timer::ticks();
        d[i] = b - a;
        if (a > prev && a - prev < smallest_step) smallest_step = a - prev;
        prev = b;
    }
    std::sort(d.begin(), d.end());
    FloorStats f;
    f.min_ns = timer.to_ns(d.front());
    f.p50_ns = timer.to_ns(d[kN / 2]);
    f.p99_ns = timer.to_ns(d[kN * 99 / 100]);
    f.max_ns = timer.to_ns(d.back());
    f.step_ns = smallest_step == UINT64_MAX ? 0.0 : timer.to_ns(smallest_step);
    return f;
}

std::string cpu_os() {
    utsname u{};
    if (uname(&u) != 0) return "unknown";
    return std::string(u.sysname) + " " + u.release + " " + u.machine;
}

// ---- reporting ---------------------------------------------------------------------------------

struct Pcts {
    double mean, p50, p90, p99, p999, p9999, max;
};

Pcts pcts(const Timer& t, const Series& s) {
    return {t.to_ns(1) * s.mean_ticks, t.to_ns(s.percentile(5000)), t.to_ns(s.percentile(9000)),
            t.to_ns(s.percentile(9900)), t.to_ns(s.percentile(9990)), t.to_ns(s.percentile(9999)),
            t.to_ns(s.max())};
}

void print_table_header() {
    std::printf("  %-22s %10s %9s %9s %9s %9s %9s %10s %11s\n", "series (ns)", "n", "mean", "p50", "p90", "p99",
                "p99.9", "p99.99", "max");
}

void print_row(const Timer& t, const char* label, const Series& s) {
    const Pcts p = pcts(t, s);
    std::printf("  %-22s %10zu %9.1f %9.1f %9.1f %9.1f %9.1f %10.1f %11.1f\n", label, s.v.size(), p.mean, p.p50,
                p.p90, p.p99, p.p999, p.p9999, p.max);
}

void write_csv_row(std::FILE* f, const Timer& t, const char* strategy, const char* pass, const char* metric,
                   const Series& s) {
    const Pcts p = pcts(t, s);
    std::fprintf(f, "%s,%s,%s,%zu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,\n", strategy, pass, metric, s.v.size(), p.mean,
                 p.p50, p.p90, p.p99, p.p999, p.p9999, p.max);
}

void print_counters(const char* title, const Counters& c) {
    std::printf("  %-26s orders sent %llu (enter %llu, cancel %llu, replace %llu), risk rejects %llu, fills %llu,\n"
                "  %-26s book updates %llu, book errors %llu, exchange replies %llu\n",
                title, static_cast<unsigned long long>(c.s.orders_sent), static_cast<unsigned long long>(c.enters),
                static_cast<unsigned long long>(c.cancels), static_cast<unsigned long long>(c.replaces),
                static_cast<unsigned long long>(c.s.risk_rejects), static_cast<unsigned long long>(c.s.fills), "",
                static_cast<unsigned long long>(c.s.book_updates), static_cast<unsigned long long>(c.s.book_errors),
                static_cast<unsigned long long>(c.replies));
}

// ---- driver ------------------------------------------------------------------------------------

template <class S>
int run_strategy(const Options& o, const Feed& feed, const Timer& timer, std::FILE* csv) {
    typename S::Config scfg;
    scfg.max_locates = o.symbols + 1;
    const char* name = S::kName;
    const std::size_t n = feed.measured();

    std::printf("\n== strategy %s ==\n", name);

    // Pass 0: warm-up over the whole feed, discarded. Its final counters are the reference
    // that every later pass must reproduce.
    Counters reference;
    {
        Rig<S> rig(o, feed, scfg);
        rig.apply_prefix(feed.size());
        reference = rig.counters();
        keep(rig.checksum());
    }
    if (reference.s.book_errors != 0) {
        std::fprintf(stderr, "ot_bench: the books refused %llu messages; table sizes are too small for this feed\n",
                     static_cast<unsigned long long>(reference.s.book_errors));
        return 1;
    }
    bool consistent = true;

    // Pass 1: throughput.
    double mean_ns = 0, msgs_per_s = 0;
    Counters measured_delta;
    {
        Rig<S> rig(o, feed, scfg);
        rig.apply_prefix(feed.prefix);
        const Counters before = rig.counters();
        const std::uint64_t elapsed = rig.throughput();
        const Counters after = rig.counters();
        consistent = consistent && after == reference;
        measured_delta = diff(after, before);
        const double total_ns = timer.to_ns(elapsed);
        mean_ns = total_ns / static_cast<double>(n);
        msgs_per_s = 1e9 / mean_ns;
        keep(rig.checksum());
        std::printf("  throughput pass: %zu messages in %.3f ms -> %.1f ns/msg, %.3f M msgs/s\n", n, total_ns / 1e6,
                    mean_ns, msgs_per_s / 1e6);
    }

    // Pass 2: closed-loop latency.
    Series closed;
    closed.v.assign(n, 0);
    {
        Rig<S> rig(o, feed, scfg);
        rig.apply_prefix(feed.prefix);
        rig.closed_loop(closed);
        consistent = consistent && rig.counters() == reference;
        closed.finish();
    }

    // Pass 3: open-loop latency.
    Series service, response;
    std::uint64_t late_starts = 0, max_lag = 0, end_lag = 0;
    double period_ns = 0;
    if (o.rate > 0) {
        service.v.assign(n, 0);
        response.v.assign(n, 0);
        period_ns = 1e9 / static_cast<double>(o.rate);
        Rig<S> rig(o, feed, scfg);
        rig.apply_prefix(feed.prefix);
        end_lag = rig.open_loop(timer, period_ns, service, response, late_starts, max_lag);
        consistent = consistent && rig.counters() == reference;
        service.finish();
        response.finish();
    }

    std::printf("  measured region: %zu messages, all series in nanoseconds (timer read floor included)\n", n);
    print_table_header();
    print_row(timer, "closed-loop service", closed);
    if (o.rate > 0) {
        print_row(timer, "open-loop service", service);
        print_row(timer, "open-loop response", response);
        const double util = timer.to_ns(1) * service.mean_ticks / period_ns;
        std::printf("  open loop: offered %llu msgs/s (period %.1f ns), mean service / period = %.2f,\n"
                    "             starts more than 1 us late: %llu of %zu (%.1f%%), max start lag %.1f ns,\n"
                    "             backlog when the last message started: %.1f ns\n",
                    static_cast<unsigned long long>(o.rate), period_ns, util,
                    static_cast<unsigned long long>(late_starts), n,
                    100.0 * static_cast<double>(late_starts) / static_cast<double>(n), timer.to_ns(max_lag),
                    timer.to_ns(end_lag));
        if (util >= 1.0 || timer.to_ns(end_lag) > 10.0 * period_ns) {
            std::printf("  WARNING: offered rate is at or above capacity; response times grow with the backlog and\n"
                        "           describe overload, not steady-state latency. Lower --rate.\n");
        }
    }
    print_counters("orders in measured region:", measured_delta);
    if (reference.dropped != 0) std::printf("  WARNING: %llu orders overflowed the reply outbox\n", static_cast<unsigned long long>(reference.dropped));
    if (measured_delta.enters + measured_delta.cancels + measured_delta.replaces == 0) {
        std::printf("  note: the strategy sent no orders in the measured region; only book+strategy evaluation is timed\n");
    }
    std::printf("  counters identical across all passes: %s\n", consistent ? "yes" : "NO (BUG)");

    if (csv != nullptr) {
        write_csv_row(csv, timer, name, "closed", "service", closed);
        if (o.rate > 0) {
            write_csv_row(csv, timer, name, "open", "service", service);
            write_csv_row(csv, timer, name, "open", "response", response);
        }
        std::fprintf(csv, "%s,throughput,ns_per_msg,%zu,%.3f,,,,,,,%.1f\n", name, n, mean_ns, msgs_per_s);
    }
    return consistent ? 0 : 1;
}

int run(int argc, char** argv) {
    Options o;
    if (const int rc = parse_args(argc, argv, o); rc >= 0) return rc;

    Timer timer;
    const std::string pinned = pin_thread(o);
    timer.calibrate();
    const FloorStats floor = measure_floor(timer);

    Feed feed;
    if (!build_feed(o, feed)) {
        std::fputs("ot_bench: the synthetic feed ended early or exceeds 4 GiB; lower --messages/--warmup\n", stderr);
        return 1;
    }

    std::FILE* csv = nullptr;
    if (o.csv != nullptr) {
        csv = std::fopen(o.csv, "w");
        if (csv == nullptr) {
            std::fprintf(stderr, "ot_bench: cannot create '%s': %s\n", o.csv, std::strerror(errno));
            return 1;
        }
        std::fputs("strategy,pass,metric,count,mean_ns,p50_ns,p90_ns,p99_ns,p999_ns,p9999_ns,max_ns,msgs_per_s\n",
                   csv);
    }

    std::printf("ot_bench: tick-to-decision latency (Engine::on_itch)\n");
    std::printf("  cpu        %s\n", cpu_model().c_str());
    std::printf("  os         %s\n", cpu_os().c_str());
#if defined(__VERSION__)
    std::printf("  compiler   %s\n", __VERSION__);
#endif
    std::printf("  build      %s (flags visible to the compiler only; the build system's -O level is not recorded)\n",
                build_flags().c_str());
    if (sanitized()) {
        std::printf("  WARNING    sanitizers are enabled: latencies are not representative\n");
    }
#ifndef __OPTIMIZE__
    std::printf("  WARNING    built without optimisation: latencies are not representative\n");
#endif
    std::printf("  timer      %s", timer.description());
#ifdef OT_BENCH_RDTSC
    std::printf(", %.4f GHz", timer.ticks_per_ns());
#endif
    std::printf("\n  timer read floor  min %.1f, p50 %.1f, p99 %.1f, max %.1f ns per empty timed region;"
                " smallest step %.1f ns\n",
                floor.min_ns, floor.p50_ns, floor.p99_ns, floor.max_ns, floor.step_ns);
    if (floor.step_ns > 10.0) {
        std::printf("  NOTE       timer step is %.1f ns: every per-call sample is a multiple of it. Means are unbiased,\n"
                    "             but percentiles below a few steps only tell you which step the call landed in.\n",
                    floor.step_ns);
    }
    std::printf("  pinned     %s\n", pinned.c_str());
    std::printf("  feed       seed %llu, %llu symbols, %zu messages total (preamble %zu, warmup %zu, measured %zu),"
                " %zu bytes\n",
                static_cast<unsigned long long>(o.seed), static_cast<unsigned long long>(o.symbols), feed.size(),
                feed.preamble, feed.prefix - feed.preamble, feed.measured(), feed.bytes.size());
    std::printf("  feed mix   (measured region)");
    for (int c = 0; c < 256; ++c) {
        if (feed.type_count[c] != 0) {
            std::printf(" %c=%llu", c, static_cast<unsigned long long>(feed.type_count[c]));
        }
    }
    std::printf("\n  gateway    accepts and discards; exchange replies %s\n",
                o.reports ? "ON (answered after each timed call, outside the timed region)" : "OFF (--no-reports)");
    std::printf("  mode       %s\n",
                o.rate == 0 ? "closed loop" : "closed loop + open loop (see --rate)");

    int rc = 0;
    if (o.which == Which::imbalance || o.which == Which::all)
        rc |= run_strategy<strategy::ImbalanceTaker>(o, feed, timer, csv);
    if (o.which == Which::maker || o.which == Which::all)
        rc |= run_strategy<strategy::MicropriceMaker>(o, feed, timer, csv);
    if (o.which == Which::ema || o.which == Which::all)
        rc |= run_strategy<strategy::EmaCross>(o, feed, timer, csv);

    if (csv != nullptr && std::fclose(csv) != 0) {
        std::fprintf(stderr, "ot_bench: writing '%s' failed\n", o.csv);
        return 1;
    }
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::bad_alloc&) {
        std::fputs("ot_bench: out of memory; lower --messages, --warmup or --symbols\n", stderr);
        return 1;
    }
}

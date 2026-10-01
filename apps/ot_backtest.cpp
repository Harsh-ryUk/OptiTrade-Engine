// ot_backtest: replays ITCH data through the trading engine and the exchange simulator.
//
// Input is a capture written by ot_gen (--capture), a Nasdaq ITCH 5.0 BinaryFILE (--itch) or a
// synthetic market generated on the fly (--synthetic). One strategy trades it against the
// simulated exchange and the run ends with a PnL report and a 64-bit digest of every order sent
// and every report received: two runs with the same digest made identical decisions.
//
// Exit status: 0 success, 1 unreadable or corrupt input, 2 usage error.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>

#include "optitrade/core/decode_status.hpp"
#include "optitrade/replay/backtest.hpp"
#include "optitrade/replay/capture.hpp"
#include "optitrade/sim/synthetic_market.hpp"
#include "optitrade/strategy/ema_cross.hpp"
#include "optitrade/strategy/imbalance_taker.hpp"
#include "optitrade/strategy/microprice_maker.hpp"

namespace {

using namespace optitrade;

constexpr int kExitInput = 1;
constexpr int kExitUsage = 2;

constexpr std::uint64_t kMaxLatencyUs = 1'000'000'000;  // 1000 s; keeps microseconds * 1000 far from overflow

void usage(std::FILE* out) {
    std::fputs(
        "usage: ot_backtest (--capture FILE | --itch FILE | --synthetic SEED[:SYMBOLS:MESSAGES])\n"
        "                   [--strategy imbalance|maker|ema] [--latency-us N] [--json]\n"
        "\n"
        "Replays market data through the trading engine against a simulated exchange and\n"
        "prints a PnL report and a digest of every order sent and every report received.\n"
        "\n"
        "input (exactly one)\n"
        "  --capture FILE   OTCAP001 capture file (see ot_gen)\n"
        "  --itch FILE      Nasdaq ITCH 5.0 BinaryFILE\n"
        "  --synthetic S    generate a market on the fly: SEED, SEED:SYMBOLS or\n"
        "                   SEED:SYMBOLS:MESSAGES (defaults 8 symbols, 100000 messages)\n"
        "\n"
        "options\n"
        "  --strategy NAME  imbalance (default), maker or ema\n"
        "  --latency-us N   one-way wire latency in microseconds, applied to orders going out\n"
        "                   and to reports coming back (default 50)\n"
        "  --book-orders N  live-order capacity of the order books for --capture/--itch\n"
        "                   (default 2097152)\n"
        "  --session-orders N  order-table slots: orders that may be working at the same moment\n"
        "                   (default 262144). Finished orders give their slot back, so this is\n"
        "                   not a limit on the run; a full table is reported as capacity rejects\n"
        "  --json           print one JSON object instead of the table (PnL in units of 1e-4)\n"
        "  --help           show this text\n",
        out);
}

// Whole-string unsigned decimal in [0, max]. Rejects signs, blanks, trailing junk and overflow,
// all of which strtoull would otherwise accept silently.
bool parse_u64(const char* text, std::uint64_t max, std::uint64_t& out) {
    if (text[0] < '0' || text[0] > '9') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || v > max) return false;
    out = v;
    return true;
}

// "SEED[:SYMBOLS[:MESSAGES]]"
bool parse_synthetic(const char* text, sim::SyntheticConfig& cfg) {
    std::uint64_t fields[3] = {cfg.seed, cfg.symbols, cfg.messages};
    const std::uint64_t limits[3] = {UINT64_MAX, 65'535, UINT64_MAX};
    std::string s(text);
    std::size_t begin = 0;
    for (int i = 0; i < 3; ++i) {
        const std::size_t colon = s.find(':', begin);
        const std::string part = s.substr(begin, colon == std::string::npos ? std::string::npos : colon - begin);
        if (!parse_u64(part.c_str(), limits[i], fields[i])) return false;
        if (colon == std::string::npos) {
            begin = std::string::npos;
            break;
        }
        begin = colon + 1;
    }
    if (begin != std::string::npos || fields[1] == 0) return false;  // more than three fields, or no symbols
    cfg.seed = fields[0];
    cfg.symbols = static_cast<std::uint16_t>(fields[1]);
    cfg.messages = fields[2];
    return true;
}

// The generator as a record source, so a long synthetic run needs no memory for the feed.
class SyntheticSource {
public:
    explicit SyntheticSource(const sim::SyntheticConfig& cfg) : market_(cfg) {}
    bool next(replay::Record& rec) noexcept {
        std::size_t len = 0;
        if (!market_.next(buf_, len, rec.ts)) return false;
        rec.payload = std::span<const std::byte>(buf_.data(), len);
        return true;
    }

private:
    sim::SyntheticMarket market_;
    std::array<std::byte, 128> buf_{};
};

struct Options {
    const char* capture{nullptr};
    const char* itch{nullptr};
    bool synthetic{false};
    sim::SyntheticConfig synth{};
    const char* strategy{"imbalance"};
    std::uint64_t latency_us{50};
    std::uint64_t book_orders{1u << 21};
    std::uint64_t session_orders{1u << 18};
    bool json{false};
};

// Sizes every table up front. Synthetic runs know their instrument count; real files use the
// range of Nasdaq locates.
replay::BacktestConfig make_config(const Options& o) {
    replay::BacktestConfig c;
    c.sim.order_latency_ns = o.latency_us * 1000;
    c.sim.report_latency_ns = o.latency_us * 1000;
    c.engine.oms.max_orders = static_cast<std::size_t>(o.session_orders);
    if (o.synthetic) {
        const std::size_t symbols = o.synth.symbols;
        c.sim.books.max_orders = std::max<std::size_t>(4096, symbols * 512);  // model keeps <= 256 per symbol
        c.sim.books.max_symbols = symbols + 16;
        c.engine.max_locates = symbols + 1;  // locate = index + 1
    } else {
        c.sim.books.max_orders = static_cast<std::size_t>(o.book_orders);
        c.sim.books.max_symbols = std::size_t{1} << 14;
        c.engine.max_locates = std::size_t{1} << 14;
    }
    c.sim.books.max_levels_per_side = 64;  // the strategies look at the top 5; deeper levels are dropped
    c.engine.books = c.sim.books;
    return c;
}

void print_json(const replay::BacktestReport& r, const char* strategy, const Options& o) {
    const auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    const auto i = [](std::int64_t v) { return static_cast<long long>(v); };
    std::printf(
        "{\"strategy\":\"%s\",\"latency_us\":%llu,\"messages\":%llu,\"book_updates\":%llu,"
        "\"orders_sent\":%llu,\"orders_rejected_risk\":%llu,\"fills\":%llu,\"volume\":%llu,"
        "\"realized_pnl\":%lld,\"unrealized_pnl\":%lld,\"total_pnl\":%lld,\"max_drawdown\":%lld,"
        "\"pnl_unit\":\"1e-4\",\"dropped_reports\":%llu,\"capacity_rejects\":%llu,"
        "\"book_errors\":%llu,\"itch_skipped\":%llu,\"digest\":\"%016llx\"}\n",
        strategy, u(o.latency_us), u(r.messages), u(r.book_updates), u(r.orders_sent),
        u(r.orders_rejected_risk), u(r.fills), u(r.volume), i(r.realized_pnl), i(r.unrealized_pnl),
        i(r.total_pnl), i(r.max_drawdown), u(r.dropped_reports), u(r.capacity_rejects), u(r.book_errors),
        u(r.itch_skipped), u(r.digest));
}

// Runs one strategy over one source and prints the outcome. Returns the process exit code.
template <class S, class Source>
int run(Source& src, const Options& o, const char* title) {
    const replay::BacktestConfig cfg = make_config(o);
    typename S::Config scfg;
    scfg.max_locates = cfg.engine.max_locates;
    const replay::BacktestReport r = replay::run_backtest(src, cfg, S(scfg));

    if (o.json) {
        print_json(r, S::kName, o);
    } else {
        replay::print_report(r, title, stdout);
    }
    // Undersized tables degrade a run without failing it; say so on stderr, where --json
    // output on stdout stays clean.
    const auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    if (r.capacity_rejects != 0) {
        std::fprintf(stderr,
                     "ot_backtest: warning: %llu strategy requests were refused because the order table or the "
                     "simulator was full; the strategy did not get to trade freely (raise --session-orders)\n",
                     u(r.capacity_rejects));
    }
    if (r.book_errors != 0) {
        std::fprintf(stderr,
                     "ot_backtest: warning: the order books refused %llu messages; they are probably too small for "
                     "this feed (raise --book-orders); results are unreliable\n",
                     u(r.book_errors));
    }
    if (r.itch_skipped != 0) {
        std::fprintf(stderr, "ot_backtest: warning: %llu ITCH messages could not be decoded and were skipped\n",
                     u(r.itch_skipped));
    }
    if (r.dropped_reports != 0) {
        std::fprintf(stderr, "ot_backtest: warning: %llu simulator reports were dropped; results are unreliable\n",
                     u(r.dropped_reports));
    }
    return 0;
}

template <class Source>
int dispatch(Source& src, const Options& o, const char* title) {
    if (std::strcmp(o.strategy, "imbalance") == 0) return run<strategy::ImbalanceTaker>(src, o, title);
    if (std::strcmp(o.strategy, "maker") == 0) return run<strategy::MicropriceMaker>(src, o, title);
    return run<strategy::EmaCross>(src, o, title);  // "ema"; the name was validated while parsing
}

// Reads a file-backed source to the end. A file that ends mid-record is an input error even
// though the records before it were replayed, so the caller must not treat the report as complete.
template <class Reader>
int run_file(const char* what, const char* path, const Options& o) {
    Reader reader(path);
    if (!reader.ok()) {
        std::fprintf(stderr, "ot_backtest: cannot read %s '%s'%s\n", what, path,
                     reader.error() == DecodeStatus::ok ? "" : " (missing, empty or not a valid file)");
        return kExitInput;
    }
    const std::string title = std::string(o.strategy) + " on " + path;
    const int rc = dispatch(reader, o, title.c_str());
    if (rc == 0 && reader.error() != DecodeStatus::ok) {
        std::fprintf(stderr, "ot_backtest: '%s' is corrupt after %llu records (%s); the report above covers only those\n",
                     path, static_cast<unsigned long long>(reader.records_read()), to_string(reader.error()));
        return kExitInput;
    }
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    int sources = 0;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            usage(stdout);
            return 0;
        }
        if (std::strcmp(arg, "--json") == 0) {
            o.json = true;
            continue;
        }
        const bool is_capture = std::strcmp(arg, "--capture") == 0;
        const bool is_itch = std::strcmp(arg, "--itch") == 0;
        const bool is_synth = std::strcmp(arg, "--synthetic") == 0;
        const bool is_strategy = std::strcmp(arg, "--strategy") == 0;
        const bool is_latency = std::strcmp(arg, "--latency-us") == 0;
        const bool is_book_orders = std::strcmp(arg, "--book-orders") == 0;
        const bool is_session_orders = std::strcmp(arg, "--session-orders") == 0;
        if (!is_capture && !is_itch && !is_synth && !is_strategy && !is_latency && !is_book_orders &&
            !is_session_orders) {
            std::fprintf(stderr, "ot_backtest: unknown argument '%s' (try --help)\n", arg);
            return kExitUsage;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "ot_backtest: %s needs a value\n", arg);
            return kExitUsage;
        }
        const char* value = argv[++i];
        std::uint64_t n = 0;
        bool good = true;
        if (is_capture) {
            o.capture = value;
            ++sources;
        } else if (is_itch) {
            o.itch = value;
            ++sources;
        } else if (is_synth) {
            good = parse_synthetic(value, o.synth);
            o.synthetic = true;
            ++sources;
        } else if (is_strategy) {
            good = std::strcmp(value, "imbalance") == 0 || std::strcmp(value, "maker") == 0 ||
                   std::strcmp(value, "ema") == 0;
            o.strategy = value;
        } else if (is_latency) {
            good = parse_u64(value, kMaxLatencyUs, n);
            o.latency_us = n;
        } else if (is_book_orders) {
            good = parse_u64(value, std::uint64_t{1} << 28, n) && n >= 1;
            o.book_orders = n;
        } else {
            good = parse_u64(value, std::uint64_t{1} << 28, n) && n >= 1;
            o.session_orders = n;
        }
        if (!good) {
            std::fprintf(stderr, "ot_backtest: invalid value '%s' for %s\n", value, arg);
            return kExitUsage;
        }
    }
    if (sources != 1) {
        std::fputs("ot_backtest: give exactly one of --capture, --itch, --synthetic (try --help)\n", stderr);
        return kExitUsage;
    }

    if (o.capture != nullptr) return run_file<replay::CaptureReader>("capture", o.capture, o);
    if (o.itch != nullptr) return run_file<replay::ItchFileReader>("ITCH file", o.itch, o);

    SyntheticSource src(o.synth);
    const std::string title = std::string(o.strategy) + " on synthetic seed " + std::to_string(o.synth.seed);
    return dispatch(src, o, title.c_str());
}

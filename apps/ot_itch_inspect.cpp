// ot_itch_inspect: summarises a Nasdaq ITCH 5.0 BinaryFILE ([u16 BE length][message]...).
//
// The file is streamed through a fixed buffer, so a multi-gigabyte day file costs
// no more memory than the order table and books it drives. Every frame is counted
// in the type histogram; frames of a type the library models are also decoded and
// applied to MarketBooks, which yields the final book state and any anomalies.
//
// Anomaly vocabulary:
//   decode failures     a modelled type whose frame is the wrong size or has a
//                       forbidden field value (truncated / bad_length / bad_field)
//   apply results       what MarketBooks answered (unknown_order, duplicate_order,
//                       capacity, invalid). "ignored" is not an anomaly: it is the
//                       normal answer to Trade and System Event messages.
// Unmodelled types (trading actions, NOII, ...) are valid ITCH and are only counted.
//
// "Most active" ranks instruments by the number of order-flow messages (A F E C X
// D U) that MarketBooks applied, attributed to the locate in the message header.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <numeric>
#include <string_view>
#include <type_traits>
#include <vector>

#include "optitrade/book/market_books.hpp"
#include "optitrade/itch/decoder.hpp"
#include "optitrade/replay/capture.hpp"

namespace {

using namespace optitrade;

constexpr int kExitUsage = 2;

void usage(std::FILE* out) {
    std::fputs(
        "usage: ot_itch_inspect [options] FILE\n"
        "\n"
        "Streams a Nasdaq ITCH 5.0 BinaryFILE and prints a message-type histogram, the\n"
        "time span, the stock directory size, the final book state and anomaly counts.\n"
        "\n"
        "  --top N          instruments to show top-of-book for (default 5)\n"
        "  --max-orders N   live-order table capacity (default 4194304); orders beyond\n"
        "                   it are reported as capacity anomalies\n"
        "  --levels N       price levels kept per side per book (default 256)\n"
        "  --help           show this text\n"
        "\n"
        "Exit status: 0 clean, 1 unreadable or truncated file, 2 usage error.\n",
        out);
}

bool parse_size(const char* text, std::size_t max, std::size_t& out) {
    if (text[0] < '0' || text[0] > '9') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || v > max) return false;
    out = static_cast<std::size_t>(v);
    return true;
}

constexpr std::size_t kApplied = static_cast<std::size_t>(book::Applied::invalid) + 1;
constexpr std::size_t kStatuses = static_cast<std::size_t>(DecodeStatus::bad_field) + 1;

const char* applied_name(std::size_t i) {
    static constexpr std::array<const char*, kApplied> names = {
        "ok", "ignored", "unknown_order", "duplicate_order", "unknown_symbol", "capacity", "invalid"};
    return names[i];
}

// Applies each decoded message to the books and tallies the outcome.
struct Sink {
    book::MarketBooks& books;
    std::array<std::uint64_t, kApplied> applied{};
    std::uint64_t directory_entries{0};
    std::vector<std::uint64_t> activity = std::vector<std::uint64_t>(65536, 0);

    template <class M>
    void on(const M& m) noexcept {
        const book::Applied r = books.on(m);
        ++applied[static_cast<std::size_t>(r)];
        if constexpr (std::is_same_v<M, itch::StockDirectory>) {
            if (r == book::Applied::ok) ++directory_entries;
        } else if constexpr (std::is_same_v<M, itch::AddOrder> || std::is_same_v<M, itch::OrderExecuted> ||
                             std::is_same_v<M, itch::OrderExecutedPrice> || std::is_same_v<M, itch::OrderCancel> ||
                             std::is_same_v<M, itch::OrderDelete> || std::is_same_v<M, itch::OrderReplace>) {
            if (r == book::Applied::ok) ++activity[m.h.locate];
        }
    }
};

void print_clock(Nanos ns) {
    const unsigned long long s = ns / 1'000'000'000ULL;
    std::printf("%02llu:%02llu:%02llu.%09llu", s / 3600, (s / 60) % 60, s % 60, ns % 1'000'000'000ULL);
}

void print_side(const book::OrderBook& bk, Side side) {
    const auto best = bk.best(side);
    if (!best) {
        std::printf("%-28s", "-");
        return;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%u @ %lld.%04lld (%u ord)", static_cast<unsigned>(best->qty),
                  static_cast<long long>(best->price / kPriceScale), static_cast<long long>(best->price % kPriceScale),
                  static_cast<unsigned>(best->orders));
    std::printf("%-28s", buf);
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = nullptr;
    std::size_t top_n = 5;
    book::MarketBooks::Config cfg;
    cfg.max_orders = std::size_t{1} << 22;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            usage(stdout);
            return 0;
        }
        std::size_t* target = nullptr;
        std::size_t max = 0;
        if (std::strcmp(arg, "--top") == 0) {
            target = &top_n;
            max = 65536;
        } else if (std::strcmp(arg, "--max-orders") == 0) {
            target = &cfg.max_orders;
            max = std::size_t{1} << 32;
        } else if (std::strcmp(arg, "--levels") == 0) {
            target = &cfg.max_levels_per_side;
            max = 1'000'000;
        } else if (arg[0] == '-' && arg[1] != '\0') {
            std::fprintf(stderr, "ot_itch_inspect: unknown option '%s' (try --help)\n", arg);
            return kExitUsage;
        } else if (path == nullptr) {
            path = arg;
            continue;
        } else {
            std::fputs("ot_itch_inspect: more than one input file given\n", stderr);
            return kExitUsage;
        }
        if (i + 1 >= argc || !parse_size(argv[i + 1], max, *target) || (target != &top_n && *target == 0)) {
            std::fprintf(stderr, "ot_itch_inspect: %s needs a valid number\n", arg);
            return kExitUsage;
        }
        ++i;
    }
    if (path == nullptr) {
        std::fputs("ot_itch_inspect: no input file (try --help)\n", stderr);
        return kExitUsage;
    }

    replay::ItchFileReader reader(path);
    if (!reader.ok()) {
        std::fprintf(stderr, "ot_itch_inspect: cannot open '%s': %s\n", path, std::strerror(errno));
        return 1;
    }

    try {
        book::MarketBooks books(cfg);
        Sink sink{books};
        std::array<std::uint64_t, 256> histogram{};
        std::array<std::uint64_t, kStatuses> decode_status{};
        std::uint64_t unmodelled = 0;
        std::uint64_t frames = 0;
        Nanos first_ts = 0, last_ts = 0, min_ts = 0, max_ts = 0;

        replay::Record rec;
        while (reader.next(rec)) {
            if (frames == 0) first_ts = min_ts = max_ts = rec.ts;
            last_ts = rec.ts;
            min_ts = std::min(min_ts, rec.ts);
            max_ts = std::max(max_ts, rec.ts);
            ++frames;
            ++histogram[static_cast<unsigned char>(rec.payload[0])];
            const DecodeStatus st = itch::decode(rec.payload, sink);
            if (st == DecodeStatus::unknown_type) {
                ++unmodelled;
            } else {
                ++decode_status[static_cast<std::size_t>(st)];
            }
        }

        std::printf("file      %s\n", path);
        std::printf("messages  %llu\n", static_cast<unsigned long long>(frames));

        std::printf("\nmessage types\n");
        std::array<unsigned, 256> order{};
        std::iota(order.begin(), order.end(), 0u);
        std::stable_sort(order.begin(), order.end(), [&](unsigned a, unsigned b) { return histogram[a] > histogram[b]; });
        for (const unsigned t : order) {
            if (histogram[t] == 0) break;
            const char c = static_cast<char>(t);
            const bool printable = t >= 0x20 && t < 0x7F;
            char label[8];
            if (printable) {
                std::snprintf(label, sizeof label, "'%c'%s", c, itch::message_length(c) != 0 ? "" : "*");
            } else {
                std::snprintf(label, sizeof label, "0x%02X*", t);
            }
            std::printf("  %-6s %14llu  %6.2f%%\n", label, static_cast<unsigned long long>(histogram[t]),
                        100.0 * static_cast<double>(histogram[t]) / static_cast<double>(frames));
        }
        if (unmodelled != 0) std::puts("  (* = type not modelled by the decoder; counted, not applied)");

        std::printf("\ntime span\n");
        if (frames == 0) {
            std::puts("  (no messages)");
        } else {
            std::printf("  first   ");
            print_clock(first_ts);
            std::printf("\n  last    ");
            print_clock(last_ts);
            std::printf("\n  span    %.3f s (min to max timestamp)%s\n",
                        static_cast<double>(max_ts - min_ts) / 1e9, last_ts < first_ts ? "  [last precedes first]" : "");
        }

        std::size_t books_n = 0;
        for (std::size_t l = 0; l < 65536; ++l) books_n += books.book(static_cast<Locate>(l)) != nullptr;
        std::printf("\nstate at end of file\n");
        std::printf("  stock directory entries  %llu\n", static_cast<unsigned long long>(sink.directory_entries));
        std::printf("  books                    %zu\n", books_n);
        std::printf("  live orders              %zu\n", books.live_orders());
        std::printf("  depth overflows          %llu\n", static_cast<unsigned long long>(books.overflows()));

        std::vector<std::uint32_t> ranked;
        for (std::uint32_t l = 0; l < 65536; ++l) {
            if (sink.activity[l] != 0) ranked.push_back(l);
        }
        std::stable_sort(ranked.begin(), ranked.end(),
                         [&](std::uint32_t a, std::uint32_t b) { return sink.activity[a] > sink.activity[b]; });
        if (ranked.size() > top_n) ranked.resize(top_n);
        std::printf("\nmost active instruments (top of book)\n");
        if (ranked.empty()) std::puts("  (none)");
        else std::printf("  %-8s %-6s %12s  %-28s%-28s\n", "symbol", "locate", "messages", "best bid", "best ask");
        for (const std::uint32_t l : ranked) {
            const auto loc = static_cast<Locate>(l);
            const Symbol sym = books.symbol(loc);
            const std::string_view name = sym.view();
            std::printf("  %-8.*s %-6u %12llu  ", static_cast<int>(name.size()), name.data(), l,
                        static_cast<unsigned long long>(sink.activity[l]));
            print_side(*books.book(loc), Side::buy);
            print_side(*books.book(loc), Side::sell);
            std::printf("\n");
        }

        std::uint64_t anomalies = 0;
        std::printf("\nanomalies\n");
        for (std::size_t s = 1; s < kStatuses; ++s) {
            if (static_cast<DecodeStatus>(s) == DecodeStatus::unknown_type) continue;  // valid but unmodelled: see histogram
            std::printf("  decode %-16s %llu\n", to_string(static_cast<DecodeStatus>(s)),
                        static_cast<unsigned long long>(decode_status[s]));
            anomalies += decode_status[s];
        }
        for (std::size_t a = 0; a < kApplied; ++a) {
            const auto r = static_cast<book::Applied>(a);
            if (r == book::Applied::ok || r == book::Applied::ignored) continue;
            std::printf("  apply  %-16s %llu\n", applied_name(a), static_cast<unsigned long long>(sink.applied[a]));
            anomalies += sink.applied[a];
        }
        std::printf("  total                   %llu\n", static_cast<unsigned long long>(anomalies));

        if (reader.error() != DecodeStatus::ok) {
            std::fprintf(stderr, "ot_itch_inspect: '%s' ends inside a frame (%s) after %llu messages\n", path,
                         to_string(reader.error()), static_cast<unsigned long long>(reader.records_read()));
            return 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ot_itch_inspect: %s (try smaller --max-orders / --levels)\n", e.what());
        return 1;
    }
    return 0;
}

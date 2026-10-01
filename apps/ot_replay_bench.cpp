// Replay throughput of the feed path on a Nasdaq BinaryFILE: memory-maps the file and times
//   frames : walking the 2-byte length prefixes only (the floor: memory bandwidth)
//   decode : frames + itch::decode into PODs (no book)
//   books  : frames + decode + MarketBooks::on() (the full market data path)
// Everything is single threaded. Run it twice: the first pass warms the page cache.

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "optitrade/book/market_books.hpp"
#include "optitrade/core/endian.hpp"
#include "optitrade/itch/decoder.hpp"

namespace {

using namespace optitrade;
using Clock = std::chrono::steady_clock;

struct Sink : itch::NullHandler {
    using itch::NullHandler::on;
    std::uint64_t seen{0};
    template <class M> void on(const M&) noexcept { ++seen; }
};

struct Apply : itch::NullHandler {
    using itch::NullHandler::on;
    book::MarketBooks* books;
    std::uint64_t ok{0}, refused{0};
    explicit Apply(book::MarketBooks* b) : books(b) {}
    void tally(book::Applied r) noexcept {
        if (r == book::Applied::ok || r == book::Applied::ignored) ++ok; else ++refused;
    }
    void on(const itch::StockDirectory& m) noexcept { tally(books->on(m)); }
    void on(const itch::AddOrder& m) noexcept { tally(books->on(m)); }
    void on(const itch::OrderExecuted& m) noexcept { tally(books->on(m)); }
    void on(const itch::OrderExecutedPrice& m) noexcept { tally(books->on(m)); }
    void on(const itch::OrderCancel& m) noexcept { tally(books->on(m)); }
    void on(const itch::OrderDelete& m) noexcept { tally(books->on(m)); }
    void on(const itch::OrderReplace& m) noexcept { tally(books->on(m)); }
};

enum class Mode { frames, decode, books };

struct Result { std::uint64_t messages{0}; double seconds{0}; std::uint64_t extra{0}; std::uint64_t refused{0}; };

Result run(Mode mode, const std::byte* data, std::size_t size, std::size_t max_orders, std::size_t levels,
           std::size_t ahead) {
    Result r;
    Sink sink;
    book::MarketBooks::Config cfg;
    cfg.max_orders = max_orders;
    cfg.max_levels_per_side = levels;
    book::MarketBooks books(cfg);
    Apply apply(&books);

    const auto t0 = Clock::now();
    std::size_t pos = 0;
    std::uint64_t frames = 0;
    // Look-ahead cursor for cache hints: `ahead` frames in front of the one being applied.
    std::size_t apos = 0;
    std::uint64_t prefetched = 0;
    while (pos + 2 <= size) {
        while (ahead != 0 && mode == Mode::books && prefetched < frames + ahead && apos + 2 <= size) {
            const std::size_t alen = be::load16(data + apos);
            if (apos + 2 + alen > size) break;
            books.prefetch(std::span<const std::byte>(data + apos + 2, alen));
            apos += 2 + alen;
            ++prefetched;
        }
        const std::size_t len = be::load16(data + pos);
        if (pos + 2 + len > size) break;
        const std::span<const std::byte> msg(data + pos + 2, len);
        pos += 2 + len;
        ++frames;
        if (mode == Mode::decode) (void)itch::decode(msg, sink);
        else if (mode == Mode::books) (void)itch::decode(msg, apply);
    }
    r.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
    r.messages = frames;
    r.extra = mode == Mode::decode ? sink.seen : apply.ok;
    r.refused = apply.refused;
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    std::size_t max_orders = std::size_t{1} << 23, levels = 1024, repeat = 1;
    std::vector<std::size_t> aheads{0, 4, 8, 16, 32};
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--max-orders" && i + 1 < argc) max_orders = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--levels" && i + 1 < argc) levels = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--repeat" && i + 1 < argc) repeat = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--ahead" && i + 1 < argc) {
            aheads.clear();
            for (const char* q = argv[++i]; *q != '\0';) {
                char* end = nullptr;
                aheads.push_back(std::strtoull(q, &end, 10));
                q = (*end == ',') ? end + 1 : end;
                if (end == q - 0 && *end != '\0') break;
            }
        }
        else if (a[0] != '-') path = a;
        else {
            std::fprintf(stderr, "usage: %s [--max-orders N] [--levels N] [--repeat N] FILE\n", argv[0]);
            return 2;
        }
    }
    if (path.empty()) { std::fprintf(stderr, "ot_replay_bench: need a Nasdaq BinaryFILE\n"); return 2; }
    const int fd = ::open(path.c_str(), O_RDONLY);
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st) != 0) { std::perror("ot_replay_bench: open"); return 1; }
    const auto size = static_cast<std::size_t>(st.st_size);
    void* map = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { std::perror("ot_replay_bench: mmap"); return 1; }
    ::madvise(map, size, MADV_SEQUENTIAL);
    const auto* data = static_cast<const std::byte*>(map);

    std::printf("file %.2f GB, max_orders %zu, levels/side %zu\n", static_cast<double>(size) / 1e9, max_orders, levels);
    std::printf("%-12s %12s %10s %12s %10s\n", "mode", "messages", "seconds", "M msg/s", "ns/msg");
    for (std::size_t rep = 0; rep < repeat; ++rep) {
        for (const auto& [mode, name] : {std::pair{Mode::frames, "frames"}, std::pair{Mode::decode, "decode"}}) {
            const Result r = run(mode, data, size, max_orders, levels, 0);
            std::printf("%-12s %12llu %10.2f %12.2f %10.1f\n", name, static_cast<unsigned long long>(r.messages),
                        r.seconds, static_cast<double>(r.messages) / r.seconds / 1e6,
                        r.seconds * 1e9 / static_cast<double>(r.messages));
        }
        for (const std::size_t ahead : aheads) {
            const Result r = run(Mode::books, data, size, max_orders, levels, ahead);
            char label[32];
            std::snprintf(label, sizeof label, "books+pf%zu", ahead);
            std::printf("%-12s %12llu %10.2f %12.2f %10.1f   (refused %llu)\n", label,
                        static_cast<unsigned long long>(r.messages), r.seconds,
                        static_cast<double>(r.messages) / r.seconds / 1e6,
                        r.seconds * 1e9 / static_cast<double>(r.messages),
                        static_cast<unsigned long long>(r.refused));
            std::fflush(stdout);
        }
    }
    return 0;
}

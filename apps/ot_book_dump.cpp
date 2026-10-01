// Prints the order book ladder of one synthetic instrument after N feed messages, as CSV
// (side,price,quantity,orders). Used by tools/plot_book.py for the README chart.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>

#include "optitrade/book/market_books.hpp"
#include "optitrade/itch/decoder.hpp"
#include "optitrade/sim/synthetic_market.hpp"

namespace {

using namespace optitrade;

struct Apply : itch::NullHandler {
    using itch::NullHandler::on;
    book::MarketBooks* books;
    explicit Apply(book::MarketBooks* b) : books(b) {}
    void on(const itch::StockDirectory& m) noexcept { (void)books->on(m); }
    void on(const itch::AddOrder& m) noexcept { (void)books->on(m); }
    void on(const itch::OrderExecuted& m) noexcept { (void)books->on(m); }
    void on(const itch::OrderExecutedPrice& m) noexcept { (void)books->on(m); }
    void on(const itch::OrderCancel& m) noexcept { (void)books->on(m); }
    void on(const itch::OrderDelete& m) noexcept { (void)books->on(m); }
    void on(const itch::OrderReplace& m) noexcept { (void)books->on(m); }
};

bool parse_u64(const char* s, std::uint64_t& out) {
    char* end = nullptr;
    if (*s == '\0' || *s == '-') return false;
    out = std::strtoull(s, &end, 10);
    return *end == '\0';
}

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s [--seed N] [--symbols N] [--messages N] [--symbol SYM00] [--depth N]\n"
                 "Prints the ladder of one instrument after N flow messages as CSV.\n",
                 argv0);
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t seed = 42, symbols = 8, messages = 20000, depth = 10;
    std::string symbol = "SYM00";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        std::uint64_t* target = a == "--seed" ? &seed : a == "--symbols" ? &symbols
                              : a == "--messages" ? &messages : a == "--depth" ? &depth : nullptr;
        if (a == "--help") { usage(argv[0]); return 0; }
        if (i + 1 >= argc) { usage(argv[0]); return 2; }
        if (a == "--symbol") { symbol = argv[++i]; continue; }
        if (!target || !parse_u64(argv[++i], *target)) { usage(argv[0]); return 2; }
    }
    if (symbols < 1 || symbols > 1024 || depth < 1) { usage(argv[0]); return 2; }

    sim::SyntheticConfig cfg;
    cfg.seed = seed;
    cfg.symbols = static_cast<std::uint16_t>(symbols);
    cfg.messages = messages;

    book::MarketBooks::Config bc;
    bc.max_orders = 1u << 18;
    bc.max_levels_per_side = 64;
    book::MarketBooks books(bc);
    Apply apply(&books);

    sim::SyntheticMarket market(cfg);
    std::array<std::byte, 64> buf{};
    std::size_t len = 0;
    Nanos ts = 0;
    while (market.next(buf, len, ts)) itch::decode(std::span<const std::byte>(buf.data(), len), apply);

    const auto loc = books.locate(Symbol(symbol));
    const book::OrderBook* ob = loc ? books.book(*loc) : nullptr;
    if (!ob) {
        std::fprintf(stderr, "ot_book_dump: unknown symbol '%s'\n", symbol.c_str());
        return 1;
    }
    std::printf("side,price,quantity,orders\n");
    for (const Side side : {Side::buy, Side::sell}) {
        const std::size_t n = std::min<std::size_t>(ob->depth(side), static_cast<std::size_t>(depth));
        for (std::size_t i = 0; i < n; ++i) {
            const book::Level& l = ob->level(side, i);
            std::printf("%s,%lld.%04lld,%u,%u\n", side == Side::buy ? "bid" : "ask",
                        static_cast<long long>(l.price / kPriceScale),
                        static_cast<long long>(l.price % kPriceScale), l.qty, l.orders);
        }
    }
    return 0;
}

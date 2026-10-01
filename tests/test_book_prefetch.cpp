// MarketBooks::prefetch() and OrderBook::prefetch() are cache hints. They must never change what
// the books do, and they must accept any bytes.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "check.hpp"
#include "optitrade/book/market_books.hpp"
#include "optitrade/core/rng.hpp"
#include "optitrade/itch/decoder.hpp"
#include "optitrade/sim/synthetic_market.hpp"

using namespace optitrade;
using book::MarketBooks;

namespace {

struct Apply : itch::NullHandler {
    using itch::NullHandler::on;
    MarketBooks* books;
    std::uint64_t digest{0};
    explicit Apply(MarketBooks* b) : books(b) {}
    void mix(book::Applied r) noexcept { digest = digest * 1000003u + static_cast<std::uint64_t>(r) + 1; }
    void on(const itch::StockDirectory& m) noexcept { mix(books->on(m)); }
    void on(const itch::AddOrder& m) noexcept { mix(books->on(m)); }
    void on(const itch::OrderExecuted& m) noexcept { mix(books->on(m)); }
    void on(const itch::OrderExecutedPrice& m) noexcept { mix(books->on(m)); }
    void on(const itch::OrderCancel& m) noexcept { mix(books->on(m)); }
    void on(const itch::OrderDelete& m) noexcept { mix(books->on(m)); }
    void on(const itch::OrderReplace& m) noexcept { mix(books->on(m)); }
};

using Wire = std::vector<std::byte>;

std::vector<Wire> make_feed(std::uint64_t seed, std::uint64_t n) {
    sim::SyntheticConfig cfg;
    cfg.seed = seed;
    cfg.symbols = 6;
    cfg.messages = n;
    sim::SyntheticMarket market(cfg);
    std::vector<Wire> feed;
    std::array<std::byte, 64> buf{};
    std::size_t len = 0;
    Nanos ts = 0;
    while (market.next(buf, len, ts)) feed.emplace_back(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(len));
    return feed;
}

}  // namespace

OT_TEST(hints_do_not_change_results) {
    const auto feed = make_feed(5, 60'000);
    MarketBooks::Config cfg;
    cfg.max_orders = 1 << 16;
    cfg.max_levels_per_side = 64;
    MarketBooks plain(cfg), hinted(cfg);
    Apply a(&plain), b(&hinted);
    constexpr std::size_t kAhead = 12;
    for (std::size_t i = 0; i < feed.size(); ++i) {
        if (i + kAhead < feed.size()) hinted.prefetch(feed[i + kAhead]);
        (void)itch::decode(feed[i], a);
        (void)itch::decode(feed[i], b);
    }
    OT_CHECK_EQ(a.digest, b.digest);
    OT_CHECK_EQ(plain.live_orders(), hinted.live_orders());
    OT_CHECK_EQ(plain.applied(), hinted.applied());
    for (Locate l = 1; l <= 6; ++l) {
        const auto* x = plain.book(l);
        const auto* y = hinted.book(l);
        OT_CHECK((x == nullptr) == (y == nullptr));
        if (!x) continue;
        for (const Side s : {Side::buy, Side::sell}) {
            OT_CHECK_EQ(x->depth(s), y->depth(s));
            for (std::size_t i = 0; i < x->depth(s); ++i) {
                OT_CHECK_EQ(x->level(s, i).price, y->level(s, i).price);
                OT_CHECK_EQ(x->level(s, i).qty, y->level(s, i).qty);
            }
        }
    }
}

OT_TEST(hints_accept_any_bytes) {
    MarketBooks::Config cfg;
    cfg.max_orders = 1024;
    cfg.max_levels_per_side = 8;
    MarketBooks books(cfg);
    Rng rng(99);
    // Every length from 0 to 64 with random content, including all message type bytes.
    for (int n = 0; n < 20'000; ++n) {
        const std::size_t len = rng.bounded(65);
        std::vector<std::byte> junk(len);  // exact-size heap buffer: ASan sees any overrun
        for (auto& x : junk) x = static_cast<std::byte>(rng.next());
        if (len != 0 && rng.chance(1, 2)) junk[0] = static_cast<std::byte>("AFECXDUPSR"[rng.bounded(10)]);
        books.prefetch(junk);
    }
    OT_CHECK_EQ(books.live_orders(), std::size_t{0});
    OT_CHECK_EQ(books.applied(), std::uint64_t{0});
}

OT_TEST_MAIN()

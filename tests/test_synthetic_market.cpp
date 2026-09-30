// Synthetic market generator: reproducibility, wire validity, book consistency,
// message mix and regime behaviour. The consumer-side checks run the whole stream
// through the real ITCH decoder and book::MarketBooks, so a message that names a
// dead order, crosses the book or decodes badly fails here.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

#include "check.hpp"
#include "optitrade/book/market_books.hpp"
#include "optitrade/core/digest.hpp"
#include "optitrade/itch/decoder.hpp"
#include "optitrade/sim/synthetic_market.hpp"

using namespace optitrade;
using book::Applied;
using book::MarketBooks;
using sim::SyntheticConfig;
using sim::SyntheticMarket;

namespace {

// Drives a generator to completion, calling f(message, timestamp, index).
template <class F>
std::uint64_t for_each_message(const SyntheticConfig& cfg, F&& f) {
    SyntheticMarket gen(cfg);
    std::array<std::byte, 64> buf{};
    std::size_t len = 0;
    Nanos ts = 0;
    std::uint64_t n = 0;
    while (gen.next(buf, len, ts)) {
        f(std::span<const std::byte>(buf.data(), len), ts, n);
        ++n;
    }
    return n;
}

std::uint64_t stream_digest(const SyntheticConfig& cfg, std::uint64_t limit = UINT64_MAX) {
    Digest d;
    for_each_message(cfg, [&](std::span<const std::byte> m, Nanos ts, std::uint64_t n) {
        if (n >= limit) return;
        d.update(ts);
        d.update(m.data(), m.size());
    });
    return d.value();
}

// Applies decoded messages to MarketBooks and remembers the last result.
struct Feeder {
    MarketBooks& books;
    Applied last{Applied::ok};
    Locate locate{};
    OrderRef max_ref{0};
    std::uint64_t ref_violations{0};

    template <class M>
    void on(const M& m) {
        last = books.on(m);
        locate = m.h.locate;
        if constexpr (std::is_same_v<M, itch::AddOrder>) {
            if (m.ref <= max_ref) ++ref_violations;
            max_ref = m.ref;
        } else if constexpr (std::is_same_v<M, itch::OrderReplace>) {
            if (m.new_ref <= max_ref) ++ref_violations;
            max_ref = m.new_ref;
        }
    }
};

MarketBooks::Config book_config() {
    MarketBooks::Config c;
    c.max_orders = 1u << 14;
    c.max_levels_per_side = 64;
    return c;
}

bool ok_or_ignored(Applied a) { return a == Applied::ok || a == Applied::ignored; }

}  // namespace

OT_TEST(same_seed_is_byte_identical_and_different_seeds_differ) {
    SyntheticConfig a;
    a.seed = 99;
    a.messages = 20'000;
    SyntheticConfig b = a;
    OT_CHECK_EQ(stream_digest(a), stream_digest(b));

    b.seed = 100;
    OT_CHECK(stream_digest(a) != stream_digest(b));

    SyntheticConfig c = a;
    c.symbols = 3;
    OT_CHECK(stream_digest(a) != stream_digest(c));

    // Two generators stepping side by side never diverge.
    SyntheticMarket g1(a), g2(a);
    std::array<std::byte, 64> b1{}, b2{};
    std::size_t l1 = 0, l2 = 0;
    Nanos t1 = 0, t2 = 0;
    bool identical = true;
    while (true) {
        const bool r1 = g1.next(b1, l1, t1);
        const bool r2 = g2.next(b2, l2, t2);
        if (r1 != r2 || l1 != l2 || t1 != t2 || std::memcmp(b1.data(), b2.data(), l1) != 0) identical = false;
        if (!r1) break;
    }
    OT_CHECK(identical);
}

OT_TEST(golden_digest_of_first_50k_messages_seed_7) {
    SyntheticConfig cfg;
    cfg.seed = 7;
    cfg.messages = 100'000;
    // Frozen after one verified run. It changes only if the wire bytes or the
    // random-number consumption of the generator change, which would also change
    // every backtest digest downstream.
    OT_CHECK_EQ(stream_digest(cfg, 50'000), std::uint64_t{7170825501908168531ULL});
}

OT_TEST(stream_length_and_preamble) {
    SyntheticConfig cfg;
    cfg.messages = 1'000;
    SyntheticMarket gen(cfg);
    OT_CHECK_EQ(gen.preamble_length(), std::uint64_t{1 + 8 + 8 * 24});

    std::array<std::byte, 64> buf{};
    std::size_t len = 0;
    Nanos ts = 0;
    std::uint64_t n = 0;
    while (gen.next(buf, len, ts)) {
        if (n == 0) {
            OT_CHECK_EQ(static_cast<char>(buf[0]), 'S');
            OT_CHECK_EQ(static_cast<char>(buf[11]), 'O');
        } else if (n <= 8) {
            OT_CHECK_EQ(static_cast<char>(buf[0]), 'R');
        } else if (n < gen.preamble_length()) {
            OT_CHECK_EQ(static_cast<char>(buf[0]), 'A');
        }
        ++n;
    }
    OT_CHECK_EQ(n, gen.preamble_length() + 1'000);
    OT_CHECK_EQ(gen.emitted(), n);
    OT_CHECK(!gen.next(buf, len, ts));  // stays finished
    OT_CHECK_EQ(len, std::size_t{0});
    OT_CHECK_EQ(gen.emitted(), n);
}

OT_TEST(every_message_decodes_with_matching_timestamp) {
    SyntheticConfig cfg;
    cfg.seed = 3;
    cfg.messages = 60'000;
    struct Probe : itch::NullHandler {
        using itch::NullHandler::on;
        Nanos ts{};
        void on(const itch::SystemEvent& m) noexcept { ts = m.h.timestamp; }
        void on(const itch::StockDirectory& m) noexcept { ts = m.h.timestamp; }
        void on(const itch::AddOrder& m) noexcept { ts = m.h.timestamp; }
        void on(const itch::OrderExecuted& m) noexcept { ts = m.h.timestamp; }
        void on(const itch::OrderExecutedPrice& m) noexcept { ts = m.h.timestamp; }
        void on(const itch::OrderCancel& m) noexcept { ts = m.h.timestamp; }
        void on(const itch::OrderDelete& m) noexcept { ts = m.h.timestamp; }
        void on(const itch::OrderReplace& m) noexcept { ts = m.h.timestamp; }
        void on(const itch::Trade& m) noexcept { ts = m.h.timestamp; }
    };
    Nanos prev = 0;
    std::uint64_t bad = 0;
    const std::uint64_t n = for_each_message(cfg, [&](std::span<const std::byte> m, Nanos ts, std::uint64_t) {
        Probe p;
        if (itch::decode(m, p) != DecodeStatus::ok) ++bad;
        if (m.size() != itch::message_length(static_cast<char>(m[0]))) ++bad;
        if (p.ts != ts) ++bad;
        if (ts < prev) ++bad;
        if (ts >= (Nanos{1} << 48)) ++bad;
        prev = ts;
    });
    OT_CHECK_EQ(bad, std::uint64_t{0});
    OT_CHECK_EQ(n, std::uint64_t{1 + 8 + 8 * 24 + 60'000});
}

OT_TEST(mean_gap_matches_configuration) {
    SyntheticConfig cfg;
    cfg.messages = 50'000;
    cfg.mean_gap_ns = 5'000;
    Nanos first = 0, last = 0;
    const std::uint64_t n = for_each_message(cfg, [&](std::span<const std::byte>, Nanos ts, std::uint64_t i) {
        if (i == 0) first = ts;
        last = ts;
    });
    const Nanos mean = (last - first) / (n - 1);
    OT_CHECK(mean > 4'500 && mean < 5'500);
}

OT_TEST(whole_stream_applies_cleanly_and_never_crosses) {
    SyntheticConfig cfg;
    cfg.seed = 11;
    cfg.messages = 200'000;
    MarketBooks books(book_config());
    Feeder feeder{books};
    std::uint64_t bad_result = 0, crossed = 0, decode_errors = 0;
    for_each_message(cfg, [&](std::span<const std::byte> m, Nanos, std::uint64_t) {
        if (itch::decode(m, feeder) != DecodeStatus::ok) ++decode_errors;
        if (!ok_or_ignored(feeder.last)) ++bad_result;
        if (feeder.locate != 0) {
            const book::OrderBook* b = books.book(feeder.locate);
            if (b == nullptr || b->crossed()) ++crossed;
        }
    });
    OT_CHECK_EQ(decode_errors, std::uint64_t{0});
    OT_CHECK_EQ(bad_result, std::uint64_t{0});
    OT_CHECK_EQ(crossed, std::uint64_t{0});
    OT_CHECK_EQ(feeder.ref_violations, std::uint64_t{0});
    // The shadow model bounds the population: the book cannot grow without limit.
    OT_CHECK(books.live_orders() > 8 * 10 && books.live_orders() < 8 * 200);
}

OT_TEST(message_mix_matches_documented_percentages) {
    SyntheticConfig cfg;
    cfg.seed = 5;
    cfg.messages = 100'000;
    std::array<std::uint64_t, 256> count{};
    const std::uint64_t total = for_each_message(cfg, [&](std::span<const std::byte> m, Nanos, std::uint64_t) {
        ++count[std::to_integer<std::uint8_t>(m[0])];
    });
    auto pct = [&](char a, char b = 0) {
        std::uint64_t c = count[static_cast<std::uint8_t>(a)];
        if (b != 0) c += count[static_cast<std::uint8_t>(b)];
        return 100.0 * static_cast<double>(c) / static_cast<double>(total);
    };
    auto near = [](double actual, double nominal) { return actual > nominal - 3.0 && actual < nominal + 3.0; };
    OT_CHECK(near(pct('A', 'F'), 40));
    OT_CHECK(near(pct('D'), 22));
    OT_CHECK(near(pct('X'), 8));
    OT_CHECK(near(pct('E'), 14));
    OT_CHECK(near(pct('U'), 8));
    OT_CHECK(near(pct('C'), 2));
    OT_CHECK(near(pct('P'), 3));
    OT_CHECK(pct('S') < 1.0);
    OT_CHECK(count['F'] > 0);  // attributed adds do occur
}

OT_TEST(every_symbol_builds_depth_on_both_sides) {
    SyntheticConfig cfg;
    cfg.seed = 21;
    cfg.messages = 50'000;
    MarketBooks books(book_config());
    Feeder feeder{books};
    std::array<std::size_t, 8> max_depth_bid{}, max_depth_ask{};
    bool preamble_depth_ok = true;
    const std::uint64_t preamble = SyntheticMarket(cfg).preamble_length();
    for_each_message(cfg, [&](std::span<const std::byte> m, Nanos, std::uint64_t i) {
        itch::decode(m, feeder);
        for (Locate l = 1; l <= 8; ++l) {
            const book::OrderBook* b = books.book(l);
            if (b == nullptr) continue;
            max_depth_bid[l - 1] = std::max(max_depth_bid[l - 1], b->depth(Side::buy));
            max_depth_ask[l - 1] = std::max(max_depth_ask[l - 1], b->depth(Side::sell));
            if (i + 1 == preamble && (b->depth(Side::buy) < 5 || b->depth(Side::sell) < 5)) preamble_depth_ok = false;
        }
    });
    OT_CHECK(preamble_depth_ok);
    for (std::size_t s = 0; s < 8; ++s) {
        OT_CHECK(max_depth_bid[s] >= 5);
        OT_CHECK(max_depth_ask[s] >= 5);
    }
}

OT_TEST(regimes_flip_the_sign_of_book_imbalance) {
    SyntheticConfig cfg;
    cfg.seed = 8;
    cfg.messages = 100'000;
    cfg.regime_period = 20'000;  // five regimes: buy, sell, buy, sell, buy
    MarketBooks books(book_config());
    Feeder feeder{books};
    const std::uint64_t preamble = SyntheticMarket(cfg).preamble_length();
    std::array<std::int64_t, 5> sum{};
    std::array<std::int64_t, 5> samples{};
    for_each_message(cfg, [&](std::span<const std::byte> m, Nanos, std::uint64_t i) {
        itch::decode(m, feeder);
        if (i < preamble) return;
        const std::uint64_t flow = i - preamble;
        const std::uint64_t regime = flow / cfg.regime_period;
        // Skip the first quarter of every regime: the book needs time to relax
        // towards the new skew.
        if (regime >= 5 || flow % cfg.regime_period < cfg.regime_period / 4 || flow % 50 != 0) return;
        std::int64_t imbalance = 0;
        for (Locate l = 1; l <= 8; ++l) {
            const book::OrderBook* b = books.book(l);
            imbalance += static_cast<std::int64_t>(b->total_qty(Side::buy, 5)) -
                         static_cast<std::int64_t>(b->total_qty(Side::sell, 5));
        }
        sum[regime] += imbalance;
        ++samples[regime];
    });
    for (std::size_t r = 0; r < 5; ++r) {
        OT_CHECK(samples[r] > 100);
        const std::int64_t mean = sum[r] / std::max<std::int64_t>(samples[r], 1);
        if (r % 2 == 0) {
            OT_CHECK(mean > 1'000);   // buy-heavy regime: more displayed size on the bid
        } else {
            OT_CHECK(mean < -1'000);  // sell-heavy regime
        }
    }
}

OT_TEST(timestamps_saturate_at_48_bits) {
    SyntheticConfig cfg;
    cfg.messages = 100'000;
    cfg.mean_gap_ns = Nanos{1} << 40;  // clamped to 2^32; the day would overflow 48 bits
    Nanos prev = 0;
    bool monotone = true, in_range = true;
    for_each_message(cfg, [&](std::span<const std::byte>, Nanos ts, std::uint64_t) {
        if (ts < prev) monotone = false;
        if (ts >= (Nanos{1} << 48)) in_range = false;
        prev = ts;
    });
    OT_CHECK(monotone);
    OT_CHECK(in_range);
    OT_CHECK_EQ(prev, (Nanos{1} << 48) - 1);  // saturated
}

OT_TEST(degenerate_configurations_are_sanitised) {
    SyntheticConfig cfg;
    cfg.symbols = 0;
    cfg.tick = 0;
    cfg.start_price = 0;
    cfg.mean_gap_ns = 0;
    cfg.regime_period = 0;
    cfg.messages = 30'000;
    MarketBooks books(book_config());
    Feeder feeder{books};
    std::uint64_t bad = 0, n = 0;
    for_each_message(cfg, [&](std::span<const std::byte> m, Nanos, std::uint64_t) {
        ++n;
        if (itch::decode(m, feeder) != DecodeStatus::ok || !ok_or_ignored(feeder.last)) ++bad;
        if (feeder.locate != 0 && books.book(feeder.locate)->crossed()) ++bad;
    });
    OT_CHECK_EQ(bad, std::uint64_t{0});
    OT_CHECK_EQ(n, std::uint64_t{1 + 1 + 24 + 30'000});
    OT_CHECK(books.locate(Symbol("SYM00")).has_value());

    // Many symbols: names are distinct and stay within the eight-byte field.
    SyntheticConfig wide;
    wide.symbols = 1'200;
    wide.messages = 0;
    MarketBooks wide_books(MarketBooks::Config{1u << 16, 16, 1u << 12});
    Feeder wf{wide_books};
    for_each_message(wide, [&](std::span<const std::byte> m, Nanos, std::uint64_t) {
        itch::decode(m, wf);
        if (!ok_or_ignored(wf.last)) ++bad;
    });
    OT_CHECK_EQ(bad, std::uint64_t{0});
    OT_CHECK(wide_books.locate(Symbol("SYM1199")).has_value());
    OT_CHECK(wide_books.locate(Symbol("SYM00")).has_value());
}

OT_TEST(rejects_too_small_output_buffer_without_advancing) {
    SyntheticConfig cfg;
    cfg.messages = 10;
    SyntheticMarket gen(cfg);
    std::array<std::byte, itch::kMaxMessageLength - 1> tiny{};
    std::size_t len = 99;
    Nanos ts = 0;
    OT_CHECK(!gen.next(tiny, len, ts));
    OT_CHECK_EQ(len, std::size_t{0});
    OT_CHECK_EQ(gen.emitted(), std::uint64_t{0});

    std::array<std::byte, itch::kMaxMessageLength> exact{};
    OT_CHECK(gen.next(exact, len, ts));
    OT_CHECK_EQ(len, itch::kSystemEventLength);
    OT_CHECK_EQ(gen.emitted(), std::uint64_t{1});
}

OT_TEST_MAIN()

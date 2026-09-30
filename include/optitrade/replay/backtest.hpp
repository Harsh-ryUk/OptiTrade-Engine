#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <utility>

#include "optitrade/core/digest.hpp"
#include "optitrade/core/types.hpp"
#include "optitrade/engine/engine.hpp"
#include "optitrade/oms/interfaces.hpp"
#include "optitrade/ouch/codec.hpp"
#include "optitrade/replay/capture.hpp"
#include "optitrade/risk/risk_engine.hpp"
#include "optitrade/sim/exchange_sim.hpp"
#include "optitrade/strategy/strategy.hpp"

// Backtester: replays a timestamped ITCH source through the trading engine while the
// exchange simulator answers the engine's orders, and summarises what happened.
//
//   record --> sim.advance(ts) --> deliver due reports --> engine.on_ouch
//          '-> sim.on_itch(msg, ts)                    (same message, exchange view)
//          '-> engine.on_itch(msg, ts)                 (same message, engine view)
//
// The order of the three steps is what makes the run causal: an order sent at time t
// cannot be filled by a message stamped before t + order_latency, and its reports cannot
// reach the engine before t + order_latency + report_latency. Reports are handed to the
// engine at the moment they fall due, stamped with their own delivery time, so strategy
// callbacks see the clock a live system would show.
//
// Digest. Every order the engine hands to the gateway (with its send time and whether the
// simulator accepted it) and every report delivered to the engine (with its delivery time)
// is folded into one 64-bit FNV-1a value, in the order it happened. Two runs with the same
// digest made the same decisions and received the same answers, byte for byte; the
// determinism test freezes these values across compilers and platforms.
//
// Choices the specification leaves open
//   * Time never runs backwards. A record stamped earlier than its predecessor (corrupt or
//     concatenated files) is processed at the predecessor's time. The simulator requires
//     non-decreasing send times, and a strategy must never see the clock step back.
//   * End of input. Orders still in flight and reports still queued are played out: the
//     simulator is advanced to the end of time and every report is delivered, repeatedly,
//     because a strategy may react to a report with another order. The loop is bounded
//     (kMaxFlushRounds); a strategy that keeps answering forever is cut off there.
//   * Unrealized PnL is marked at the mid of each instrument's final book (the same mid
//     definition the engine uses: both sides present and not crossed). An instrument
//     without a valid mid keeps its last mark from the engine.
//   * PnL and drawdown are in price units (1e-4 currency), as in the risk engine.
//   * `volume` is the sum of executed shares in the reports delivered to the engine.
//   * `dropped_reports` is an addition to the documented report fields. It stays 0 unless the
//     simulator's report ring overflowed, in which case the run is not trustworthy.
//
// Sizing. Nothing is allocated per message, but the engine and the simulator each build
// their own MarketBooks, so `BacktestConfig{}` (default MarketBooks::Config) reserves
// well over 100 MB per book set. Size books, order tables and locates explicitly. The order
// manager keeps every order of the run (see oms/order_manager.hpp), so engine.oms.max_orders
// bounds the orders one backtest can send; beyond it the strategy's submits are refused.
namespace optitrade::replay {

struct BacktestConfig {
    engine::Config engine{};
    sim::SimConfig sim{};
};

struct BacktestReport {
    std::uint64_t messages{};             // records read from the source
    std::uint64_t book_updates{};         // order-flow messages the engine's books applied
    std::uint64_t orders_sent{};          // new orders the gateway accepted
    std::uint64_t orders_rejected_risk{}; // strategy submits/replaces refused by pre-trade risk
    std::uint64_t fills{};                // executions applied to orders
    std::uint64_t volume{};               // shares executed
    std::int64_t realized_pnl{};
    std::int64_t unrealized_pnl{};
    std::int64_t total_pnl{};
    std::int64_t max_drawdown{};
    std::uint64_t digest{};               // over every outbound order message and every report delivered
    std::uint64_t dropped_reports{};

    friend bool operator==(const BacktestReport&, const BacktestReport&) = default;
};

// Anything that yields timestamped ITCH messages: CaptureReader, ItchFileReader, MemorySource.
template <class Src>
concept RecordSource = requires(Src& s, Record& r) {
    { s.next(r) } -> std::convertible_to<bool>;
};

namespace detail {

inline constexpr int kMaxFlushRounds = 64;
inline constexpr std::uint64_t kTagOrder = 1;
inline constexpr std::uint64_t kTagReport = 2;

// Sits between the engine and the simulator: forwards every order and folds it into the
// digest. Orders are re-encoded to OUCH bytes so the digest covers exactly what a real
// session would have put on the wire.
class DigestGateway final : public oms::OrderGateway {
public:
    DigestGateway(sim::ExchangeSim& exchange, Digest& digest) noexcept : sim_(exchange), digest_(digest) {}

    bool send(const ouch::EnterOrder& m, Nanos now) override { return forward(m, now); }
    bool send(const ouch::CancelOrder& m, Nanos now) override { return forward(m, now); }
    bool send(const ouch::ReplaceOrder& m, Nanos now) override { return forward(m, now); }

private:
    template <class M>
    bool forward(const M& m, Nanos now) {
        std::array<std::byte, ouch::kMaxMessageLength> wire{};
        const std::size_t n = ouch::encode(m, wire);
        const bool accepted = sim_.send(m, now);
        digest_.update(kTagOrder);
        digest_.update(now);
        digest_.update(accepted ? 1u : 0u);
        digest_.update(n);
        digest_.update(wire.data(), n);
        return accepted;
    }

    sim::ExchangeSim& sim_;
    Digest& digest_;
};

// Adds up executed shares in an outbound OUCH message.
struct VolumeCounter : ouch::NullHandler {
    using ouch::NullHandler::on;
    void on(const ouch::Executed& e) noexcept { shares += e.shares; }
    std::uint64_t shares{};
};

// Mid of a sane touch (both sides present, positive, not crossed), else 0.
inline Price mid_of(const book::OrderBook* b) noexcept {
    if (b == nullptr) return 0;
    const auto bid = b->best(Side::buy);
    const auto ask = b->best(Side::sell);
    if (!bid || !ask || bid->price <= 0 || bid->price >= ask->price) return 0;
    return bid->price + (ask->price - bid->price) / 2;
}

}  // namespace detail

template <strategy::Strategy S, RecordSource Source>
BacktestReport run_backtest(Source& src, const BacktestConfig& config, S strategy) {
    Digest digest;
    sim::ExchangeSim exchange(config.sim);
    detail::DigestGateway gateway(exchange, digest);
    engine::Engine<S> eng(config.engine, gateway, std::move(strategy));

    detail::VolumeCounter volume;
    // Hands every report due at `upto` to the engine, oldest first. Returns how many.
    auto deliver = [&](Nanos upto) {
        std::size_t delivered = 0;
        exchange.drain_reports(upto, [&](std::span<const std::byte> msg, Nanos at) {
            digest.update(detail::kTagReport);
            digest.update(at);
            digest.update(msg.size());
            digest.update(msg.data(), msg.size());
            ouch::decode_outbound(msg, volume);
            eng.on_ouch(msg, at);
            ++delivered;
        });
        return delivered;
    };

    BacktestReport report;
    Record rec;
    Nanos now = 0;
    while (src.next(rec)) {
        now = std::max(now, rec.ts);
        exchange.advance(now);
        deliver(now);
        exchange.on_itch(rec.payload, now);
        eng.on_itch(rec.payload, now);
        ++report.messages;
    }

    // Play out whatever is still in flight. An order sent while reports are delivered
    // needs another pass, hence the loop.
    constexpr Nanos kEndOfTime = ~Nanos{0};
    for (int round = 0; round < detail::kMaxFlushRounds; ++round) {
        exchange.advance(kEndOfTime);
        if (deliver(kEndOfTime) == 0) break;
    }

    const std::size_t locates = std::min(config.engine.max_locates, risk::RiskEngine::kMaxLocates);
    for (std::size_t l = 0; l < locates; ++l) {
        const auto loc = static_cast<Locate>(l);
        if (const Price mid = detail::mid_of(eng.books().book(loc)); mid > 0) eng.risk().mark(loc, mid);
    }

    const engine::Stats& st = eng.stats();
    report.book_updates = st.book_updates;
    report.orders_sent = st.orders_sent;
    report.orders_rejected_risk = st.risk_rejects;
    report.fills = st.fills;
    report.volume = volume.shares;
    report.realized_pnl = eng.risk().realized_pnl();
    report.unrealized_pnl = eng.risk().unrealized_pnl();
    report.total_pnl = eng.risk().total_pnl();
    report.max_drawdown = eng.risk().max_drawdown();
    report.digest = digest.value();
    report.dropped_reports = exchange.dropped_reports();
    return report;
}

namespace detail {

// Price units (1e-4) as decimal text, integer arithmetic only. Works on the magnitude as
// unsigned so that INT64_MIN needs no special case.
inline void format_units(char* out, std::size_t cap, std::int64_t v) noexcept {
    const std::uint64_t mag = v < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
    std::snprintf(out, cap, "%s%llu.%04llu", v < 0 ? "-" : "",
                  static_cast<unsigned long long>(mag / kPriceScale),
                  static_cast<unsigned long long>(mag % kPriceScale));
}

}  // namespace detail

inline void print_report(const BacktestReport& r, const char* title, std::FILE* out) {
    const auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    char realized[40], unrealized[40], total[40], drawdown[40];
    detail::format_units(realized, sizeof realized, r.realized_pnl);
    detail::format_units(unrealized, sizeof unrealized, r.unrealized_pnl);
    detail::format_units(total, sizeof total, r.total_pnl);
    detail::format_units(drawdown, sizeof drawdown, r.max_drawdown);

    std::fprintf(out, "== %s ==\n", title);
    std::fprintf(out, "messages        %llu\n", u(r.messages));
    std::fprintf(out, "book updates    %llu\n", u(r.book_updates));
    std::fprintf(out, "orders sent     %llu\n", u(r.orders_sent));
    std::fprintf(out, "risk rejects    %llu\n", u(r.orders_rejected_risk));
    std::fprintf(out, "fills           %llu\n", u(r.fills));
    std::fprintf(out, "volume          %llu\n", u(r.volume));
    std::fprintf(out, "realized pnl    %s\n", realized);
    std::fprintf(out, "unrealized pnl  %s\n", unrealized);
    std::fprintf(out, "total pnl       %s\n", total);
    std::fprintf(out, "max drawdown    %s\n", drawdown);
    if (r.dropped_reports != 0) {
        std::fprintf(out, "dropped reports %llu  (simulator report ring overflowed; results unreliable)\n",
                     u(r.dropped_reports));
    }
    std::fprintf(out, "digest          %016llx\n", u(r.digest));
}

}  // namespace optitrade::replay

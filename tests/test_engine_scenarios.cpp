#include <cassert>
#include <cstdint>

#include "optitrade/dpdk/engine_wire_adapter.hpp"
#include "optitrade/engine/trading_engine.hpp"
#include "optitrade/wire/market_data_packet.hpp"

namespace {

optitrade::EngineConfig make_config(
    const optitrade::Quantity max_order_quantity = 100) noexcept {
    optitrade::EngineConfig config{};
    config.symbol_id = 0;
    config.strategy.imbalance_threshold_bps = 6000;
    config.strategy.order_quantity = 10;
    config.risk.max_order_quantity = max_order_quantity;
    config.risk.max_absolute_position = 1000;
    config.risk.max_notional_ticks = 1'000'000'000ULL;
    config.risk.max_outstanding_orders = 64;
    return config;
}

optitrade::wire::MarketDataMessage make_message(
    const std::uint32_t sequence_number,
    const optitrade::wire::Side side,
    const std::uint16_t level,
    const std::int64_t price_ticks,
    const std::uint32_t quantity,
    const std::uint32_t symbol_id = 77) noexcept {
    optitrade::wire::MarketDataMessage message{};
    message.sequence_number = sequence_number;
    message.exchange_timestamp_ns =
        static_cast<std::uint64_t>(sequence_number) * 1000ULL;
    message.symbol_id = sequence_number % 4;
    message.price_ticks = price_ticks;
    message.quantity = quantity;
    message.side = side;
    message.action = optitrade::wire::UpdateAction::modify;
    message.level = level;
    return message;
}

optitrade::EngineResult apply_message(
    optitrade::TradingEngine<64>& engine,
    const optitrade::wire::MarketDataMessage& message) noexcept {
    optitrade::MarketUpdate update{};

    assert(optitrade::dpdk::EngineWireAdapter::to_market_update(
        message,
        update));

    return engine.on_market_update(update);
}

void seed_balanced_book(
    optitrade::TradingEngine<64>& engine,
    std::uint32_t& sequence_number) noexcept {
    for (std::uint16_t level = 0; level < 5; ++level) {
        const auto result = apply_message(
            engine,
            make_message(
                sequence_number++,
                optitrade::wire::Side::sell,
                level,
                100100 + level,
                10));

        assert(result.status == optitrade::EngineStatus::no_order);
    }

    for (std::uint16_t level = 0; level < 5; ++level) {
        const auto result = apply_message(
            engine,
            make_message(
                sequence_number++,
                optitrade::wire::Side::buy,
                level,
                100000 - level,
                10));

        assert(result.status == optitrade::EngineStatus::no_order);
    }
}

}  // namespace

int main() {
    static_assert(sizeof(optitrade::OrderRequest) == 32);

    {
        optitrade::TradingEngine<64> engine(make_config());
        std::uint32_t sequence_number = 1;

        seed_balanced_book(engine, sequence_number);

        optitrade::OrderRequest order{};
        assert(!engine.pop_order(order));
    }

    {
        optitrade::TradingEngine<64> engine(make_config());
        std::uint32_t sequence_number = 100;

        seed_balanced_book(engine, sequence_number);

        const auto result = apply_message(
            engine,
            make_message(
                sequence_number++,
                optitrade::wire::Side::buy,
                0,
                100000,
                1000));

        assert(result.status == optitrade::EngineStatus::order_emitted);
        assert(result.signal == optitrade::Signal::buy);

        optitrade::OrderRequest order{};
        assert(engine.pop_order(order));
        assert(order.side == optitrade::Side::buy);
        assert(order.price_ticks == 100100);
        assert(order.quantity == 10);
    }

    {
        optitrade::TradingEngine<64> engine(make_config());
        std::uint32_t sequence_number = 200;

        seed_balanced_book(engine, sequence_number);

        const auto result = apply_message(
            engine,
            make_message(
                sequence_number++,
                optitrade::wire::Side::sell,
                0,
                100100,
                1000));

        assert(result.status == optitrade::EngineStatus::order_emitted);
        assert(result.signal == optitrade::Signal::sell);

        optitrade::OrderRequest order{};
        assert(engine.pop_order(order));
        assert(order.side == optitrade::Side::sell);
        assert(order.price_ticks == 100000);
        assert(order.quantity == 10);
    }

    {
        optitrade::TradingEngine<64> engine(make_config(5));
        std::uint32_t sequence_number = 300;

        seed_balanced_book(engine, sequence_number);

        const auto result = apply_message(
            engine,
            make_message(
                sequence_number++,
                optitrade::wire::Side::buy,
                0,
                100000,
                1000));

        assert(result.status == optitrade::EngineStatus::risk_rejected);

        optitrade::OrderRequest order{};
        assert(!engine.pop_order(order));
    }

    {
        optitrade::TradingEngine<64> engine(make_config());

        const auto result = apply_message(
            engine,
            make_message(
                400,
                optitrade::wire::Side::buy,
                0,
                100000,
                10,
                99));

        assert(result.status == optitrade::EngineStatus::invalid_update);
    }

    return 0;
}

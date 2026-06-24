#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include "optitrade/dpdk/engine_wire_adapter.hpp"
#include "optitrade/engine/trading_engine.hpp"
#include "optitrade/wire/order_packet.hpp"

namespace {

optitrade::wire::MarketDataMessage make_market_message(
    const std::uint32_t sequence,
    const optitrade::wire::Side side,
    const std::uint16_t level,
    const std::int64_t price_ticks,
    const std::uint32_t quantity) {
    optitrade::wire::MarketDataMessage message{};
    message.sequence_number = sequence;
    message.exchange_timestamp_ns = sequence * 1000ULL;
    message.symbol_id = sequence % 4;
    message.price_ticks = price_ticks;
    message.quantity = quantity;
    message.side = side;
    message.action = optitrade::wire::UpdateAction::modify;
    message.level = level;
    return message;
}

optitrade::EngineResult send_to_engine(
    optitrade::TradingEngine<16>& engine,
    const optitrade::wire::MarketDataMessage& wire_message) {
    optitrade::MarketUpdate update{};

    assert(optitrade::dpdk::EngineWireAdapter::to_market_update(
        wire_message,
        update));

    return engine.on_market_update(update);
}

}  // namespace

int main() {
    optitrade::EngineConfig config{};
    config.symbol_id = 0;
    config.strategy.imbalance_threshold_bps = 6000;
    config.strategy.order_quantity = 10;
    config.risk.max_order_quantity = 100;
    config.risk.max_absolute_position = 1000;
    config.risk.max_notional_ticks = 1'000'000'000ULL;

    optitrade::TradingEngine<16> engine(config);

    std::uint32_t sequence = 1;

    for (std::uint16_t level = 0; level < 5; ++level) {
        const auto result = send_to_engine(
            engine,
            make_market_message(
                sequence++,
                optitrade::wire::Side::sell,
                level,
                100100 + level,
                10));

        assert(result.status == optitrade::EngineStatus::no_order);
    }

    for (std::uint16_t level = 0; level < 5; ++level) {
        const auto result = send_to_engine(
            engine,
            make_market_message(
                sequence++,
                optitrade::wire::Side::buy,
                level,
                100000 - level,
                10));

        assert(result.status == optitrade::EngineStatus::no_order);
    }

    const auto trigger_result = send_to_engine(
        engine,
        make_market_message(
            sequence++,
            optitrade::wire::Side::buy,
            0,
            100000,
            1000));

    assert(trigger_result.status == optitrade::EngineStatus::order_emitted);
    assert(trigger_result.signal == optitrade::Signal::buy);
    assert(trigger_result.imbalance_bps >= 6000);

    optitrade::OrderRequest emitted_order{};
    assert(engine.pop_order(emitted_order));

    static_assert(sizeof(optitrade::OrderRequest) == 40);

    assert(emitted_order.client_order_id == 1);
    assert(emitted_order.symbol_id == 0);
    assert(emitted_order.price_ticks == 100100);
    assert(emitted_order.quantity == 10);
    assert(emitted_order.side == optitrade::Side::buy);
    assert(emitted_order.order_kind == optitrade::OrderKind::limit);

    optitrade::wire::OutboundOrderMessage wire_order{};

    assert(optitrade::dpdk::EngineWireAdapter::to_wire_order(
        emitted_order,
        5001,
        wire_order));

    optitrade::wire::WireMessage encoded{};
    assert(optitrade::wire::encode_order(wire_order, encoded));

    optitrade::wire::OutboundOrderMessage decoded{};

    assert(optitrade::wire::decode_order(
               std::span<const std::byte>(encoded),
               decoded) ==
           optitrade::wire::DecodeError::none);

    assert(decoded.sequence_number == 5001);
    assert(decoded.client_order_id == emitted_order.client_order_id);
    assert(decoded.symbol_id == emitted_order.symbol_id);
    assert(decoded.price_ticks == emitted_order.price_ticks);
    assert(decoded.quantity == emitted_order.quantity);
    assert(decoded.side == optitrade::wire::Side::buy);
    assert(decoded.reserved == emitted_order.source_sequence);

    return 0;
}

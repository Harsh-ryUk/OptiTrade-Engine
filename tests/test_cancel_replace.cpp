// Author: Harsh
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>

#include "optitrade/engine/trading_engine.hpp"

using namespace optitrade;

namespace {

EngineResult feed(TradingEngine<64>& engine, std::uint32_t& seq, Side side,
                  std::uint16_t level, PriceTicks price, Quantity qty) {
    MarketUpdate u{};
    u.sequence_number = seq++;
    u.symbol_id = 0;
    u.side = side;
    u.level = level;
    u.price_ticks = price;
    u.quantity = qty;
    return engine.on_market_update(u);
}

}  // namespace

int main() {
    EngineConfig config{};
    config.strategy.order_quantity = 10;
    config.strategy.imbalance_threshold_bps = 6000;

    auto engine = std::make_unique<TradingEngine<64>>(config);
    std::uint32_t seq = 1;

    for (std::uint16_t l = 0; l < 5; ++l) feed(*engine, seq, Side::sell, l, 100100 + l, 10);
    for (std::uint16_t l = 0; l < 5; ++l) feed(*engine, seq, Side::buy, l, 100000 - l, 10);

    // Heavy bids -> BUY.
    assert(feed(*engine, seq, Side::buy, 0, 100000, 1000).status == EngineStatus::order_emitted);
    assert(engine->risk(0).position() == 10);

    // Heavy asks right after -> flips to SELL: cancel the buy, send a new sell.
    assert(feed(*engine, seq, Side::sell, 0, 100100, 5000).status == EngineStatus::order_emitted);
    assert(engine->risk(0).position() == -10);  // buy exposure released, not doubled

    OrderRequest o{};
    assert(engine->pop_order(o) && o.message_type == MessageType::new_order && o.side == Side::buy);
    assert(engine->pop_order(o) && o.message_type == MessageType::cancel && o.side == Side::buy);
    assert(engine->pop_order(o) && o.message_type == MessageType::new_order && o.side == Side::sell);
    assert(!engine->pop_order(o));

    // A gap is reported and counted.
    assert(feed(*engine, seq += 5, Side::buy, 0, 100000, 10).status == EngineStatus::gap_detected);
    assert(engine->sequence_tracker().get_total_gap_count() == 1);

    std::cout << "test_cancel_replace pass\n";
    return 0;
}

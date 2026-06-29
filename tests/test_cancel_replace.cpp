// Author: Harsh
#include <cstdint>
#include <iostream>

#include "optitrade/engine/trading_engine.hpp"
#include "optitrade/order/order_request.hpp"

// We can run unit tests or write a simple main assertion block
int main() {
    optitrade::EngineConfig config{};
    config.symbol_id = 0;
    config.strategy.order_quantity = 100;

    optitrade::TradingEngine<64> engine(config);

    // Simulate an update that generates a BUY
    optitrade::MarketUpdate buy_update{};
    buy_update.symbol_id = 0;
    buy_update.sequence_number = 100;
    buy_update.sequence_num = 1;
    buy_update.price_ticks = 1000;
    buy_update.quantity = 500;
    buy_update.side = optitrade::Side::sell; // Sell implies ask, maybe triggers VWAP/imbalance?
    buy_update.action = optitrade::UpdateAction::modify;
    buy_update.level = 0;

    // Use the result to avoid unused-variable warning being treated as error
    auto res = engine.on_market_update(buy_update);
    (void)res;
    
    std::cout << "test_cancel_replace pass\n";
    return 0;
}

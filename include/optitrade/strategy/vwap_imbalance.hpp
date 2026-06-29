// Author: Harsh
#pragma once

#include <cstdint>

#include "optitrade/book/fixed_l2_book.hpp"
#include "optitrade/common/types.hpp"

namespace optitrade {

#include "optitrade/strategy/strategy_types.hpp"


class VWAPImbalanceStrategy {
public:
    explicit VWAPImbalanceStrategy(const StrategyConfig config = {}) noexcept
        : config_(config) {}

    [[nodiscard]] StrategyDecision evaluate(const FixedL2Book& book) noexcept {
        const auto& bids = book.bids();
        const auto& asks = book.asks();

        double total_bid_vol = 0.0;
        double bid_price_vol = 0.0;
        for (const auto& level : bids) {
            if (level.quantity > 0) {
                total_bid_vol += level.quantity;
                bid_price_vol += static_cast<double>(level.price_ticks) * level.quantity;
            }
        }

        double total_ask_vol = 0.0;
        double ask_price_vol = 0.0;
        for (const auto& level : asks) {
            if (level.quantity > 0) {
                total_ask_vol += level.quantity;
                ask_price_vol += static_cast<double>(level.price_ticks) * level.quantity;
            }
        }

        if (total_bid_vol == 0.0 || total_ask_vol == 0.0) {
            return {Signal::hold, 0, 0, 0};
        }

        double vwap_bid = bid_price_vol / total_bid_vol;
        double vwap_ask = ask_price_vol / total_ask_vol;

        StrategyDecision decision{};
        decision.quantity = config_.order_quantity;

        if (vwap_bid > vwap_ask * 1.001) {
            decision.signal = Signal::buy;
            decision.limit_price_ticks = asks[0].price_ticks;
        } else if (vwap_ask > vwap_bid * 1.001) {
            decision.signal = Signal::sell;
            decision.limit_price_ticks = bids[0].price_ticks;
        } else {
            decision.signal = Signal::hold;
        }

        return decision;
    }

private:
    StrategyConfig config_{};
};

}  // namespace optitrade

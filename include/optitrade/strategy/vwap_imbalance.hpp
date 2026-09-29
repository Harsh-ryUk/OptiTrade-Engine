// Author: Harsh
#pragma once

#include <cstdint>

#include "optitrade/book/fixed_l2_book.hpp"
#include "optitrade/common/types.hpp"
#include "optitrade/strategy/strategy_types.hpp"

namespace optitrade {


class VWAPImbalanceStrategy {
public:
    VWAPImbalanceStrategy(const StrategyConfig config = {}) noexcept
        : config_(config) {}

    // Signals on top-of-book depth imbalance: (bid_qty - ask_qty) / total_qty in bps.
    [[nodiscard]] StrategyDecision evaluate(const FixedL2Book& book) noexcept {
        StrategyDecision decision{};

        if (!book.has_complete_visible_depth()) {
            return decision;
        }

        const auto bid_qty = static_cast<std::int64_t>(book.total_bid_quantity());
        const auto ask_qty = static_cast<std::int64_t>(book.total_ask_quantity());
        const std::int64_t total = bid_qty + ask_qty;

        decision.imbalance_bps = ((bid_qty - ask_qty) * 10000) / total;

        if (decision.imbalance_bps >= config_.imbalance_threshold_bps) {
            decision.signal = Signal::buy;
            decision.limit_price_ticks = *book.best_ask();
        } else if (decision.imbalance_bps <= -config_.imbalance_threshold_bps) {
            decision.signal = Signal::sell;
            decision.limit_price_ticks = *book.best_bid();
        }

        decision.quantity = config_.order_quantity;
        return decision;
    }

private:
    StrategyConfig config_{};
};

}  // namespace optitrade

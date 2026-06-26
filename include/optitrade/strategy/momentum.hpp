#pragma once

#include <cstdint>
#include <array>
#include <optional>

#include "optitrade/book/fixed_l2_book.hpp"
#include "optitrade/strategy/strategy_types.hpp"

namespace optitrade {

class MomentumStrategy {
public:
    explicit MomentumStrategy(const StrategyConfig config = {}) noexcept
        : config_(config) {}

    [[nodiscard]] StrategyDecision evaluate(const FixedL2Book& book) noexcept {
        const auto& bids = book.bids();
        const auto& asks = book.asks();

        if (bids.empty() || asks.empty() || bids[0].quantity == 0 || asks[0].quantity == 0) {
            return {Signal::hold, 0, 0, 0};
        }

        double mid_price = (static_cast<double>(bids[0].price_ticks) + asks[0].price_ticks) / 2.0;

        if (last_mid_price_.has_value()) {
            int direction = 0;
            if (mid_price > last_mid_price_.value()) {
                direction = 1;
            } else if (mid_price < last_mid_price_.value()) {
                direction = -1;
            }

            history_[history_idx_] = direction;
            history_idx_ = (history_idx_ + 1) % 8;
            
            if (history_count_ < 8) {
                history_count_++;
            }
        }

        last_mid_price_ = mid_price;

        StrategyDecision decision{};
        decision.quantity = config_.order_quantity;
        decision.signal = Signal::hold;

        if (history_count_ == 8) {
            int ups = 0;
            int downs = 0;
            for (int dir : history_) {
                if (dir == 1) ups++;
                else if (dir == -1) downs++;
            }

            if (ups >= 6) {
                decision.signal = Signal::buy;
                decision.limit_price_ticks = asks[0].price_ticks;
            } else if (downs >= 6) {
                decision.signal = Signal::sell;
                decision.limit_price_ticks = bids[0].price_ticks;
            }
        }

        return decision;
    }

private:
    StrategyConfig config_{};
    std::optional<double> last_mid_price_{};
    std::array<int, 8> history_{};
    size_t history_idx_{0};
    size_t history_count_{0};
};

}  // namespace optitrade

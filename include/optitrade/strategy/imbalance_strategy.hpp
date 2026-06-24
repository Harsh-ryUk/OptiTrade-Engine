#pragma once

#include <cstdint>
#include <deque>

#include "optitrade/book/fixed_l2_book.hpp"

namespace optitrade {

enum class StrategyType {
    basic,
    vwap,
    momentum
};

struct StrategyConfig {
    StrategyType type{StrategyType::basic};
    std::int64_t imbalance_threshold_bps{6000};
    Quantity order_quantity{10};

    // For VWAP
    std::int64_t vwap_threshold_ticks{2};

    // For Momentum
    std::size_t momentum_window{5};
    std::int64_t momentum_threshold_ticks{5};
};

struct StrategyDecision {
    Signal signal{Signal::no_signal};
    PriceTicks limit_price_ticks{};
    Quantity quantity{};
    std::int64_t imbalance_bps{};
};

class ImbalanceStrategy {
public:
    ImbalanceStrategy() noexcept = default;

    explicit ImbalanceStrategy(const StrategyConfig config) noexcept
        : config_(config) {
    }

    [[nodiscard]] StrategyDecision evaluate(
        const FixedL2Book& book) noexcept {
        
        switch (config_.type) {
            case StrategyType::vwap:
                return evaluate_vwap(book);
            case StrategyType::momentum:
                return evaluate_momentum(book);
            case StrategyType::basic:
            default:
                return evaluate_basic(book);
        }
    }

private:
    [[nodiscard]] StrategyDecision evaluate_basic(
        const FixedL2Book& book) const noexcept {
        StrategyDecision decision{};

        if (!book.has_complete_visible_depth()) {
            return decision;
        }

        const std::uint64_t bid_quantity = book.total_bid_quantity();
        const std::uint64_t ask_quantity = book.total_ask_quantity();
        const std::uint64_t total_quantity = bid_quantity + ask_quantity;

        if (total_quantity == 0) {
            return decision;
        }

        const auto signed_bid_quantity =
            static_cast<std::int64_t>(bid_quantity);
        const auto signed_ask_quantity =
            static_cast<std::int64_t>(ask_quantity);
        const auto signed_total_quantity =
            static_cast<std::int64_t>(total_quantity);

        decision.imbalance_bps =
            ((signed_bid_quantity - signed_ask_quantity) * 10000) /
            signed_total_quantity;

        if (decision.imbalance_bps >= config_.imbalance_threshold_bps) {
            const auto best_ask = book.best_ask();
            if (best_ask.has_value()) {
                decision.signal = Signal::buy;
                decision.limit_price_ticks = *best_ask;
                decision.quantity = config_.order_quantity;
            }
        } else if (decision.imbalance_bps <= -config_.imbalance_threshold_bps) {
            const auto best_bid = book.best_bid();
            if (best_bid.has_value()) {
                decision.signal = Signal::sell;
                decision.limit_price_ticks = *best_bid;
                decision.quantity = config_.order_quantity;
            }
        }

        return decision;
    }

    [[nodiscard]] StrategyDecision evaluate_vwap(
        const FixedL2Book& book) const noexcept {
        StrategyDecision decision{};

        std::int64_t total_value = 0;
        std::int64_t total_volume = 0;

        auto process_levels = [&](const auto& levels, int max_depth) {
            int depth = 0;
            for (const auto& level : levels) {
                if (level.active && level.quantity > 0) {
                    total_value += level.price_ticks * level.quantity;
                    total_volume += level.quantity;
                    if (++depth == max_depth) break;
                }
            }
        };

        process_levels(book.bids(), 3);
        process_levels(book.asks(), 3);

        if (total_volume == 0) return decision;

        std::int64_t vwap = total_value / total_volume;

        const auto best_bid = book.best_bid();
        const auto best_ask = book.best_ask();
        if (!best_bid || !best_ask) return decision;

        std::int64_t mid_price = (*best_bid + *best_ask) / 2;

        if (vwap < mid_price - config_.vwap_threshold_ticks) {
            // More volume at bids, pulling VWAP down
            decision.signal = Signal::buy;
            decision.limit_price_ticks = *best_ask;
            decision.quantity = config_.order_quantity;
        } else if (vwap > mid_price + config_.vwap_threshold_ticks) {
            // More volume at asks, pulling VWAP up
            decision.signal = Signal::sell;
            decision.limit_price_ticks = *best_bid;
            decision.quantity = config_.order_quantity;
        }

        return decision;
    }

    [[nodiscard]] StrategyDecision evaluate_momentum(
        const FixedL2Book& book) noexcept {
        StrategyDecision decision{};
        
        const auto best_bid = book.best_bid();
        const auto best_ask = book.best_ask();
        if (!best_bid || !best_ask) return decision;

        std::int64_t mid_price = (*best_bid + *best_ask) / 2;
        
        mid_history_.push_back(mid_price);
        if (mid_history_.size() > config_.momentum_window) {
            mid_history_.pop_front();
        }

        if (mid_history_.size() == config_.momentum_window) {
            std::int64_t rate_of_change = mid_price - mid_history_.front();
            if (rate_of_change >= config_.momentum_threshold_ticks) {
                decision.signal = Signal::buy;
                decision.limit_price_ticks = *best_ask;
                decision.quantity = config_.order_quantity;
            } else if (rate_of_change <= -config_.momentum_threshold_ticks) {
                decision.signal = Signal::sell;
                decision.limit_price_ticks = *best_bid;
                decision.quantity = config_.order_quantity;
            }
        }

        return decision;
    }

    StrategyConfig config_{};
    std::deque<std::int64_t> mid_history_{};
};

}  // namespace optitrade

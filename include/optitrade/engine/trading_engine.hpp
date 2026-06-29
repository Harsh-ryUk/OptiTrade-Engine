#pragma once

#include <cstddef>
#include <cstdint>

#include "optitrade/book/fixed_l2_book.hpp"
#include "optitrade/order/order_request.hpp"
#include "optitrade/order/preallocated_outbox.hpp"
#include "optitrade/risk/risk_guard.hpp"
#if defined(OPTITRADE_STRATEGY_MOMENTUM)
#include "optitrade/strategy/momentum.hpp"
#else
#include "optitrade/strategy/vwap_imbalance.hpp"
#endif
#include "optitrade/common/sequence_tracker.hpp"
#include "optitrade/order/pending_order_tracker.hpp"

namespace optitrade {

enum class EngineStatus : std::uint8_t {
    no_order = 0,
    order_emitted,
    invalid_update,
    risk_rejected,
    outbox_full,
    gap_detected,
};

struct EngineConfig {
    InstrumentId symbol_id{77};
    StrategyConfig strategy{};
    RiskLimits risk{};
};

struct EngineResult {
    EngineStatus status{EngineStatus::no_order};
    Signal signal{Signal::hold};
    std::int64_t imbalance_bps{};
};

#if defined(OPTITRADE_STRATEGY_MOMENTUM)
using ActiveStrategy = MomentumStrategy;
#else
using ActiveStrategy = VWAPImbalanceStrategy;
#endif

template <std::size_t OutboxCapacity = 64>
class TradingEngine {
public:
    explicit TradingEngine(const EngineConfig config = {}) noexcept
        : config_(config) {
        for (auto& s : strategies_) {
            s = ActiveStrategy(config.strategy);
        }
        for (auto& r : risks_) {
            r = RiskGuard(config.risk);
        }
    }

    [[nodiscard]] EngineResult on_market_update(
        const MarketUpdate& update) noexcept {
        if (!sequence_tracker_.check_and_update(update.symbol_id, update.sequence_number)) {
            return {
                EngineStatus::gap_detected,
                Signal::hold,
                0,
            };
        }

        const auto index = update.symbol_id % 16;

        if (!books_[index].apply(update)) {
            return {
                EngineStatus::invalid_update,
                Signal::hold,
                0,
            };
        }

        const StrategyDecision decision = strategies_[index].evaluate(books_[index]);

        if (decision.signal == Signal::hold) {
            return {
                EngineStatus::no_order,
                Signal::hold,
                decision.imbalance_bps,
            };
        }

        if (outbox_.full()) {
            return {
                EngineStatus::outbox_full,
                decision.signal,
                decision.imbalance_bps,
            };
        }

        OrderRequest order{};
        order.client_order_id = next_client_order_id_;
        order.price_ticks = decision.limit_price_ticks;
        order.quantity = decision.quantity;
        order.symbol_id = update.symbol_id;
        order.source_sequence = update.sequence_number;
        order.side =
            decision.signal == Signal::buy ? Side::buy : Side::sell;
        order.order_kind = OrderKind::limit;
        order.flags = 0;

        auto recent_opt = pending_orders_[index].find_recent_active_order(update.sequence_number);
        bool needs_cancel = false;
        OrderRequest cancel_order{};

        if (recent_opt.has_value()) {
            const auto& recent = recent_opt.value();
            if (recent.side != order.side) {
                cancel_order = recent;
                cancel_order.message_type = MessageType::cancel;
                needs_cancel = true;
                order.message_type = MessageType::new_order;
                pending_orders_[index].remove_order(recent.client_order_id);
            } else if (recent.price_ticks != order.price_ticks) {
                order.message_type = MessageType::replace;
                pending_orders_[index].remove_order(recent.client_order_id);
            } else {
                order.message_type = MessageType::new_order;
            }
        } else {
            order.message_type = MessageType::new_order;
        }

        if (needs_cancel) {
            if (!risks_[index].validate_and_commit(cancel_order) || !outbox_.push(cancel_order)) {
                return {
                    EngineStatus::outbox_full,
                    decision.signal,
                    decision.imbalance_bps,
                };
            }
        }

        if (!risks_[index].validate_and_commit(order)) {
            return {
                EngineStatus::risk_rejected,
                decision.signal,
                decision.imbalance_bps,
            };
        }

        if (!outbox_.push(order)) {
            return {
                EngineStatus::outbox_full,
                decision.signal,
                decision.imbalance_bps,
            };
        }

        pending_orders_[index].add_order(order);

        ++next_client_order_id_;

        return {
            EngineStatus::order_emitted,
            decision.signal,
            decision.imbalance_bps,
        };
    }

    [[nodiscard]] bool pop_order(OrderRequest& order) noexcept {
        return outbox_.pop(order);
    }

    [[nodiscard]] const FixedL2Book& book(const InstrumentId symbol_id) const noexcept {
        return books_[symbol_id % 16];
    }

    void acknowledge_one_order(const InstrumentId symbol_id) noexcept {
        risks_[symbol_id % 16].acknowledge_one();
    }

    [[nodiscard]] const RiskGuard& risk(const InstrumentId symbol_id) const noexcept {
        return risks_[symbol_id % 16];
    }

    [[nodiscard]] const SequenceTracker& sequence_tracker() const noexcept {
        return sequence_tracker_;
    }

private:
    EngineConfig config_{};
    std::array<FixedL2Book, 16> books_;
    std::array<ActiveStrategy, 16> strategies_;
    std::array<RiskGuard, 16> risks_;
    std::array<PendingOrderTracker, 16> pending_orders_;
    PreallocatedOutbox<OutboxCapacity> outbox_;
    SequenceTracker sequence_tracker_;
    std::uint64_t next_client_order_id_{1};
};

}  // namespace optitrade

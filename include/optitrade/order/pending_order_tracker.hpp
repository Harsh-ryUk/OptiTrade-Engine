#pragma once

#include <cstdint>
#include <optional>
#include <array>

#include "optitrade/order/order_request.hpp"
#include "optitrade/common/types.hpp"

namespace optitrade {

struct TrackedOrder {
    OrderRequest order{};
    bool active{false};
};

class PendingOrderTracker {
public:
    PendingOrderTracker() = default;

    void add_order(const OrderRequest& order) noexcept {
        size_t idx = head_ % 64;
        buffer_[idx].order = order;
        buffer_[idx].active = true;
        head_++;
    }

    [[nodiscard]] std::optional<OrderRequest> find_recent_active_order(uint32_t current_sequence) const noexcept {
        size_t count = head_ > 64 ? 64 : head_;
        for (size_t i = 1; i <= count; ++i) {
            size_t idx = (head_ - i) % 64;
            if (buffer_[idx].active) {
                const auto& order = buffer_[idx].order;
                if (current_sequence >= order.source_sequence && 
                    (current_sequence - order.source_sequence) <= 4) {
                    return order;
                }
            }
        }
        return std::nullopt;
    }

    void remove_order(uint64_t client_order_id) noexcept {
        size_t count = head_ > 64 ? 64 : head_;
        for (size_t i = 1; i <= count; ++i) {
            size_t idx = (head_ - i) % 64;
            if (buffer_[idx].active && buffer_[idx].order.client_order_id == client_order_id) {
                buffer_[idx].active = false;
                break;
            }
        }
    }
    
    // For compatibility with earlier simplistic interface, used internally if needed
    [[nodiscard]] std::optional<OrderRequest> get_pending() const noexcept {
        if (head_ > 0) {
            size_t idx = (head_ - 1) % 64;
            if (buffer_[idx].active) {
                return buffer_[idx].order;
            }
        }
        return std::nullopt;
    }

    void replace_order(const OrderRequest& order) noexcept {
        add_order(order); // Just add the replaced one to the ring buffer, we'll track it
    }

private:
    std::array<TrackedOrder, 64> buffer_{};
    size_t head_{0};
};

}  // namespace optitrade

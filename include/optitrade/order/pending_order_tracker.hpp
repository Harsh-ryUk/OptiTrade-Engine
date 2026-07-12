// Author: Harsh
#pragma once

#include <cstdint>
#include <optional>
#include <array>

#include "optitrade/order/order_request.hpp"
#include "optitrade/common/types.hpp"
#include "../../order_book.hpp"

namespace optitrade {

struct TrackedRestingOrder {
    OrderRequest request{};
    Order* book_order{nullptr};
    bool active{false};
};

class PendingOrderTracker {
public:
    PendingOrderTracker() = default;

    void add_order(const OrderRequest& order, Order* book_order) noexcept {
        size_t idx = head_ % 64;
        buffer_[idx].request = order;
        buffer_[idx].book_order = book_order;
        buffer_[idx].active = true;
        head_++;
    }

    [[nodiscard]] TrackedRestingOrder* find_recent_active_order(uint32_t current_sequence) noexcept {
        size_t count = head_ > 64 ? 64 : head_;
        for (size_t i = 1; i <= count; ++i) {
            size_t idx = (head_ - i) % 64;
            if (buffer_[idx].active) {
                const auto& order = buffer_[idx].request;
                if (current_sequence >= order.source_sequence && 
                    (current_sequence - order.source_sequence) <= 4) {
                    return &buffer_[idx];
                }
            }
        }
        return nullptr;
    }

    [[nodiscard]] TrackedRestingOrder* get_pending_by_id(uint64_t client_order_id) noexcept {
        size_t count = head_ > 64 ? 64 : head_;
        for (size_t i = 1; i <= count; ++i) {
            size_t idx = (head_ - i) % 64;
            if (buffer_[idx].active && buffer_[idx].request.client_order_id == client_order_id) {
                return &buffer_[idx];
            }
        }
        return nullptr;
    }

    void remove_order(uint64_t client_order_id) noexcept {
        TrackedRestingOrder* tracked = get_pending_by_id(client_order_id);
        if (tracked) {
            tracked->active = false;
        }
    }

private:
    std::array<TrackedRestingOrder, 64> buffer_{};
    size_t head_{0};
};

}  // namespace optitrade

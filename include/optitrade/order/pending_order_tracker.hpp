#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "optitrade/order/order_request.hpp"

namespace optitrade {

class PendingOrderTracker {
public:
    PendingOrderTracker() = default;

    void add_order(const OrderRequest& order) noexcept {
        pending_order_ = order;
    }

    [[nodiscard]] std::optional<OrderRequest> get_pending() const noexcept {
        return pending_order_;
    }

    void remove_order() noexcept {
        pending_order_.reset();
    }

    void replace_order(const OrderRequest& order) noexcept {
        pending_order_ = order;
    }

private:
    std::optional<OrderRequest> pending_order_{};
};

}  // namespace optitrade

#pragma once

#include <cstdint>

#include "optitrade/core/types.hpp"

namespace optitrade::oms {

// Stable handle for one logical order. Assigned by OrderManager::submit(), never
// reused. Survives cancel/replace (the exchange-side token changes on replace,
// the OrderId does not).
using OrderId = std::uint64_t;

enum class Tif : std::uint8_t { day, ioc };

struct OrderRequest {
    Locate locate{};
    Side side{Side::buy};
    Price price{};
    Qty qty{};
    Tif tif{Tif::day};
};

enum class OrderStatus : std::uint8_t {
    pending_new,      // sent, not yet acknowledged
    live,             // acknowledged and resting (possibly partially filled)
    pending_cancel,   // cancel sent
    pending_replace,  // replace sent
    filled,           // terminal
    canceled,         // terminal
    rejected,         // terminal
};

constexpr bool is_terminal(OrderStatus s) noexcept {
    return s == OrderStatus::filled || s == OrderStatus::canceled || s == OrderStatus::rejected;
}

struct OrderInfo {
    OrderId id{};
    OrderRequest req{};          // current terms (updated by an acknowledged replace)
    OrderStatus status{OrderStatus::pending_new};
    Qty cum_qty{};               // shares executed so far
    Qty leaves_qty{};            // shares still open (0 once terminal)
    Price avg_price{};           // volume weighted execution price, 0 if no fills
    OrderRef exchange_ref{};     // reference number from the Accepted message, 0 before
    Nanos last_update{};
    char reason{};               // reject / cancel reason code from the exchange, 0 if none
};

struct Fill {
    OrderId id{};
    Locate locate{};
    Side side{Side::buy};
    Qty qty{};
    Price price{};
    Nanos ts{};
    std::uint64_t match{};
};

enum class SubmitStatus : std::uint8_t {
    ok,
    rejected_by_risk,  // see SubmitResult::risk_reason
    invalid_request,   // zero qty, non-positive price, unknown locate...
    unknown_order,
    bad_state,         // e.g. cancel of a terminal order, replace while pending
    capacity,          // order table full
    gateway_busy,      // OrderGateway::send returned false
};

}  // namespace optitrade::oms

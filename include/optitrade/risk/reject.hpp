#pragma once

#include <cstdint>

namespace optitrade::risk {

enum class Reject : std::uint8_t {
    none = 0,
    kill_switch,
    invalid_order,      // zero qty / non-positive price
    order_qty,          // larger than max_order_qty
    order_notional,     // price * qty larger than max_order_notional
    position,           // would exceed max_position for the instrument
    gross_position,     // would exceed max_gross_position across instruments
    price_band,         // too far from the reference price (fat finger)
    no_reference,       // price band required but no reference price available
    rate_limit,         // more than max_orders_per_second
    open_orders,        // too many open orders
};

constexpr const char* to_string(Reject r) noexcept {
    switch (r) {
        case Reject::none: return "none";
        case Reject::kill_switch: return "kill_switch";
        case Reject::invalid_order: return "invalid_order";
        case Reject::order_qty: return "order_qty";
        case Reject::order_notional: return "order_notional";
        case Reject::position: return "position";
        case Reject::gross_position: return "gross_position";
        case Reject::price_band: return "price_band";
        case Reject::no_reference: return "no_reference";
        case Reject::rate_limit: return "rate_limit";
        case Reject::open_orders: return "open_orders";
    }
    return "?";
}

}  // namespace optitrade::risk

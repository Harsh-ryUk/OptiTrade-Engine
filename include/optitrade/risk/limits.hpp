#pragma once

#include <cstdint>

#include "optitrade/core/types.hpp"

namespace optitrade::risk {

// Pre-trade limits. A limit documented as "0 = disabled" is skipped entirely when zero.
// The two hard caps (max_order_qty, max_position) have no such escape hatch on purpose: a
// zero there means "nothing is allowed", which is the safe reading of a mis-initialised
// configuration.
struct Limits {
    Qty max_order_qty{1000};
    // price * qty in price units (1e-4 currency). 0 = disabled.
    std::int64_t max_order_notional{0};
    // Absolute shares per instrument, worst case including open orders on the order's side.
    // An order that moves an over-limit instrument back toward the limit is not blocked by it.
    std::int64_t max_position{5000};
    // Sum over instruments of the worst-case absolute position including open orders
    // (see RiskEngine for the exact definition). 0 = disabled.
    std::int64_t max_gross_position{0};
    // Largest allowed |price - reference| in basis points of the reference. 0 = band disabled.
    std::uint32_t price_band_bps{0};
    // With the band enabled, reject orders that arrive without a usable reference price
    // instead of letting them through unchecked.
    bool require_reference{false};
    // Orders admitted in any sliding one-second window. 0 = disabled.
    // The window is a ring of admission timestamps, so it costs 8 bytes per permitted order.
    // Above RiskEngine::kMaxRateRing (1 Mi orders per second) the limit is treated as disabled
    // rather than allocating a ring for it.
    std::uint32_t max_orders_per_second{0};
    // Working orders across all instruments. 0 = disabled.
    std::uint32_t max_open_orders{0};
    // Trips the kill switch once realized + unrealized PnL <= -max_loss. 0 (or negative) = disabled.
    std::int64_t max_loss{0};
};

}  // namespace optitrade::risk

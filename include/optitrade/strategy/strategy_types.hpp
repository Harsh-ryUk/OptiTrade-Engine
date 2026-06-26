#pragma once

#include <cstdint>
#include "optitrade/common/types.hpp"

namespace optitrade {

struct StrategyConfig {
    std::int64_t imbalance_threshold_bps{6000};
    std::uint32_t order_quantity{100};
};

struct StrategyDecision {
    Signal signal{Signal::hold};
    std::int64_t limit_price_ticks{};
    std::uint32_t quantity{};
    std::int64_t imbalance_bps{};
};

} // namespace optitrade

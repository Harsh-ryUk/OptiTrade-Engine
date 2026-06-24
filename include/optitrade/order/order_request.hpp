#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "optitrade/common/types.hpp"

namespace optitrade {

enum class MessageType : std::uint8_t {
    new_order = 1,
    cancel = 2,
    replace = 3,
};

enum class OrderKind : std::uint8_t {
    limit = 1,
};

struct alignas(8) OrderRequest {
    std::uint64_t client_order_id{};
    PriceTicks price_ticks{};
    Quantity quantity{};
    InstrumentId symbol_id{};
    SequenceNumber source_sequence{};
    Side side{Side::buy};
    OrderKind order_kind{OrderKind::limit};
    MessageType message_type{MessageType::new_order};
    std::uint8_t padding{};
    std::uint16_t flags{};
};

static_assert(sizeof(OrderRequest) == 40);
static_assert(std::is_trivially_copyable_v<OrderRequest>);

}  // namespace optitrade

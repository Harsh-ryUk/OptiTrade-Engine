#pragma once

#include <cstdint>

#include "optitrade/common/types.hpp"
#include "optitrade/order/order_request.hpp"
#include "optitrade/wire/market_data_packet.hpp"
#include "optitrade/wire/order_packet.hpp"

namespace optitrade::dpdk {

class EngineWireAdapter {
public:
    [[nodiscard]] static bool to_market_update(
        const wire::MarketDataMessage& input,
        MarketUpdate& output) noexcept {
        Side side{};

        switch (input.side) {
            case wire::Side::buy:
                side = Side::buy;
                break;
            case wire::Side::sell:
                side = Side::sell;
                break;
            default:
                return false;
        }

        UpdateAction action{};

        switch (input.action) {
            case wire::UpdateAction::add:
                action = UpdateAction::add;
                break;
            case wire::UpdateAction::modify:
                action = UpdateAction::modify;
                break;
            case wire::UpdateAction::erase:
                action = UpdateAction::erase;
                break;
            default:
                return false;
        }

        output.sequence_number = input.sequence_number;
        output.exchange_timestamp_ns = input.exchange_timestamp_ns;
        output.symbol_id = input.symbol_id;
        output.price_ticks = input.price_ticks;
        output.quantity = input.quantity;
        output.side = side;
        output.action = action;
        output.level = input.level;
        output.source_flags = input.source_flags;
        output.sequence_num = input.sequence_num;

        return true;
    }

    [[nodiscard]] static bool to_wire_order(
        const OrderRequest& input,
        const std::uint32_t output_sequence_number,
        wire::OutboundOrderMessage& output) noexcept {
        wire::Side side{};

        switch (input.side) {
            case Side::buy:
                side = wire::Side::buy;
                break;
            case Side::sell:
                side = wire::Side::sell;
                break;
            default:
                return false;
        }

        if (input.order_kind != OrderKind::limit) {
            return false;
        }

        output.sequence_number = output_sequence_number;
        output.wire_flags = 0;
        output.client_order_id = input.client_order_id;
        output.symbol_id = input.symbol_id;
        output.price_ticks = input.price_ticks;
        output.quantity = input.quantity;
        output.side = side;
        switch (input.message_type) {
            case MessageType::cancel:
                output.order_type = wire::OrderType::cancel;
                break;
            case MessageType::replace:
                output.order_type = wire::OrderType::replace;
                break;
            case MessageType::new_order:
            default:
                output.order_type = wire::OrderType::limit;
                break;
        }
        output.time_in_force = wire::TimeInForce::immediate_or_cancel;
        output.order_flags =
            static_cast<std::uint8_t>(input.flags & 0xFFU);
        output.reserved = input.source_sequence;

        return true;
    }
};

}  // namespace optitrade::dpdk

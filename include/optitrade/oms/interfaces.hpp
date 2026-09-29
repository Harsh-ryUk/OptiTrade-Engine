#pragma once

#include <cstddef>

#include "optitrade/core/types.hpp"
#include "optitrade/oms/types.hpp"
#include "optitrade/risk/reject.hpp"

namespace optitrade::ouch {
struct EnterOrder;
struct CancelOrder;
struct ReplaceOrder;
}  // namespace optitrade::ouch

namespace optitrade::oms {

struct SubmitResult {
    SubmitStatus status{SubmitStatus::ok};
    OrderId id{};
    risk::Reject risk_reason{risk::Reject::none};
};

// Where outbound order-entry messages go: a real session, or the simulator.
// `now` is the caller's clock, so a simulated gateway can model latency without
// reading the wall clock. Returns false if the message could not be accepted.
class OrderGateway {
public:
    virtual ~OrderGateway() = default;
    virtual bool send(const ouch::EnterOrder& msg, Nanos now) = 0;
    virtual bool send(const ouch::CancelOrder& msg, Nanos now) = 0;
    virtual bool send(const ouch::ReplaceOrder& msg, Nanos now) = 0;
};

// What a strategy may do. Implemented by OrderManager.
class OrderApi {
public:
    virtual ~OrderApi() = default;
    virtual SubmitResult submit(const OrderRequest& req, Nanos now) = 0;
    virtual SubmitStatus cancel(OrderId id, Nanos now) = 0;
    // `new_qty` is the total intended size (executed + still open), as in OUCH.
    virtual SubmitStatus replace(OrderId id, Price new_price, Qty new_qty, Nanos now) = 0;
    virtual const OrderInfo* find(OrderId id) const = 0;
    virtual std::size_t open_orders() const = 0;
};

// Receives order lifecycle events from OrderManager.
class OrderListener {
public:
    virtual ~OrderListener() = default;
    virtual void on_fill(const Fill& fill) = 0;
    virtual void on_order_update(const OrderInfo& info) = 0;
};

// Price used by pre-trade risk checks (fat-finger band). 0 means "none available".
class ReferenceSource {
public:
    virtual ~ReferenceSource() = default;
    virtual Price reference_price(Locate locate) const = 0;
};

}  // namespace optitrade::oms

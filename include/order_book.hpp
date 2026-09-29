#pragma once

#include <cstdint>
#include <array>
#include <cstddef>

#include "optitrade/common/types.hpp"

namespace optitrade {

using OrderId = std::uint64_t;

// Cache-line aligned Order for intrusive linked list
struct alignas(64) Order {
    Order* next{nullptr};
    Order* prev{nullptr};
    
    OrderId id{};
    PriceTicks price{};
    Quantity quantity{};
    Side side{Side::buy};
    bool active{false};
};

// Fixed-size memory pool for Order objects to avoid dynamic allocations in hot path
template <std::size_t MaxOrders>
class OrderPool {
public:
    OrderPool() noexcept {
        // Initialize free list
        for (std::size_t i = 0; i < MaxOrders - 1; ++i) {
            orders_[i].next = &orders_[i + 1];
        }
        orders_[MaxOrders - 1].next = nullptr;
        free_head_ = &orders_[0];
    }

    // Allocate an Order from the pool in O(1)
    [[nodiscard]] Order* allocate() noexcept {
        if (!free_head_) [[unlikely]] {
            return nullptr;
        }
        Order* order = free_head_;
        free_head_ = free_head_->next;
        
        order->next = nullptr;
        order->prev = nullptr;
        order->active = true;
        
        return order;
    }

    // Return an Order to the pool in O(1)
    void deallocate(Order* order) noexcept {
        if (!order) return;
        order->active = false;
        order->next = free_head_;
        order->prev = nullptr;
        free_head_ = order;
    }

private:
    std::array<Order, MaxOrders> orders_{};
    Order* free_head_{nullptr};
};

// Intrusive doubly-linked list for a price level
class PriceLevel {
public:
    PriceLevel() = default;

    // Insert order at the end of the queue (time priority) O(1)
    void add_order(Order* order) noexcept {
        if (!head_) {
            head_ = order;
            tail_ = order;
            order->next = nullptr;
            order->prev = nullptr;
        } else {
            tail_->next = order;
            order->prev = tail_;
            order->next = nullptr;
            tail_ = order;
        }
        total_quantity_ += order->quantity;
    }

    // Remove order from anywhere in the queue O(1)
    void remove_order(Order* order) noexcept {
        if (order->prev) {
            order->prev->next = order->next;
        } else {
            head_ = order->next;
        }

        if (order->next) {
            order->next->prev = order->prev;
        } else {
            tail_ = order->prev;
        }

        total_quantity_ -= order->quantity;
        order->next = nullptr;
        order->prev = nullptr;
    }
    
    [[nodiscard]] Order* head() const noexcept { return head_; }
    [[nodiscard]] Quantity total_quantity() const noexcept { return total_quantity_; }
    [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }

private:
    Order* head_{nullptr};
    Order* tail_{nullptr};
    Quantity total_quantity_{0};
};

// The Order Book matching engine core using intrusive lists and fixed pool
template <std::size_t MaxLevels, std::size_t MaxOrders>
class OrderBook {
public:
    OrderBook() = default;

    // O(1) Order insertion
    [[nodiscard]] Order* add_order(OrderId id, Side side, PriceTicks price, Quantity quantity) noexcept {
        Order* order = pool_.allocate();
        if (!order) [[unlikely]] return nullptr;
        
        order->id = id;
        order->side = side;
        order->price = price;
        order->quantity = quantity;
        
        std::size_t level_idx = get_level_index(price);
        
        if (side == Side::buy) {
            bids_[level_idx].add_order(order);
        } else {
            asks_[level_idx].add_order(order);
        }
        
        return order;
    }

    // O(1) Order cancellation
    void cancel_order(Order* order) noexcept {
        if (!order || !order->active) return;
        
        std::size_t level_idx = get_level_index(order->price);
        
        if (order->side == Side::buy) {
            bids_[level_idx].remove_order(order);
        } else {
            asks_[level_idx].remove_order(order);
        }
        
        pool_.deallocate(order);
    }
    
    [[nodiscard]] PriceLevel& get_bid_level(PriceTicks price) noexcept {
        return bids_[get_level_index(price)];
    }
    
    [[nodiscard]] PriceLevel& get_ask_level(PriceTicks price) noexcept {
        return asks_[get_level_index(price)];
    }

private:
    // Simple fast hashing for demo bounds
    [[nodiscard]] inline std::size_t get_level_index(PriceTicks price) const noexcept {
        return static_cast<std::size_t>(static_cast<std::uint64_t>(price) % MaxLevels);
    }

    OrderPool<MaxOrders> pool_{};
    std::array<PriceLevel, MaxLevels> bids_{};
    std::array<PriceLevel, MaxLevels> asks_{};
};

} // namespace optitrade

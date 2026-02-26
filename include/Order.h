#pragma once
#include "Types.h"

// ─── Resting order ────────────────────────────────────────────────────────────
struct Order {
    OrderId     id{0};
    std::string symbol;
    Side        side{Side::BUY};
    OrderType   type{OrderType::LIMIT};
    Price       price{0};
    Price       limit_price{0};   // STOP_LIMIT only: limit price of triggered order
    Qty         qty_original{0};
    Qty         qty_remaining{0};
    Timestamp   timestamp{0};
    OrderStatus status{OrderStatus::ACTIVE};
};

// ─── Inbound event (parsed from CSV / network message) ────────────────────────
struct OrderEvent {
    Timestamp   timestamp{0};
    std::string symbol;
    OrderType   type{OrderType::LIMIT};
    OrderId     order_id{0};
    Side        side{Side::BUY};
    Price       price{0};         // ticks; for stops: the stop/trigger price
    Qty         qty{0};           // ignored for CANCEL
    Price       limit_price{0};   // STOP_LIMIT only: price of triggered limit order

    // REPLACE-only fields
    OrderId     new_order_id{0};
    Price       new_price{0};
    Qty         new_qty{0};
};

#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ─── Primitive aliases ────────────────────────────────────────────────────────
// Price is stored as integer ticks: 1 tick = $0.01, so $150.25 → 15025
using OrderId   = uint64_t;
using Price     = int64_t;
using Qty       = uint64_t;
using Timestamp = uint64_t;   // nanoseconds since epoch (or arbitrary monotonic)

// ─── Enumerations ─────────────────────────────────────────────────────────────
enum class Side : uint8_t { BUY = 0, SELL = 1 };

enum class OrderType : uint8_t {
    LIMIT        = 0,
    MARKET       = 1,
    CANCEL       = 2,
    REPLACE      = 3,
    STOP_MARKET  = 4,   // triggers a market order when last_trade_price crosses stop_price
    STOP_LIMIT   = 5    // triggers a limit order  when last_trade_price crosses stop_price
};

enum class OrderStatus : uint8_t {
    ACTIVE,
    PARTIALLY_FILLED,
    FILLED,
    CANCELLED
};

// ─── Output structures ────────────────────────────────────────────────────────
struct Trade {
    Timestamp   timestamp{0};
    std::string symbol;
    Price       price{0};
    Qty         qty{0};
    Side        aggressor_side{Side::BUY};
    OrderId     resting_order_id{0};
    OrderId     aggressing_order_id{0};
};

struct TOBUpdate {
    Timestamp   timestamp{0};
    std::string symbol;
    Price       best_bid_price{0};   // 0 if no bids
    Qty         best_bid_qty{0};
    Price       best_ask_price{0};   // 0 if no asks
    Qty         best_ask_qty{0};
};

// Returned from every OrderBook mutation
struct MatchResult {
    std::vector<Trade> trades;
    bool               tob_changed{false};
};

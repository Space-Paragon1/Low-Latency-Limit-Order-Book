#pragma once
#include "Types.h"
#include "Order.h"
#include <map>
#include <deque>
#include <unordered_map>
#include <string>
#include <functional>

// ─── Per-symbol order book ─────────────────────────────────────────────────────
// Bids: highest price first  (std::greater<Price>)
// Asks: lowest  price first  (default ascending)
// Each price level owns a FIFO deque of order IDs (price-time priority).
// The orders_ map is the authoritative store; price-level deques hold live IDs.
class OrderBook {
public:
    explicit OrderBook(std::string symbol);

    // ── Mutations ─────────────────────────────────────────────────────────────
    MatchResult addLimit (const OrderEvent& event);
    MatchResult addMarket(const OrderEvent& event);
    bool        cancel   (OrderId id, Timestamp ts);   // false if not found/dead
    MatchResult replace  (const OrderEvent& event);    // cancel + new limit

    // ── Top-of-book queries ───────────────────────────────────────────────────
    bool      hasBid()        const { return !bids_.empty(); }
    bool      hasAsk()        const { return !asks_.empty(); }
    Price     bestBidPrice()  const;
    Qty       bestBidQty()    const;   // aggregate qty at best bid level
    Price     bestAskPrice()  const;
    Qty       bestAskQty()    const;   // aggregate qty at best ask level
    TOBUpdate makeTOB(Timestamp ts) const;

    // ── Invariant check ───────────────────────────────────────────────────────
    // Returns true if the book is in a crossed state (should never happen
    // after a complete match cycle).
    bool isCrossed() const;

    // ── Inspection (for tests) ────────────────────────────────────────────────
    const std::unordered_map<OrderId, Order>& orders() const { return orders_; }
    std::size_t bidLevels() const { return bids_.size(); }
    std::size_t askLevels() const { return asks_.size(); }

private:
    // Bid side: highest price → front
    using BidLevels = std::map<Price, std::deque<OrderId>, std::greater<Price>>;
    // Ask side: lowest  price → front
    using AskLevels = std::map<Price, std::deque<OrderId>>;

    std::string symbol_;
    BidLevels   bids_;
    AskLevels   asks_;
    std::unordered_map<OrderId, Order> orders_;   // order_id → Order

    // Add an order that already lives in orders_ to its price-level deque.
    void addToLevel(OrderId id);

    // Remove an order ID from its price-level deque; erases the level if empty.
    void removeFromLevel(OrderId id, Side side, Price price);

    // Core matching loops (aggressor is modified in-place via reference).
    std::vector<Trade> matchBuy (Order& aggressor, Timestamp ts);
    std::vector<Trade> matchSell(Order& aggressor, Timestamp ts);

    // Snapshot TOB state before/after a mutation to detect changes.
    struct TOBSnapshot { Price bp, ap; Qty bq, aq; };
    TOBSnapshot snapshot() const;
    bool snapshotChanged(const TOBSnapshot& before) const;
};

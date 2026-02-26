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
    MatchResult addStop  (const OrderEvent& event);    // register a stop order

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
    // ── Stop order record (pending trigger) ───────────────────────────────────
    // Stops never rest in bids_/asks_; they live in the stop queues below.
    struct StopEntry {
        OrderId   id;
        OrderType type;         // STOP_MARKET or STOP_LIMIT
        Side      side;
        Price     stop_price;   // trigger threshold
        Price     limit_price;  // STOP_LIMIT only; 0 for STOP_MARKET
        Qty       qty;
        Timestamp timestamp;
    };

    // Bid side: highest price → front
    using BidLevels = std::map<Price, std::deque<OrderId>, std::greater<Price>>;
    // Ask side: lowest  price → front
    using AskLevels = std::map<Price, std::deque<OrderId>>;
    // Buy stop levels: ascending — cheapest stop fires first on upward price move
    using BuyStopLevels  = std::map<Price, std::deque<StopEntry>>;
    // Sell stop levels: descending — most expensive stop fires first on downward price move
    using SellStopLevels = std::map<Price, std::deque<StopEntry>, std::greater<Price>>;

    std::string symbol_;
    BidLevels   bids_;
    AskLevels   asks_;
    std::unordered_map<OrderId, Order> orders_;   // order_id → Order

    Price         last_trade_price_{0};   // updated on every fill; 0 = no trades yet
    BuyStopLevels  buy_stops_;            // buy stops:  fire when last_trade >= stop_price
    SellStopLevels sell_stops_;           // sell stops: fire when last_trade <= stop_price

    // Add an order that already lives in orders_ to its price-level deque.
    void addToLevel(OrderId id);

    // Remove an order ID from its price-level deque; erases the level if empty.
    void removeFromLevel(OrderId id, Side side, Price price);

    // Core matching loops (aggressor is modified in-place via reference).
    std::vector<Trade> matchBuy (Order& aggressor, Timestamp ts);
    std::vector<Trade> matchSell(Order& aggressor, Timestamp ts);

    // Pure matching helpers (no stop-trigger check); called by public wrappers
    // and by executeStop to avoid re-entrant trigger checking.
    MatchResult addLimitInternal (const OrderEvent& event);
    MatchResult addMarketInternal(const OrderEvent& event);

    // Stop trigger helpers
    bool isStopTriggered(const StopEntry& entry) const;
    void executeStop(const StopEntry& entry, MatchResult& accumulated, Timestamp ts);
    void checkAndTriggerStops(MatchResult& accumulated, Timestamp ts);

    // Snapshot TOB state before/after a mutation to detect changes.
    struct TOBSnapshot { Price bp, ap; Qty bq, aq; };
    TOBSnapshot snapshot() const;
    bool snapshotChanged(const TOBSnapshot& before) const;
};

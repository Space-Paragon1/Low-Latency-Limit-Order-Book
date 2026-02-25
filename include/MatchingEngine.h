#pragma once
#include "Types.h"
#include "Order.h"
#include "OrderBook.h"
#include <unordered_map>
#include <string>
#include <functional>

// ─── Multi-symbol matching engine ─────────────────────────────────────────────
// Owns one OrderBook per symbol.  Callers register callbacks that are fired
// synchronously inside processEvent().
class MatchingEngine {
public:
    using TradeHandler = std::function<void(const Trade&)>;
    using TOBHandler   = std::function<void(const TOBUpdate&)>;

    void setTradeHandler(TradeHandler h) { trade_handler_ = std::move(h); }
    void setTOBHandler  (TOBHandler   h) { tob_handler_   = std::move(h); }

    // Process one inbound event; fires registered callbacks as needed.
    void processEvent(const OrderEvent& event);

    // Direct book access (for tests / L2 snapshots).
    OrderBook* getBook(const std::string& symbol);

private:
    std::unordered_map<std::string, OrderBook> books_;
    TradeHandler trade_handler_;
    TOBHandler   tob_handler_;

    OrderBook& getOrCreateBook(const std::string& symbol);
    void       publish(const MatchResult& result, const std::string& symbol,
                       Timestamp ts);
};

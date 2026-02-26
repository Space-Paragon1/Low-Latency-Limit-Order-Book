#include "MatchingEngine.h"

// ─────────────────────────────────────────────────────────────────────────────
OrderBook& MatchingEngine::getOrCreateBook(const std::string& symbol) {
    auto it = books_.find(symbol);
    if (it != books_.end()) return it->second;
    // emplace returns pair<iterator,bool>; use piecewise to avoid copy.
    auto [ins, _] = books_.emplace(symbol, OrderBook(symbol));
    return ins->second;
}

OrderBook* MatchingEngine::getBook(const std::string& symbol) {
    auto it = books_.find(symbol);
    return (it != books_.end()) ? &it->second : nullptr;
}

// ─── Publish callbacks ────────────────────────────────────────────────────────
void MatchingEngine::publish(const MatchResult& result,
                             const std::string& symbol,
                             Timestamp ts) {
    if (trade_handler_) {
        for (const Trade& t : result.trades)
            trade_handler_(t);
    }

    if (tob_handler_ && result.tob_changed) {
        OrderBook* book = getBook(symbol);
        if (book) tob_handler_(book->makeTOB(ts));
    }
}

// ─── Main dispatch ────────────────────────────────────────────────────────────
void MatchingEngine::processEvent(const OrderEvent& event) {
    OrderBook& book = getOrCreateBook(event.symbol);
    MatchResult result;

    switch (event.type) {
        case OrderType::LIMIT:
            result = book.addLimit(event);
            break;

        case OrderType::MARKET:
            result = book.addMarket(event);
            break;

        case OrderType::CANCEL: {
            bool cancelled = book.cancel(event.order_id, event.timestamp);
            if (cancelled) {
                result.tob_changed = true;   // conservative; publisher may skip
            }
            break;
        }

        case OrderType::REPLACE:
            result = book.replace(event);
            break;

        case OrderType::STOP_MARKET:   // fall-through: both routed to addStop
        case OrderType::STOP_LIMIT:
            result = book.addStop(event);
            break;
    }

    publish(result, event.symbol, event.timestamp);
}

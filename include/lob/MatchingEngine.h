#pragma once

#include "lob/OrderBook.h"
#include "lob/OrderEvent.h"
#include "lob/Types.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>

namespace lob {

// ─── Multi-symbol matching engine ─────────────────────────────────────────────
// Routes each event to its symbol's OrderBook (created on first order) and
// fires the registered callbacks synchronously, in order, before returning:
// every trade first, then at most one TOB update if the top of book changed.
class MatchingEngine {
public:
    using TradeHandler = std::function<void(const Trade&)>;
    using TOBHandler   = std::function<void(const TOBUpdate&)>;

    // `expected_orders_per_book` pre-sizes each new book's pools and id map so
    // the hot path never grows them.
    explicit MatchingEngine(std::size_t expected_orders_per_book = 1024)
        : expected_orders_(expected_orders_per_book) {}

    void setTradeHandler(TradeHandler h) { trade_handler_ = std::move(h); }
    void setTOBHandler  (TOBHandler   h) { tob_handler_   = std::move(h); }

    // Processes one event.  A CANCEL or REPLACE for a symbol with no book is
    // rejected with UNKNOWN_ORDER.  The returned trades view is valid until
    // the next event for the same symbol.
    MatchResult processEvent(const OrderEvent& event);

    [[nodiscard]] OrderBook*       getBook(const Symbol& symbol) noexcept;
    [[nodiscard]] const OrderBook* getBook(const Symbol& symbol) const noexcept;
    [[nodiscard]] OrderBook*       getBook(std::string_view symbol) noexcept;   // nullptr if over-long
    [[nodiscard]] OrderBook*       getBook(const char* symbol) noexcept {
        return getBook(std::string_view(symbol));
    }
    OrderBook&                     getOrCreateBook(const Symbol& symbol);
    [[nodiscard]] std::size_t      bookCount() const noexcept { return books_.size(); }

private:
    // unique_ptr keeps each book at a stable address across rehashes.
    std::unordered_map<Symbol, std::unique_ptr<OrderBook>, SymbolHash> books_;
    std::size_t  expected_orders_;
    Symbol       cached_symbol_;            // one-entry cache: consecutive events
    OrderBook*   cached_book_{nullptr};     // usually hit the same book
    TradeHandler trade_handler_;
    TOBHandler   tob_handler_;
};

}  // namespace lob

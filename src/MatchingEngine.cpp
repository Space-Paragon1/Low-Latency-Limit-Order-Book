#include "lob/MatchingEngine.h"

namespace lob {

OrderBook* MatchingEngine::getBook(const Symbol& symbol) noexcept {
    if (cached_book_ != nullptr && cached_symbol_ == symbol) return cached_book_;
    const auto it = books_.find(symbol);
    if (it == books_.end()) return nullptr;
    cached_symbol_ = symbol;
    cached_book_   = it->second.get();
    return cached_book_;
}

const OrderBook* MatchingEngine::getBook(const Symbol& symbol) const noexcept {
    const auto it = books_.find(symbol);
    return it == books_.end() ? nullptr : it->second.get();
}

OrderBook* MatchingEngine::getBook(std::string_view symbol) noexcept {
    Symbol s;
    return Symbol::tryParse(symbol, s) ? getBook(s) : nullptr;
}

OrderBook& MatchingEngine::getOrCreateBook(const Symbol& symbol) {
    if (OrderBook* book = getBook(symbol)) return *book;
    auto [it, inserted] =
        books_.emplace(symbol, std::make_unique<OrderBook>(symbol, expected_orders_));
    (void)inserted;
    cached_symbol_ = symbol;
    cached_book_   = it->second.get();
    return *cached_book_;
}

MatchResult MatchingEngine::processEvent(const OrderEvent& event) {
    OrderBook* book = getBook(event.symbol);
    if (book == nullptr) {
        if (event.type == OrderType::CANCEL || event.type == OrderType::REPLACE) {
            MatchResult r;
            r.status        = OrderStatus::REJECTED;
            r.reject_reason = RejectReason::UNKNOWN_ORDER;
            return r;
        }
        book = &getOrCreateBook(event.symbol);
    }

    const MatchResult result = book->apply(event);

    if (trade_handler_)
        for (const Trade& t : result.trades) trade_handler_(t);
    if (result.tob_changed && tob_handler_) tob_handler_(book->makeTOB(event.timestamp));
    return result;
}

}  // namespace lob

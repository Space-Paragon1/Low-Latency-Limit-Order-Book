#include "OrderBook.h"
#include <algorithm>
#include <stdexcept>
#include <climits>

// ─────────────────────────────────────────────────────────────────────────────
OrderBook::OrderBook(std::string symbol) : symbol_(std::move(symbol)) {}

// ─── TOB helpers ──────────────────────────────────────────────────────────────
Price OrderBook::bestBidPrice() const {
    return bids_.empty() ? 0 : bids_.begin()->first;
}

Price OrderBook::bestAskPrice() const {
    return asks_.empty() ? 0 : asks_.begin()->first;
}

// Sum all qty_remaining for orders at the best price level.
Qty OrderBook::bestBidQty() const {
    if (bids_.empty()) return 0;
    Qty total = 0;
    for (OrderId id : bids_.begin()->second) {
        auto it = orders_.find(id);
        if (it != orders_.end()) total += it->second.qty_remaining;
    }
    return total;
}

Qty OrderBook::bestAskQty() const {
    if (asks_.empty()) return 0;
    Qty total = 0;
    for (OrderId id : asks_.begin()->second) {
        auto it = orders_.find(id);
        if (it != orders_.end()) total += it->second.qty_remaining;
    }
    return total;
}

TOBUpdate OrderBook::makeTOB(Timestamp ts) const {
    return TOBUpdate{ts, symbol_,
                     bestBidPrice(), bestBidQty(),
                     bestAskPrice(), bestAskQty()};
}

bool OrderBook::isCrossed() const {
    if (bids_.empty() || asks_.empty()) return false;
    return bids_.begin()->first >= asks_.begin()->first;
}

// ─── Snapshot helpers (before/after mutation) ─────────────────────────────────
OrderBook::TOBSnapshot OrderBook::snapshot() const {
    return {bestBidPrice(), bestAskPrice(), bestBidQty(), bestAskQty()};
}

bool OrderBook::snapshotChanged(const TOBSnapshot& before) const {
    TOBSnapshot after = snapshot();
    return (before.bp != after.bp || before.bq != after.bq ||
            before.ap != after.ap || before.aq != after.aq);
}

// ─── Level management ─────────────────────────────────────────────────────────
void OrderBook::addToLevel(OrderId id) {
    const Order& o = orders_.at(id);
    if (o.side == Side::BUY)
        bids_[o.price].push_back(id);
    else
        asks_[o.price].push_back(id);
}

void OrderBook::removeFromLevel(OrderId id, Side side, Price price) {
    if (side == Side::BUY) {
        auto it = bids_.find(price);
        if (it == bids_.end()) return;
        auto& q = it->second;
        auto  pos = std::find(q.begin(), q.end(), id);
        if (pos != q.end()) q.erase(pos);
        if (q.empty()) bids_.erase(it);
    } else {
        auto it = asks_.find(price);
        if (it == asks_.end()) return;
        auto& q = it->second;
        auto  pos = std::find(q.begin(), q.end(), id);
        if (pos != q.end()) q.erase(pos);
        if (q.empty()) asks_.erase(it);
    }
}

// ─── Core matching: BUY aggressor sweeps the ask side ─────────────────────────
std::vector<Trade> OrderBook::matchBuy(Order& aggressor, Timestamp ts) {
    std::vector<Trade> trades;

    while (!asks_.empty() && aggressor.qty_remaining > 0) {
        Price ask_price = asks_.begin()->first;

        // Limit orders only cross up to their limit price.
        if (aggressor.type == OrderType::LIMIT && aggressor.price < ask_price)
            break;

        auto& ask_queue = asks_.begin()->second;

        while (!ask_queue.empty() && aggressor.qty_remaining > 0) {
            OrderId  rest_id = ask_queue.front();
            Order&   resting = orders_.at(rest_id);

            Qty fill = std::min(aggressor.qty_remaining, resting.qty_remaining);

            trades.push_back(Trade{
                ts, symbol_, resting.price, fill,
                Side::BUY, rest_id, aggressor.id
            });

            aggressor.qty_remaining -= fill;
            resting.qty_remaining   -= fill;

            if (resting.qty_remaining == 0) {
                resting.status = OrderStatus::FILLED;
                ask_queue.pop_front();
                // Keep in orders_ for audit; will not be in any price level.
            } else {
                resting.status = OrderStatus::PARTIALLY_FILLED;
                // Resting order consumed the aggressor completely; stop.
            }
        }

        if (ask_queue.empty())
            asks_.erase(asks_.begin());
        else
            break;   // Best level still has resting qty → aggressor exhausted.
    }

    aggressor.status = (aggressor.qty_remaining == 0)
                       ? OrderStatus::FILLED
                       : (trades.empty() ? OrderStatus::ACTIVE
                                         : OrderStatus::PARTIALLY_FILLED);
    return trades;
}

// ─── Core matching: SELL aggressor sweeps the bid side ────────────────────────
std::vector<Trade> OrderBook::matchSell(Order& aggressor, Timestamp ts) {
    std::vector<Trade> trades;

    while (!bids_.empty() && aggressor.qty_remaining > 0) {
        Price bid_price = bids_.begin()->first;

        if (aggressor.type == OrderType::LIMIT && aggressor.price > bid_price)
            break;

        auto& bid_queue = bids_.begin()->second;

        while (!bid_queue.empty() && aggressor.qty_remaining > 0) {
            OrderId rest_id = bid_queue.front();
            Order&  resting = orders_.at(rest_id);

            Qty fill = std::min(aggressor.qty_remaining, resting.qty_remaining);

            trades.push_back(Trade{
                ts, symbol_, resting.price, fill,
                Side::SELL, rest_id, aggressor.id
            });

            aggressor.qty_remaining -= fill;
            resting.qty_remaining   -= fill;

            if (resting.qty_remaining == 0) {
                resting.status = OrderStatus::FILLED;
                bid_queue.pop_front();
            } else {
                resting.status = OrderStatus::PARTIALLY_FILLED;
            }
        }

        if (bid_queue.empty())
            bids_.erase(bids_.begin());
        else
            break;
    }

    aggressor.status = (aggressor.qty_remaining == 0)
                       ? OrderStatus::FILLED
                       : (trades.empty() ? OrderStatus::ACTIVE
                                         : OrderStatus::PARTIALLY_FILLED);
    return trades;
}

// ─── addLimit ─────────────────────────────────────────────────────────────────
MatchResult OrderBook::addLimit(const OrderEvent& event) {
    if (event.qty == 0) return {};

    Order o;
    o.id            = event.order_id;
    o.symbol        = event.symbol;
    o.side          = event.side;
    o.type          = OrderType::LIMIT;
    o.price         = event.price;
    o.qty_original  = event.qty;
    o.qty_remaining = event.qty;
    o.timestamp     = event.timestamp;
    o.status        = OrderStatus::ACTIVE;

    orders_[o.id] = o;          // register in lookup table first

    auto snap = snapshot();

    std::vector<Trade> trades;
    if (event.side == Side::BUY)
        trades = matchBuy (orders_.at(o.id), event.timestamp);
    else
        trades = matchSell(orders_.at(o.id), event.timestamp);

    // If unfilled remainder exists, add to the book.
    if (orders_.at(o.id).qty_remaining > 0)
        addToLevel(o.id);

    return MatchResult{std::move(trades), snapshotChanged(snap)};
}

// ─── addMarket ────────────────────────────────────────────────────────────────
MatchResult OrderBook::addMarket(const OrderEvent& event) {
    if (event.qty == 0) return {};

    Order o;
    o.id            = event.order_id;
    o.symbol        = event.symbol;
    o.side          = event.side;
    o.type          = OrderType::MARKET;
    // Market orders match at any price; use extreme sentinels.
    o.price         = (event.side == Side::BUY) ? INT64_MAX : 0;
    o.qty_original  = event.qty;
    o.qty_remaining = event.qty;
    o.timestamp     = event.timestamp;
    o.status        = OrderStatus::ACTIVE;

    orders_[o.id] = o;

    auto snap = snapshot();

    std::vector<Trade> trades;
    if (event.side == Side::BUY)
        trades = matchBuy (orders_.at(o.id), event.timestamp);
    else
        trades = matchSell(orders_.at(o.id), event.timestamp);

    // Market orders never rest; any unfilled qty is silently discarded.
    if (orders_.at(o.id).qty_remaining > 0)
        orders_.at(o.id).status = OrderStatus::CANCELLED;

    return MatchResult{std::move(trades), snapshotChanged(snap)};
}

// ─── cancel ───────────────────────────────────────────────────────────────────
bool OrderBook::cancel(OrderId id, Timestamp /*ts*/) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return false;

    Order& o = it->second;
    if (o.status == OrderStatus::FILLED || o.status == OrderStatus::CANCELLED)
        return false;

    removeFromLevel(id, o.side, o.price);
    o.status        = OrderStatus::CANCELLED;
    o.qty_remaining = 0;
    return true;
}

// ─── replace ──────────────────────────────────────────────────────────────────
// Semantics: cancel original order (loses time priority), submit new limit.
MatchResult OrderBook::replace(const OrderEvent& event) {
    cancel(event.order_id, event.timestamp);

    OrderEvent neo   = event;
    neo.order_id     = event.new_order_id;
    neo.price        = event.new_price;
    neo.qty          = event.new_qty;
    neo.type         = OrderType::LIMIT;

    return addLimit(neo);
}

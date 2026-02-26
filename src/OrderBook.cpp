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
            last_trade_price_ = resting.price;

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
            last_trade_price_ = resting.price;

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

// ─── addLimitInternal ─────────────────────────────────────────────────────────
// Pure matching logic; does NOT check stop triggers. Called by the public
// addLimit wrapper and by executeStop (to avoid re-entrant trigger checking).
MatchResult OrderBook::addLimitInternal(const OrderEvent& event) {
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

// ─── addMarketInternal ────────────────────────────────────────────────────────
// Pure matching logic; does NOT check stop triggers.
MatchResult OrderBook::addMarketInternal(const OrderEvent& event) {
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
// Uses the public addLimit so that stop orders are checked after the new limit matches.
MatchResult OrderBook::replace(const OrderEvent& event) {
    cancel(event.order_id, event.timestamp);

    OrderEvent neo   = event;
    neo.order_id     = event.new_order_id;
    neo.price        = event.new_price;
    neo.qty          = event.new_qty;
    neo.type         = OrderType::LIMIT;

    return addLimit(neo);   // public wrapper: matches + checks stops
}

// ─── addLimit (public wrapper) ────────────────────────────────────────────────
// Delegates to addLimitInternal then checks for triggered stop orders.
MatchResult OrderBook::addLimit(const OrderEvent& event) {
    MatchResult result = addLimitInternal(event);
    checkAndTriggerStops(result, event.timestamp);
    return result;
}

// ─── addMarket (public wrapper) ───────────────────────────────────────────────
// Delegates to addMarketInternal then checks for triggered stop orders.
MatchResult OrderBook::addMarket(const OrderEvent& event) {
    MatchResult result = addMarketInternal(event);
    checkAndTriggerStops(result, event.timestamp);
    return result;
}

// ─── Stop order helpers ───────────────────────────────────────────────────────

bool OrderBook::isStopTriggered(const StopEntry& entry) const {
    if (last_trade_price_ == 0) return false;   // no trades yet; never trigger
    if (entry.side == Side::BUY)
        return last_trade_price_ >= entry.stop_price;
    else
        return last_trade_price_ <= entry.stop_price;
}

// Execute a triggered stop by converting it into a market or limit order.
// Calls the *Internal variants to avoid re-entrant stop checking.
// Merges the resulting trades and tob_changed flag into 'accumulated'.
void OrderBook::executeStop(const StopEntry& entry,
                            MatchResult& accumulated,
                            Timestamp ts) {
    OrderEvent synthetic;
    synthetic.timestamp = ts;
    synthetic.symbol    = symbol_;
    synthetic.order_id  = entry.id;
    synthetic.side      = entry.side;
    synthetic.qty       = entry.qty;

    MatchResult sub;
    if (entry.type == OrderType::STOP_MARKET) {
        synthetic.type  = OrderType::MARKET;
        synthetic.price = (entry.side == Side::BUY) ? INT64_MAX : 0;
        sub = addMarketInternal(synthetic);
    } else {
        synthetic.type  = OrderType::LIMIT;
        synthetic.price = entry.limit_price;
        sub = addLimitInternal(synthetic);
    }

    accumulated.trades.insert(
        accumulated.trades.end(),
        std::make_move_iterator(sub.trades.begin()),
        std::make_move_iterator(sub.trades.end())
    );
    accumulated.tob_changed |= sub.tob_changed;
}

// Check and fire any stop orders triggered by the current last_trade_price_.
// Iterates until no new stops fire (handles cascades where a triggered stop
// produces trades that trigger further stops).
void OrderBook::checkAndTriggerStops(MatchResult& accumulated, Timestamp ts) {
    if (last_trade_price_ == 0) return;   // no trades yet; nothing to check

    bool any_triggered = true;
    while (any_triggered) {
        any_triggered = false;

        // Buy stops: trigger when last_trade_price_ >= stop_price.
        // buy_stops_ is ascending → begin() has the lowest stop_price.
        while (!buy_stops_.empty() &&
               last_trade_price_ >= buy_stops_.begin()->first) {
            auto& q = buy_stops_.begin()->second;
            while (!q.empty()) {
                StopEntry entry = q.front();
                q.pop_front();
                executeStop(entry, accumulated, ts);
                any_triggered = true;
            }
            buy_stops_.erase(buy_stops_.begin());
        }

        // Sell stops: trigger when last_trade_price_ <= stop_price.
        // sell_stops_ is descending → begin() has the highest stop_price.
        while (!sell_stops_.empty() &&
               last_trade_price_ <= sell_stops_.begin()->first) {
            auto& q = sell_stops_.begin()->second;
            while (!q.empty()) {
                StopEntry entry = q.front();
                q.pop_front();
                executeStop(entry, accumulated, ts);
                any_triggered = true;
            }
            sell_stops_.erase(sell_stops_.begin());
        }
    }
}

// ─── addStop ──────────────────────────────────────────────────────────────────
// Register a stop order. If the stop is already past the trigger threshold
// (e.g., the market gapped through it), execute it immediately.
// Otherwise, enqueue it to fire when price crosses stop_price.
MatchResult OrderBook::addStop(const OrderEvent& event) {
    if (event.qty == 0)    return {};
    if (event.price == 0)  return {};   // stop_price must be non-zero
    // STOP_LIMIT requires a valid limit price
    if (event.type == OrderType::STOP_LIMIT && event.limit_price == 0) return {};

    StopEntry entry{
        event.order_id,
        event.type,
        event.side,
        event.price,        // stop_price (trigger level)
        event.limit_price,  // 0 for STOP_MARKET
        event.qty,
        event.timestamp
    };

    if (isStopTriggered(entry)) {
        // Price has already crossed the stop_price; fire immediately.
        MatchResult result;
        executeStop(entry, result, event.timestamp);
        checkAndTriggerStops(result, event.timestamp);  // handle cascade
        return result;
    }

    // Enqueue for future triggering.
    if (event.side == Side::BUY)
        buy_stops_[event.price].push_back(entry);
    else
        sell_stops_[event.price].push_back(entry);

    return {};  // stop is resting; no immediate trades, no TOB change
}

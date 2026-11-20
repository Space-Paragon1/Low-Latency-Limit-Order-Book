#include "lob/OrderBook.h"

#include <algorithm>

namespace lob {

using detail::OrderNode;
using detail::PriceLevel;

namespace {

// Does an aggressor with limit `limit` reach a resting level at `resting`?
constexpr bool reaches(Side aggressor, Price limit, Price resting) noexcept {
    return aggressor == Side::BUY ? limit >= resting : limit <= resting;
}

constexpr std::size_t kOrderChunk = 4096;   // growth increment once the reservation is used up
constexpr std::size_t kLevelChunk = 1024;

}  // namespace

OrderBook::OrderBook(Symbol symbol, std::size_t expected_orders)
    : symbol_(symbol),
      orders_(expected_orders),
      order_pool_(kOrderChunk),
      level_pool_(kLevelChunk) {
    order_pool_.reserve(expected_orders);
    trades_.reserve(256);
    elected_.reserve(64);
}

// ─── Per-operation bookkeeping ────────────────────────────────────────────────

OrderBook::Top OrderBook::top() const noexcept {
    Top t{0, 0, 0, 0};
    if (!bids_.empty()) {
        const PriceLevel* l = bids_.best();
        t.bid_px  = l->price;
        t.bid_qty = l->total_qty;
    }
    if (!asks_.empty()) {
        const PriceLevel* l = asks_.best();
        t.ask_px  = l->price;
        t.ask_qty = l->total_qty;
    }
    return t;
}

void OrderBook::beginOp() noexcept {
    trades_.clear();
    round_traded_ = false;
}

MatchResult OrderBook::finish(MatchResult r, const Top& before) noexcept {
    r.trades      = TradeSpan(trades_.data(), trades_.size());
    r.tob_changed = !(top() == before);
    return r;
}

MatchResult OrderBook::reject(RejectReason why, const Top& before) noexcept {
    MatchResult r;
    r.status        = OrderStatus::REJECTED;
    r.reject_reason = why;
    return finish(r, before);
}

RejectReason OrderBook::checkNewOrder(OrderId id, Qty qty) const noexcept {
    if (qty == 0) return RejectReason::INVALID_QUANTITY;
    if (orders_.find(id) != nullptr) return RejectReason::DUPLICATE_ORDER_ID;
    return RejectReason::NONE;
}

bool OrderBook::wouldCross(Side side, Price price) const noexcept {
    if (side == Side::BUY) return !asks_.empty() && price >= asks_.bestPrice();
    return !bids_.empty() && price <= bids_.bestPrice();
}

bool OrderBook::canFill(Side side, Price limit, bool limited, Qty qty) const noexcept {
    auto scan = [&](const auto& book) {
        Qty available = 0;
        for (std::size_t i = 0; i < book.size(); ++i) {
            const PriceLevel* l = book.at(i);
            if (limited && !reaches(side, limit, l->price)) return false;
            available += l->total_qty;
            if (available >= qty) return true;
        }
        return false;
    };
    return side == Side::BUY ? scan(asks_) : scan(bids_);
}

void OrderBook::recordTrade(Price px) noexcept {
    last_trade_price_ = px;
    if (!round_traded_) {
        round_high_   = px;
        round_low_    = px;
        round_traded_ = true;
    } else {
        round_high_ = std::max(round_high_, px);
        round_low_  = std::min(round_low_, px);
    }
}

// ─── Matching core ────────────────────────────────────────────────────────────
//
// Sweeps the opposite side from the best level outward while the aggressor has
// quantity and (for limit orders) the level is within its limit.  Fully filled
// resting orders are unlinked and recycled on the spot; emptied levels are
// popped off the back of the ladder.  Returns the aggressor's unfilled qty.
template <class Ladder>
Qty OrderBook::match(Ladder& book, Side aggressor, Price limit, bool limited, Qty qty,
                     OrderId id, Timestamp ts) {
    while (qty > 0 && !book.empty()) {
        PriceLevel* lvl = book.best();
        const Price px  = lvl->price;
        if (limited && !reaches(aggressor, limit, px)) break;

        do {
            OrderNode* resting = lvl->head;
            const Qty  fill    = std::min(qty, resting->qty);
            trades_.push_back(Trade{ts, symbol_, px, fill, aggressor, resting->id, id});
            qty            -= fill;
            resting->qty   -= fill;
            lvl->total_qty -= fill;
            if (resting->qty == 0) {
                lvl->unlink(resting);
                orders_.erase(resting->id);
                order_pool_.release(resting);
            }
        } while (qty > 0 && !lvl->empty());

        recordTrade(px);
        if (lvl->empty()) {
            book.popBest();
            level_pool_.release(lvl);
        }
    }
    return qty;
}

void OrderBook::rest(OrderId id, Side side, Price price, Qty qty, TimeInForce tif, Timestamp ts) {
    OrderNode* n   = order_pool_.acquire();
    n->id          = id;
    n->qty         = qty;
    n->limit_price = 0;
    n->timestamp   = ts;
    n->side        = side;
    n->type        = OrderType::LIMIT;
    n->tif         = tif;
    PriceLevel* lvl = side == Side::BUY ? bids_.findOrCreate(price, level_pool_)
                                        : asks_.findOrCreate(price, level_pool_);
    lvl->pushBack(n);
    orders_.insert(id, n);
}

void OrderBook::removeNode(OrderNode* n) noexcept {
    PriceLevel* lvl = n->level;
    lvl->unlink(n);
    if (lvl->empty()) {
        if (isStop(n->type)) {
            if (n->side == Side::BUY) buy_stops_.erase(lvl);
            else                      sell_stops_.erase(lvl);
        } else {
            if (n->side == Side::BUY) bids_.erase(lvl);
            else                      asks_.erase(lvl);
        }
        level_pool_.release(lvl);
    }
    if (isStop(n->type)) --pending_stops_;
    orders_.erase(n->id);
    order_pool_.release(n);
}

void OrderBook::executeLimit(OrderId id, Side side, Price price, Qty qty, TimeInForce tif,
                             Timestamp ts, MatchResult& r) {
    if (tif == TimeInForce::POST_ONLY && wouldCross(side, price)) {
        r.status        = OrderStatus::REJECTED;
        r.reject_reason = RejectReason::POST_ONLY_WOULD_CROSS;
        return;
    }
    if (tif == TimeInForce::FOK && !canFill(side, price, /*limited=*/true, qty)) {
        r.status = OrderStatus::CANCELLED;
        return;
    }

    const Qty remaining = side == Side::BUY ? match(asks_, side, price, true, qty, id, ts)
                                            : match(bids_, side, price, true, qty, id, ts);
    r.filled_qty = qty - remaining;

    if (remaining == 0) {
        r.status = OrderStatus::FILLED;
    } else if (tif == TimeInForce::IOC || tif == TimeInForce::FOK) {
        r.status = OrderStatus::CANCELLED;
    } else {
        rest(id, side, price, remaining, tif, ts);
        r.status     = r.filled_qty > 0 ? OrderStatus::PARTIALLY_FILLED : OrderStatus::ACTIVE;
        r.leaves_qty = remaining;
    }
}

void OrderBook::executeMarket(OrderId id, Side side, Qty qty, TimeInForce tif, Timestamp ts,
                              MatchResult& r) {
    if (tif == TimeInForce::FOK && !canFill(side, 0, /*limited=*/false, qty)) {
        r.status = OrderStatus::CANCELLED;
        return;
    }
    const Qty remaining = side == Side::BUY ? match(asks_, side, 0, false, qty, id, ts)
                                            : match(bids_, side, 0, false, qty, id, ts);
    r.filled_qty = qty - remaining;
    r.status     = remaining == 0 ? OrderStatus::FILLED : OrderStatus::CANCELLED;
}

// ─── Stop orders ──────────────────────────────────────────────────────────────
//
// A stop is *elected* by any trade that prints at or through its stop price
// (buy: price >= stop, sell: price <= stop).  After each operation the range of
// prices that traded is used to elect stops; elected stops are released in
// priority order (buy stops lowest-stop first, then sell stops highest-stop
// first, FIFO within a stop price) as market or limit orders.  Their trades
// form the next round's range, so cascades resolve iteratively.

template <class Ladder, class Elected>
void OrderBook::collectStops(Ladder& stops, Elected elected) {
    while (!stops.empty() && elected(stops.bestPrice())) {
        PriceLevel* lvl = stops.best();
        for (OrderNode* n = lvl->head; n != nullptr;) {
            OrderNode* next = n->next;
            elected_.push_back(*n);
            orders_.erase(n->id);
            order_pool_.release(n);
            --pending_stops_;
            n = next;
        }
        stops.popBest();
        level_pool_.release(lvl);
    }
}

void OrderBook::releaseStop(const OrderNode& stop, Timestamp ts, MatchResult& r) {
    if (stop.type == OrderType::STOP_MARKET)
        executeMarket(stop.id, stop.side, stop.qty, TimeInForce::GTC, ts, r);
    else
        executeLimit(stop.id, stop.side, stop.limit_price, stop.qty, TimeInForce::GTC, ts, r);
}

void OrderBook::runStops(Timestamp ts) {
    while (round_traded_ && pending_stops_ > 0) {
        const Price high = round_high_;
        const Price low  = round_low_;
        round_traded_    = false;

        elected_.clear();
        collectStops(buy_stops_,  [high](Price stop) { return stop <= high; });
        collectStops(sell_stops_, [low](Price stop) { return stop >= low; });
        if (elected_.empty()) break;

        // releaseStop() never touches elected_, so iterating it here is safe.
        for (const OrderNode& stop : elected_) {
            MatchResult released;
            releaseStop(stop, ts, released);
        }
    }
}

// ─── Public mutations ─────────────────────────────────────────────────────────

MatchResult OrderBook::apply(const OrderEvent& ev) {
    switch (ev.type) {
        case OrderType::LIMIT:       return addLimit(ev);
        case OrderType::MARKET:      return addMarket(ev);
        case OrderType::CANCEL:      return cancel(ev.order_id);
        case OrderType::REPLACE:     return replace(ev);
        case OrderType::STOP_MARKET:
        case OrderType::STOP_LIMIT:  return addStop(ev);
    }
    beginOp();   // unreachable for valid enum values
    return reject(RejectReason::NONE, top());
}

MatchResult OrderBook::addLimit(const OrderEvent& ev) {
    const Top before = top();
    beginOp();
    if (const RejectReason why = checkNewOrder(ev.order_id, ev.qty); why != RejectReason::NONE)
        return reject(why, before);
    if (ev.price <= 0) return reject(RejectReason::INVALID_PRICE, before);

    MatchResult r;
    executeLimit(ev.order_id, ev.side, ev.price, ev.qty, ev.tif, ev.timestamp, r);
    runStops(ev.timestamp);
    return finish(r, before);
}

MatchResult OrderBook::addMarket(const OrderEvent& ev) {
    const Top before = top();
    beginOp();
    if (const RejectReason why = checkNewOrder(ev.order_id, ev.qty); why != RejectReason::NONE)
        return reject(why, before);
    if (ev.tif == TimeInForce::POST_ONLY) return reject(RejectReason::INVALID_TIME_IN_FORCE, before);

    MatchResult r;
    executeMarket(ev.order_id, ev.side, ev.qty, ev.tif, ev.timestamp, r);
    runStops(ev.timestamp);
    return finish(r, before);
}

MatchResult OrderBook::addStop(const OrderEvent& ev) {
    const Top before = top();
    beginOp();
    if (const RejectReason why = checkNewOrder(ev.order_id, ev.qty); why != RejectReason::NONE)
        return reject(why, before);
    if (ev.price <= 0) return reject(RejectReason::INVALID_PRICE, before);
    if (ev.type == OrderType::STOP_LIMIT && ev.limit_price <= 0)
        return reject(RejectReason::INVALID_PRICE, before);

    OrderNode stop;
    stop.id          = ev.order_id;
    stop.qty         = ev.qty;
    stop.limit_price = ev.type == OrderType::STOP_LIMIT ? ev.limit_price : 0;
    stop.timestamp   = ev.timestamp;
    stop.side        = ev.side;
    stop.type        = ev.type;

    MatchResult r;
    // The market has already traded through the stop: release immediately.
    // (No trade yet → the stop simply waits; there is no reference price.)
    const bool already_elected =
        last_trade_price_ != 0 &&
        (ev.side == Side::BUY ? last_trade_price_ >= ev.price : last_trade_price_ <= ev.price);
    if (already_elected) {
        releaseStop(stop, ev.timestamp, r);
        runStops(ev.timestamp);
        return finish(r, before);
    }

    OrderNode* n = order_pool_.acquire();
    *n           = stop;
    PriceLevel* lvl = ev.side == Side::BUY ? buy_stops_.findOrCreate(ev.price, level_pool_)
                                           : sell_stops_.findOrCreate(ev.price, level_pool_);
    lvl->pushBack(n);
    orders_.insert(n->id, n);
    ++pending_stops_;

    r.status     = OrderStatus::ACTIVE;
    r.leaves_qty = ev.qty;
    return finish(r, before);
}

MatchResult OrderBook::cancel(OrderId id) {
    const Top before = top();
    beginOp();
    OrderNode* n = orders_.find(id);
    if (n == nullptr) return reject(RejectReason::UNKNOWN_ORDER, before);
    removeNode(n);
    MatchResult r;
    r.status = OrderStatus::CANCELLED;
    return finish(r, before);
}

// Cancel/replace of a resting limit order.  Side and time-in-force are taken
// from the original order.  A replace that keeps the price and does not
// increase the quantity is applied in place and keeps time priority (as on
// most exchanges); anything else cancels the original and enters a new order
// at the back of the queue, which may trade immediately.  The request is
// validated in full before the original is touched, so a rejected replace
// leaves the book unchanged.
MatchResult OrderBook::replace(const OrderEvent& ev) {
    const Top before = top();
    beginOp();
    OrderNode* n = orders_.find(ev.order_id);
    if (n == nullptr || n->type != OrderType::LIMIT) return reject(RejectReason::UNKNOWN_ORDER, before);
    if (ev.new_qty == 0)    return reject(RejectReason::INVALID_QUANTITY, before);
    if (ev.new_price <= 0)  return reject(RejectReason::INVALID_PRICE, before);
    const OrderId new_id = ev.new_order_id;
    if (new_id != n->id && orders_.find(new_id) != nullptr)
        return reject(RejectReason::DUPLICATE_ORDER_ID, before);

    MatchResult r;
    if (ev.new_price == n->level->price && ev.new_qty <= n->qty) {
        n->level->total_qty -= n->qty - ev.new_qty;
        n->qty = ev.new_qty;
        if (new_id != n->id) {
            orders_.erase(n->id);
            n->id = new_id;
            orders_.insert(new_id, n);
        }
        r.status     = OrderStatus::ACTIVE;
        r.leaves_qty = n->qty;
        return finish(r, before);
    }

    const Side        side = n->side;
    const TimeInForce tif  = n->tif;
    if (tif == TimeInForce::POST_ONLY && wouldCross(side, ev.new_price))
        return reject(RejectReason::POST_ONLY_WOULD_CROSS, before);

    removeNode(n);
    executeLimit(new_id, side, ev.new_price, ev.new_qty, tif, ev.timestamp, r);
    runStops(ev.timestamp);
    return finish(r, before);
}

// ─── Queries ──────────────────────────────────────────────────────────────────

TOBUpdate OrderBook::makeTOB(Timestamp ts) const noexcept {
    const Top t = top();
    return TOBUpdate{ts, symbol_, t.bid_px, t.bid_qty, t.ask_px, t.ask_qty};
}

bool OrderBook::isCrossed() const noexcept {
    return !bids_.empty() && !asks_.empty() && bids_.bestPrice() >= asks_.bestPrice();
}

std::optional<OrderInfo> OrderBook::findOrder(OrderId id) const {
    const OrderNode* n = orders_.find(id);
    if (n == nullptr) return std::nullopt;
    return OrderInfo{n->id, n->side, n->type, n->tif, n->level->price, n->limit_price, n->qty,
                     n->timestamp};
}

std::vector<DepthLevel> OrderBook::depth(Side side, std::size_t max_levels) const {
    std::vector<DepthLevel> out;
    auto collect = [&](const auto& book) {
        const std::size_t n = std::min(max_levels, book.size());
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            const PriceLevel* l = book.at(i);
            out.push_back(DepthLevel{l->price, l->total_qty, l->order_count});
        }
    };
    if (side == Side::BUY) collect(bids_);
    else                   collect(asks_);
    return out;
}

bool OrderBook::validate(std::string* error) const {
    std::string msg;
    std::size_t nodes  = 0;
    std::size_t stops  = 0;
    std::size_t levels = 0;

    auto check_ladder = [&](const auto& ladder, Side side, bool stop_ladder, const char* name) {
        for (std::size_t i = 0; i < ladder.size(); ++i) {
            const PriceLevel* l = ladder.at(i);
            const std::string where = std::string(name) + " level " + std::to_string(l->price);
            if (i > 0 && !ladder.isBetter(ladder.at(i - 1)->price, l->price)) {
                msg = where + ": levels not strictly ordered";
                return false;
            }
            if (l->empty()) {
                msg = where + ": empty level left in ladder";
                return false;
            }
            Qty              sum   = 0;
            std::uint32_t    count = 0;
            const OrderNode* prev  = nullptr;
            for (const OrderNode* n = l->head; n != nullptr; n = n->next) {
                const std::string who = where + " order " + std::to_string(n->id);
                if (n->level != l)                 { msg = who + ": bad level back-pointer"; return false; }
                if (n->prev != prev)               { msg = who + ": broken prev link"; return false; }
                if (n->side != side)               { msg = who + ": on the wrong side"; return false; }
                if (n->qty == 0)                   { msg = who + ": zero open quantity"; return false; }
                if (isStop(n->type) != stop_ladder) { msg = who + ": wrong order type for ladder"; return false; }
                if (orders_.find(n->id) != n)      { msg = who + ": missing from id map"; return false; }
                sum += n->qty;
                ++count;
                prev = n;
            }
            if (l->tail != prev)        { msg = where + ": bad tail pointer"; return false; }
            if (sum != l->total_qty)    { msg = where + ": total_qty mismatch"; return false; }
            if (count != l->order_count) { msg = where + ": order_count mismatch"; return false; }
            nodes += count;
            if (stop_ladder) stops += count;
            ++levels;
        }
        return true;
    };

    const bool ok = check_ladder(bids_, Side::BUY, false, "bid") &&
                    check_ladder(asks_, Side::SELL, false, "ask") &&
                    check_ladder(buy_stops_, Side::BUY, true, "buy-stop") &&
                    check_ladder(sell_stops_, Side::SELL, true, "sell-stop");
    if (ok) {
        if (nodes != orders_.size())             msg = "id map size does not match linked orders";
        else if (stops != pending_stops_)        msg = "pending stop count mismatch";
        else if (order_pool_.inUse() != nodes)   msg = "order pool leak";
        else if (level_pool_.inUse() != levels)  msg = "price level pool leak";
        else if (isCrossed())                    msg = "book is crossed";
    }
    if (msg.empty()) return true;
    if (error != nullptr) *error = msg;
    return false;
}

}  // namespace lob

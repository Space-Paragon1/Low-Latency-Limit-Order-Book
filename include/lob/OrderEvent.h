#pragma once

#include "lob/Types.h"

namespace lob {

// ─── Inbound event (parsed from CSV or a network message) ────────────────────
//
// One flat struct covers every message type; fields that do not apply to a
// type are ignored.  The static factories are the preferred way to build one.
struct OrderEvent {
    Timestamp   timestamp{0};
    Symbol      symbol;
    OrderType   type{OrderType::LIMIT};
    Side        side{Side::BUY};
    TimeInForce tif{TimeInForce::GTC};
    OrderId     order_id{0};
    Price       price{0};          // LIMIT: limit price.  STOP_*: stop (trigger) price.
    Qty         qty{0};
    Price       limit_price{0};    // STOP_LIMIT: limit price of the released order.

    // REPLACE only.  `order_id` is the order being replaced; side and
    // time-in-force are inherited from it.
    OrderId     new_order_id{0};
    Price       new_price{0};
    Qty         new_qty{0};

    static OrderEvent limit(Timestamp ts, Symbol sym, OrderId id, Side side, Price price,
                            Qty qty, TimeInForce tif = TimeInForce::GTC) noexcept {
        OrderEvent e;
        e.timestamp = ts;
        e.symbol    = sym;
        e.type      = OrderType::LIMIT;
        e.order_id  = id;
        e.side      = side;
        e.price     = price;
        e.qty       = qty;
        e.tif       = tif;
        return e;
    }

    static OrderEvent market(Timestamp ts, Symbol sym, OrderId id, Side side, Qty qty,
                             TimeInForce tif = TimeInForce::GTC) noexcept {
        OrderEvent e;
        e.timestamp = ts;
        e.symbol    = sym;
        e.type      = OrderType::MARKET;
        e.order_id  = id;
        e.side      = side;
        e.qty       = qty;
        e.tif       = tif;
        return e;
    }

    static OrderEvent cancel(Timestamp ts, Symbol sym, OrderId id) noexcept {
        OrderEvent e;
        e.timestamp = ts;
        e.symbol    = sym;
        e.type      = OrderType::CANCEL;
        e.order_id  = id;
        return e;
    }

    static OrderEvent replace(Timestamp ts, Symbol sym, OrderId old_id, OrderId new_id,
                              Price new_price, Qty new_qty) noexcept {
        OrderEvent e;
        e.timestamp    = ts;
        e.symbol       = sym;
        e.type         = OrderType::REPLACE;
        e.order_id     = old_id;
        e.new_order_id = new_id;
        e.new_price    = new_price;
        e.new_qty      = new_qty;
        return e;
    }

    static OrderEvent stopMarket(Timestamp ts, Symbol sym, OrderId id, Side side,
                                 Price stop_price, Qty qty) noexcept {
        OrderEvent e;
        e.timestamp = ts;
        e.symbol    = sym;
        e.type      = OrderType::STOP_MARKET;
        e.order_id  = id;
        e.side      = side;
        e.price     = stop_price;
        e.qty       = qty;
        return e;
    }

    static OrderEvent stopLimit(Timestamp ts, Symbol sym, OrderId id, Side side,
                                Price stop_price, Price limit_price, Qty qty) noexcept {
        OrderEvent e;
        e.timestamp   = ts;
        e.symbol      = sym;
        e.type        = OrderType::STOP_LIMIT;
        e.order_id    = id;
        e.side        = side;
        e.price       = stop_price;
        e.limit_price = limit_price;
        e.qty         = qty;
        return e;
    }
};

}  // namespace lob

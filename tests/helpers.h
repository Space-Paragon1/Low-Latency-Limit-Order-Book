#pragma once
// Shared shorthand for the test files.

#include "lob/OrderBook.h"
#include "lob/OrderEvent.h"
#include "lob/Types.h"
#include "test_framework.h"

#include <string>

namespace th {

using lob::OrderEvent;
using lob::OrderId;
using lob::Price;
using lob::Qty;
using lob::Side;
using lob::TimeInForce;

inline const lob::Symbol kSym{"SYM"};

// Orders use their id as the timestamp, which keeps traces readable.
inline OrderEvent buy(OrderId id, Price px, Qty qty, TimeInForce tif = TimeInForce::GTC) {
    return OrderEvent::limit(id, kSym, id, Side::BUY, px, qty, tif);
}
inline OrderEvent sell(OrderId id, Price px, Qty qty, TimeInForce tif = TimeInForce::GTC) {
    return OrderEvent::limit(id, kSym, id, Side::SELL, px, qty, tif);
}
inline OrderEvent mktBuy(OrderId id, Qty qty, TimeInForce tif = TimeInForce::GTC) {
    return OrderEvent::market(id, kSym, id, Side::BUY, qty, tif);
}
inline OrderEvent mktSell(OrderId id, Qty qty, TimeInForce tif = TimeInForce::GTC) {
    return OrderEvent::market(id, kSym, id, Side::SELL, qty, tif);
}
inline OrderEvent replace(OrderId old_id, OrderId new_id, Price px, Qty qty) {
    return OrderEvent::replace(new_id, kSym, old_id, new_id, px, qty);
}
inline OrderEvent stopMkt(OrderId id, Side side, Price stop, Qty qty) {
    return OrderEvent::stopMarket(id, kSym, id, side, stop, qty);
}
inline OrderEvent stopLmt(OrderId id, Side side, Price stop, Price limit, Qty qty) {
    return OrderEvent::stopLimit(id, kSym, id, side, stop, limit, qty);
}

}  // namespace th

// Runs the book's full invariant check and fails the test with its message.
#define ASSERT_VALID(book)                                                         \
    do {                                                                           \
        std::string lobtest_err;                                                   \
        if (!(book).validate(&lobtest_err)) FAIL("book invariant: " << lobtest_err); \
    } while (0)

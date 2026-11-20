// Stop-market and stop-limit orders: election, release, cascades, cancels.

#include "helpers.h"

using namespace lob;
using namespace th;

TEST(stop_market_buy_triggers_on_price_rise) {
    // Resting SELL 200 @ 10050; buy stop 100 @ 10050.
    // LIMIT BUY 50 @ 10050 prints at 10050 → stop releases a market buy for 100.
    OrderBook book(kSym);
    book.addLimit(sell(1, 10050, 200));
    auto queued = book.addStop(stopMkt(2, Side::BUY, 10050, 100));
    ASSERT_EQ(queued.status, OrderStatus::ACTIVE);
    ASSERT_EQ(book.pendingStopCount(), 1u);

    auto r = book.addLimit(buy(3, 10050, 50));
    ASSERT_EQ(r.trades.size(), 2u);
    ASSERT_EQ(r.trades[0].qty, 50u);
    ASSERT_EQ(r.trades[1].qty, 100u);
    ASSERT_EQ(r.trades[1].aggressing_order_id, 2u);
    ASSERT_EQ(r.status, OrderStatus::FILLED);   // status of order 3, not the stop
    ASSERT_EQ(book.bestAskQty(), 50u);
    ASSERT_EQ(book.pendingStopCount(), 0u);
    ASSERT_VALID(book);
}

TEST(stop_market_sell_triggers_on_price_fall) {
    OrderBook book(kSym);
    book.addLimit(buy(1, 9950, 200));
    book.addStop(stopMkt(2, Side::SELL, 9950, 100));
    auto r = book.addLimit(sell(3, 9950, 50));
    ASSERT_EQ(r.trades.size(), 2u);
    ASSERT_EQ(r.trades[0].qty, 50u);
    ASSERT_EQ(r.trades[1].qty, 100u);
    ASSERT_EQ(book.bestBidQty(), 50u);
    ASSERT_VALID(book);
}

TEST(stop_limit_triggers_and_rests_without_liquidity) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10000, 50));
    book.addStop(stopLmt(3, Side::BUY, 10000, 10010, 100));
    auto r = book.addLimit(buy(2, 10000, 50));
    ASSERT_EQ(r.trades.size(), 1u);
    ASSERT_TRUE(r.tob_changed);
    ASSERT_EQ(book.bestBidPrice(), 10010);
    ASSERT_EQ(book.bestBidQty(), 100u);
    const auto o = book.findOrder(3);
    ASSERT_TRUE(o.has_value());
    ASSERT_EQ(o->type, OrderType::LIMIT);   // released: now an ordinary resting limit
    ASSERT_VALID(book);
}

TEST(stop_does_not_trigger_before_threshold) {
    OrderBook book(kSym);
    book.addLimit(buy(1, 10000, 100));
    book.addStop(stopMkt(2, Side::SELL, 9900, 100));
    auto r = book.addLimit(sell(3, 10000, 50));
    ASSERT_EQ(r.trades.size(), 1u);
    ASSERT_EQ(book.bestBidQty(), 50u);
    ASSERT_EQ(book.pendingStopCount(), 1u);
}

TEST(stop_cascade_one_stop_triggers_another) {
    //   bids: 9900 x200, 9800 x200
    //   stop A: sell 250 @ stop 9900     stop B: sell 50 @ stop 9800
    // SELL 10 @ 9900 prints 9900 → A releases: 190 @ 9900, 60 @ 9800
    // → the 9800 print elects B: 50 @ 9800.  Bid at 9800 left with 90.
    OrderBook book(kSym);
    book.addLimit(buy(1, 9900, 200));
    book.addLimit(buy(2, 9800, 200));
    book.addStop(stopMkt(4, Side::SELL, 9900, 250));
    book.addStop(stopMkt(5, Side::SELL, 9800, 50));

    auto r = book.addLimit(sell(3, 9900, 10));
    ASSERT_EQ(r.trades.size(), 4u);
    ASSERT_EQ(r.trades[3].aggressing_order_id, 5u);
    ASSERT_EQ(book.bestBidPrice(), 9800);
    ASSERT_EQ(book.bestBidQty(), 90u);
    ASSERT_EQ(book.pendingStopCount(), 0u);
    ASSERT_VALID(book);
}

TEST(stop_waits_until_first_trade) {
    // No trade has printed, so there is no reference price yet.
    OrderBook book(kSym);
    book.addLimit(sell(1, 10000, 100));
    auto r = book.addStop(stopMkt(2, Side::BUY, 5000, 100));
    ASSERT_EQ(r.trades.size(), 0u);
    ASSERT_EQ(r.status, OrderStatus::ACTIVE);
    ASSERT_TRUE(book.hasAsk());
}

TEST(stop_released_immediately_if_already_through) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10100, 100));
    book.addLimit(buy(2, 10100, 100));   // last trade = 10100
    book.addLimit(sell(3, 10100, 50));
    auto r = book.addStop(stopMkt(4, Side::BUY, 10000, 50));
    ASSERT_EQ(r.trades.size(), 1u);
    ASSERT_EQ(r.trades[0].qty, 50u);
    ASSERT_EQ(r.status, OrderStatus::FILLED);
    ASSERT_FALSE(book.hasAsk());
    ASSERT_EQ(book.pendingStopCount(), 0u);
}

TEST(buy_stop_elected_by_early_print_of_a_falling_sweep) {
    // A sell sweep prints 10100 then 9900.  The buy stop at 10050 is elected
    // by the 10100 print even though the *last* price (9900) is below it.
    OrderBook book(kSym);
    book.addLimit(sell(1, 9800, 10));
    book.addLimit(buy(2, 9800, 10));          // last trade 9800
    book.addStop(stopMkt(3, Side::BUY, 10050, 5));
    book.addLimit(buy(4, 10100, 10));
    book.addLimit(buy(5, 9900, 10));
    book.addLimit(sell(6, 10500, 5));         // liquidity for the stop to lift
    auto r = book.addMarket(mktSell(7, 20));  // prints 10100, 9900
    ASSERT_EQ(r.trades.size(), 3u);
    ASSERT_EQ(r.trades[2].aggressing_order_id, 3u);
    ASSERT_EQ(r.trades[2].price, 10500);
    ASSERT_EQ(book.pendingStopCount(), 0u);
    ASSERT_VALID(book);
}

TEST(elected_stops_release_in_trigger_then_time_order) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10000, 1));
    book.addLimit(buy(2, 10000, 1));   // last trade 10000
    book.addStop(stopMkt(10, Side::BUY, 10030, 1));
    book.addStop(stopMkt(11, Side::BUY, 10020, 1));
    book.addStop(stopMkt(12, Side::BUY, 10020, 1));
    book.addLimit(sell(3, 10040, 1));
    book.addLimit(sell(4, 10050, 10));
    auto r = book.addLimit(buy(5, 10040, 1));   // prints 10040: elects all three
    ASSERT_EQ(r.trades.size(), 4u);
    ASSERT_EQ(r.trades[1].aggressing_order_id, 11u);   // lowest trigger first...
    ASSERT_EQ(r.trades[2].aggressing_order_id, 12u);   // ...FIFO within a trigger price
    ASSERT_EQ(r.trades[3].aggressing_order_id, 10u);
}

TEST(stop_market_without_liquidity_is_cancelled_on_release) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10000, 10));
    book.addStop(stopMkt(2, Side::BUY, 10000, 5));
    auto r = book.addLimit(buy(3, 10000, 10));   // consumes the only ask, elects the stop
    ASSERT_EQ(r.trades.size(), 1u);
    ASSERT_EQ(book.pendingStopCount(), 0u);
    ASSERT_FALSE(book.contains(2));
    ASSERT_VALID(book);
}

TEST(cancel_pending_stop) {
    OrderBook book(kSym);
    book.addStop(stopMkt(1, Side::BUY, 10100, 10));
    book.addStop(stopLmt(2, Side::SELL, 9900, 9890, 10));
    ASSERT_EQ(book.pendingStopCount(), 2u);
    auto r = book.cancel(1);
    ASSERT_EQ(r.status, OrderStatus::CANCELLED);
    ASSERT_FALSE(r.tob_changed);
    ASSERT_EQ(book.pendingStopCount(), 1u);
    ASSERT_TRUE(book.cancel(2).accepted());
    ASSERT_EQ(book.orderCount(), 0u);
    ASSERT_VALID(book);

    // A cancelled stop must not fire later.
    book.addLimit(sell(3, 10100, 10));
    auto t = book.addLimit(buy(4, 10100, 5));
    ASSERT_EQ(t.trades.size(), 1u);
}

TEST(stop_validation) {
    OrderBook book(kSym);
    book.addLimit(buy(1, 10000, 10));
    ASSERT_EQ(book.addStop(stopMkt(1, Side::BUY, 10100, 10)).reject_reason,
              RejectReason::DUPLICATE_ORDER_ID);
    ASSERT_EQ(book.addStop(stopMkt(2, Side::BUY, 0, 10)).reject_reason,
              RejectReason::INVALID_PRICE);
    ASSERT_EQ(book.addStop(stopLmt(3, Side::BUY, 10100, 0, 10)).reject_reason,
              RejectReason::INVALID_PRICE);
    ASSERT_EQ(book.addStop(stopMkt(4, Side::BUY, 10100, 0)).reject_reason,
              RejectReason::INVALID_QUANTITY);
    // A pending stop's id is live: new orders may not reuse it.
    book.addStop(stopMkt(5, Side::SELL, 9000, 10));
    ASSERT_EQ(book.addLimit(sell(5, 11000, 10)).reject_reason, RejectReason::DUPLICATE_ORDER_ID);
    ASSERT_VALID(book);
}

TEST(replace_cannot_target_a_pending_stop) {
    OrderBook book(kSym);
    book.addStop(stopMkt(1, Side::BUY, 10100, 10));
    ASSERT_EQ(book.replace(replace(1, 2, 10200, 10)).reject_reason, RejectReason::UNKNOWN_ORDER);
    ASSERT_EQ(book.pendingStopCount(), 1u);
}

TEST(find_order_on_pending_stop) {
    OrderBook book(kSym);
    book.addStop(stopLmt(9, Side::SELL, 9900, 9850, 25));
    const auto o = book.findOrder(9);
    ASSERT_TRUE(o.has_value());
    ASSERT_EQ(o->type, OrderType::STOP_LIMIT);
    ASSERT_EQ(o->price, 9900);
    ASSERT_EQ(o->limit_price, 9850);
    ASSERT_EQ(o->qty, 25u);
}

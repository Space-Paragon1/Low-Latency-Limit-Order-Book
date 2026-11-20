// Core matching behaviour of a single OrderBook: price-time priority, partial
// fills, cancels, replaces, market orders, time-in-force and invariants.

#include "helpers.h"

#include <algorithm>
#include <random>
#include <vector>

using namespace lob;
using namespace th;

// ═════════════════════════════════════════════════════════════════════════════
// Simple matches
// ═════════════════════════════════════════════════════════════════════════════

TEST(simple_buy_sell_exact) {
    OrderBook book(kSym);
    auto r1 = book.addLimit(buy(1, 10050, 100));
    ASSERT_EQ(r1.trades.size(), 0u);
    ASSERT_EQ(r1.status, OrderStatus::ACTIVE);
    ASSERT_EQ(r1.leaves_qty, 100u);
    ASSERT_TRUE(book.hasBid());
    ASSERT_FALSE(book.hasAsk());

    auto r2 = book.addLimit(sell(2, 10050, 100));
    ASSERT_EQ(r2.trades.size(), 1u);
    ASSERT_EQ(r2.trades[0].price, 10050);
    ASSERT_EQ(r2.trades[0].qty, 100u);
    ASSERT_EQ(r2.trades[0].resting_order_id, 1u);
    ASSERT_EQ(r2.trades[0].aggressing_order_id, 2u);
    ASSERT_EQ(r2.trades[0].aggressor_side, Side::SELL);
    ASSERT_EQ(r2.trades[0].symbol, kSym);
    ASSERT_EQ(r2.status, OrderStatus::FILLED);
    ASSERT_EQ(r2.filled_qty, 100u);

    ASSERT_FALSE(book.hasBid());
    ASSERT_FALSE(book.hasAsk());
    ASSERT_EQ(book.orderCount(), 0u);
    ASSERT_EQ(book.lastTradePrice(), 10050);
    ASSERT_VALID(book);
}

TEST(trade_prints_at_resting_price) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10000, 50));
    auto r = book.addLimit(buy(2, 10050, 50));   // willing to pay more
    ASSERT_EQ(r.trades.size(), 1u);
    ASSERT_EQ(r.trades[0].price, 10000);
    ASSERT_EQ(r.trades[0].aggressor_side, Side::BUY);
    ASSERT_FALSE(book.hasBid());
    ASSERT_FALSE(book.hasAsk());
}

TEST(no_cross_no_match) {
    OrderBook book(kSym);
    book.addLimit(buy(1, 9900, 100));
    auto r = book.addLimit(sell(2, 10000, 100));
    ASSERT_EQ(r.trades.size(), 0u);
    ASSERT_TRUE(book.hasBid());
    ASSERT_TRUE(book.hasAsk());
    ASSERT_FALSE(book.isCrossed());
    ASSERT_VALID(book);
}

// ═════════════════════════════════════════════════════════════════════════════
// Partial fills and priority
// ═════════════════════════════════════════════════════════════════════════════

TEST(partial_fill_resting) {
    OrderBook book(kSym);
    book.addLimit(buy(1, 10000, 200));
    auto r = book.addLimit(sell(2, 10000, 80));
    ASSERT_EQ(r.trades.size(), 1u);
    ASSERT_EQ(r.trades[0].qty, 80u);
    ASSERT_EQ(book.bestBidQty(), 120u);
    ASSERT_EQ(book.findOrder(1)->qty, 120u);
    ASSERT_FALSE(book.hasAsk());
    ASSERT_VALID(book);
}

TEST(partial_fill_aggressor_rests_remainder) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10000, 80));
    auto r = book.addLimit(buy(2, 10010, 200));
    ASSERT_EQ(r.trades.size(), 1u);
    ASSERT_EQ(r.status, OrderStatus::PARTIALLY_FILLED);
    ASSERT_EQ(r.filled_qty, 80u);
    ASSERT_EQ(r.leaves_qty, 120u);
    ASSERT_FALSE(book.hasAsk());
    ASSERT_EQ(book.bestBidPrice(), 10010);   // remainder rests at its own limit
    ASSERT_EQ(book.bestBidQty(), 120u);
    ASSERT_VALID(book);
}

TEST(multi_level_sweep) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10000, 100));
    book.addLimit(sell(2, 10010, 100));
    book.addLimit(sell(3, 10020, 100));
    auto r = book.addLimit(buy(4, 10050, 300));
    ASSERT_EQ(r.trades.size(), 3u);
    ASSERT_EQ(r.trades[0].price, 10000);
    ASSERT_EQ(r.trades[1].price, 10010);
    ASSERT_EQ(r.trades[2].price, 10020);
    ASSERT_FALSE(book.hasAsk());
    ASSERT_FALSE(book.hasBid());
    ASSERT_VALID(book);
}

TEST(sweep_stops_at_limit_price) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10000, 100));
    book.addLimit(sell(2, 10010, 100));
    book.addLimit(sell(3, 10020, 100));
    auto r = book.addLimit(buy(4, 10010, 250));
    ASSERT_EQ(r.trades.size(), 2u);
    ASSERT_EQ(r.filled_qty, 200u);
    ASSERT_EQ(book.bestBidPrice(), 10010);
    ASSERT_EQ(book.bestBidQty(), 50u);
    ASSERT_EQ(book.bestAskPrice(), 10020);
    ASSERT_VALID(book);
}

TEST(fifo_within_price_level) {
    OrderBook book(kSym);
    book.addLimit(sell(10, 10000, 50));
    book.addLimit(sell(20, 10000, 50));
    book.addLimit(sell(30, 10000, 50));
    auto r = book.addLimit(buy(40, 10000, 120));
    ASSERT_EQ(r.trades.size(), 3u);
    ASSERT_EQ(r.trades[0].resting_order_id, 10u);
    ASSERT_EQ(r.trades[1].resting_order_id, 20u);
    ASSERT_EQ(r.trades[2].resting_order_id, 30u);
    ASSERT_EQ(r.trades[2].qty, 20u);
    ASSERT_EQ(book.bestAskQty(), 30u);
    ASSERT_VALID(book);
}

TEST(price_beats_time) {
    OrderBook book(kSym);
    book.addLimit(buy(1, 10000, 10));   // earlier, worse price
    book.addLimit(buy(2, 10010, 10));   // later, better price
    auto r = book.addLimit(sell(3, 9990, 10));
    ASSERT_EQ(r.trades.size(), 1u);
    ASSERT_EQ(r.trades[0].resting_order_id, 2u);
    ASSERT_EQ(r.trades[0].price, 10010);
}

// ═════════════════════════════════════════════════════════════════════════════
// Validation and rejects
// ═════════════════════════════════════════════════════════════════════════════

TEST(duplicate_live_id_rejected_and_book_untouched) {
    OrderBook book(kSym);
    book.addLimit(buy(1, 10000, 100));
    auto r = book.addLimit(sell(1, 9000, 50));   // would cross if accepted
    ASSERT_EQ(r.status, OrderStatus::REJECTED);
    ASSERT_EQ(r.reject_reason, RejectReason::DUPLICATE_ORDER_ID);
    ASSERT_EQ(r.trades.size(), 0u);
    ASSERT_FALSE(r.tob_changed);
    ASSERT_EQ(book.bestBidQty(), 100u);
    ASSERT_FALSE(book.hasAsk());
    ASSERT_VALID(book);
}

TEST(id_of_finished_order_can_be_reused) {
    OrderBook book(kSym);
    book.addLimit(buy(1, 10000, 100));
    book.cancel(1);
    auto r = book.addLimit(buy(1, 10000, 100));
    ASSERT_TRUE(r.accepted());
    ASSERT_VALID(book);
}

TEST(zero_quantity_and_bad_price_rejected) {
    OrderBook book(kSym);
    ASSERT_EQ(book.addLimit(buy(1, 10000, 0)).reject_reason, RejectReason::INVALID_QUANTITY);
    ASSERT_EQ(book.addLimit(buy(2, 0, 10)).reject_reason, RejectReason::INVALID_PRICE);
    ASSERT_EQ(book.addLimit(buy(3, -5, 10)).reject_reason, RejectReason::INVALID_PRICE);
    ASSERT_EQ(book.addMarket(mktBuy(4, 0)).reject_reason, RejectReason::INVALID_QUANTITY);
    ASSERT_EQ(book.orderCount(), 0u);
    ASSERT_VALID(book);
}

TEST(order_id_zero_is_a_valid_id) {
    OrderBook book(kSym);
    ASSERT_TRUE(book.addLimit(buy(0, 10000, 5)).accepted());
    ASSERT_TRUE(book.contains(0));
    ASSERT_TRUE(book.cancel(0).accepted());
    ASSERT_FALSE(book.contains(0));
    ASSERT_VALID(book);
}

// ═════════════════════════════════════════════════════════════════════════════
// Cancel
// ═════════════════════════════════════════════════════════════════════════════

TEST(cancel_removes_order) {
    OrderBook book(kSym);
    book.addLimit(buy(1, 10000, 100));
    auto r = book.cancel(1);
    ASSERT_EQ(r.status, OrderStatus::CANCELLED);
    ASSERT_TRUE(r.tob_changed);
    ASSERT_FALSE(book.hasBid());
    ASSERT_EQ(book.bidLevels(), 0u);
    ASSERT_VALID(book);
}

TEST(cancel_unknown_order_rejected) {
    OrderBook book(kSym);
    auto r = book.cancel(999);
    ASSERT_EQ(r.status, OrderStatus::REJECTED);
    ASSERT_EQ(r.reject_reason, RejectReason::UNKNOWN_ORDER);
}

TEST(cancel_filled_order_rejected) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10000, 50));
    book.addLimit(buy(2, 10000, 50));
    ASSERT_EQ(book.cancel(1).reject_reason, RejectReason::UNKNOWN_ORDER);
    ASSERT_EQ(book.cancel(1).reject_reason, RejectReason::UNKNOWN_ORDER);   // and again
}

TEST(cancel_mid_level_preserves_fifo) {
    OrderBook book(kSym);
    book.addLimit(sell(10, 10000, 50));
    book.addLimit(sell(20, 10000, 50));
    book.addLimit(sell(30, 10000, 50));
    book.cancel(20);
    ASSERT_EQ(book.bestAskQty(), 100u);
    auto r = book.addLimit(buy(40, 10000, 100));
    ASSERT_EQ(r.trades.size(), 2u);
    ASSERT_EQ(r.trades[0].resting_order_id, 10u);
    ASSERT_EQ(r.trades[1].resting_order_id, 30u);
    ASSERT_VALID(book);
}

TEST(cancel_head_and_tail_of_level) {
    OrderBook book(kSym);
    for (OrderId id = 1; id <= 4; ++id) book.addLimit(buy(id, 10000, 10));
    book.cancel(1);   // head
    book.cancel(4);   // tail
    ASSERT_VALID(book);
    auto r = book.addLimit(sell(5, 10000, 20));
    ASSERT_EQ(r.trades.size(), 2u);
    ASSERT_EQ(r.trades[0].resting_order_id, 2u);
    ASSERT_EQ(r.trades[1].resting_order_id, 3u);
}

TEST(deep_cancel_does_not_change_top_of_book) {
    OrderBook book(kSym);
    book.addLimit(buy(1, 10000, 100));
    book.addLimit(buy(2, 9900, 100));
    auto r = book.cancel(2);
    ASSERT_TRUE(r.accepted());
    ASSERT_FALSE(r.tob_changed);
    ASSERT_EQ(book.bidLevels(), 1u);
    ASSERT_VALID(book);
}

// ═════════════════════════════════════════════════════════════════════════════
// Replace
// ═════════════════════════════════════════════════════════════════════════════

TEST(replace_price_change_loses_priority) {
    OrderBook book(kSym);
    book.addLimit(buy(1, 10000, 100));
    book.addLimit(buy(2, 10010, 100));
    // Move order 2 down to 10000 → joins the back of the queue behind order 1.
    auto rep = book.replace(replace(2, 3, 10000, 100));
    ASSERT_EQ(rep.status, OrderStatus::ACTIVE);
    ASSERT_TRUE(rep.tob_changed);
    auto r = book.addLimit(sell(4, 10000, 100));
    ASSERT_EQ(r.trades.size(), 1u);
    ASSERT_EQ(r.trades[0].resting_order_id, 1u);
    ASSERT_VALID(book);
}

TEST(replace_quantity_increase_loses_priority) {
    OrderBook book(kSym);
    book.addLimit(buy(1, 10000, 100));
    book.addLimit(buy(2, 10000, 100));
    book.replace(replace(1, 1, 10000, 150));   // same id, more qty → back of queue
    auto r = book.addLimit(sell(3, 10000, 100));
    ASSERT_EQ(r.trades[0].resting_order_id, 2u);
    ASSERT_EQ(book.bestBidQty(), 150u);
    ASSERT_VALID(book);
}

TEST(replace_quantity_decrease_keeps_priority) {
    OrderBook book(kSym);
    book.addLimit(buy(1, 10000, 100));
    book.addLimit(buy(2, 10000, 100));
    auto rep = book.replace(replace(1, 1, 10000, 40));
    ASSERT_EQ(rep.leaves_qty, 40u);
    ASSERT_TRUE(rep.tob_changed);   // aggregate qty at the touch changed
    ASSERT_EQ(book.bestBidQty(), 140u);
    auto r = book.addLimit(sell(3, 10000, 40));
    ASSERT_EQ(r.trades.size(), 1u);
    ASSERT_EQ(r.trades[0].resting_order_id, 1u);
    ASSERT_VALID(book);
}

TEST(replace_in_place_can_rename_order) {
    OrderBook book(kSym);
    book.addLimit(buy(1, 10000, 100));
    book.addLimit(buy(2, 10000, 100));
    book.replace(replace(1, 7, 10000, 100));   // same price & qty, new id: keeps its place
    ASSERT_FALSE(book.contains(1));
    ASSERT_TRUE(book.contains(7));
    auto r = book.addLimit(sell(3, 10000, 10));
    ASSERT_EQ(r.trades[0].resting_order_id, 7u);
    ASSERT_VALID(book);
}

TEST(replace_can_trigger_match) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10100, 50));
    book.addLimit(buy(2, 9900, 50));
    auto r = book.replace(replace(2, 3, 10200, 50));
    ASSERT_EQ(r.trades.size(), 1u);
    ASSERT_EQ(r.trades[0].price, 10100);
    ASSERT_EQ(r.trades[0].aggressing_order_id, 3u);
    ASSERT_EQ(r.status, OrderStatus::FILLED);
    ASSERT_VALID(book);
}

TEST(replace_inherits_side_from_original) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10100, 50));
    OrderEvent ev = replace(1, 2, 10200, 50);
    ev.side = Side::BUY;   // a wrong side on the request must not flip the order
    book.replace(ev);
    ASSERT_FALSE(book.hasBid());
    ASSERT_EQ(book.bestAskPrice(), 10200);
    ASSERT_EQ(book.findOrder(2)->side, Side::SELL);
}

TEST(replace_of_dead_order_rejected_no_new_order) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10000, 50));
    book.addLimit(buy(2, 10000, 50));   // order 1 fully filled
    auto r = book.replace(replace(1, 3, 10000, 50));
    ASSERT_EQ(r.reject_reason, RejectReason::UNKNOWN_ORDER);
    ASSERT_EQ(book.orderCount(), 0u);   // no phantom order created
    ASSERT_VALID(book);
}

TEST(replace_rejects_leave_original_intact) {
    OrderBook book(kSym);
    book.addLimit(buy(1, 10000, 100));
    book.addLimit(buy(2, 9000, 100));
    ASSERT_EQ(book.replace(replace(1, 2, 10010, 100)).reject_reason, RejectReason::DUPLICATE_ORDER_ID);
    ASSERT_EQ(book.replace(replace(1, 3, 10010, 0)).reject_reason, RejectReason::INVALID_QUANTITY);
    ASSERT_EQ(book.replace(replace(1, 3, 0, 100)).reject_reason, RejectReason::INVALID_PRICE);
    ASSERT_EQ(book.bestBidPrice(), 10000);
    ASSERT_EQ(book.findOrder(1)->qty, 100u);
    ASSERT_VALID(book);
}

// ═════════════════════════════════════════════════════════════════════════════
// Market orders
// ═════════════════════════════════════════════════════════════════════════════

TEST(market_buy_sweeps_asks) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10000, 100));
    book.addLimit(sell(2, 10010, 100));
    auto r = book.addMarket(mktBuy(3, 150));
    ASSERT_EQ(r.trades.size(), 2u);
    ASSERT_EQ(r.trades[0].qty, 100u);
    ASSERT_EQ(r.trades[1].qty, 50u);
    ASSERT_EQ(r.status, OrderStatus::FILLED);
    ASSERT_EQ(book.bestAskQty(), 50u);
    ASSERT_VALID(book);
}

TEST(market_sell_sweeps_bids) {
    OrderBook book(kSym);
    book.addLimit(buy(1, 10010, 100));
    book.addLimit(buy(2, 10000, 100));
    auto r = book.addMarket(mktSell(3, 120));
    ASSERT_EQ(r.trades.size(), 2u);
    ASSERT_EQ(r.trades[0].price, 10010);
    ASSERT_EQ(r.trades[0].qty, 100u);
    ASSERT_EQ(r.trades[1].qty, 20u);
    ASSERT_EQ(book.bestBidQty(), 80u);
    ASSERT_VALID(book);
}

TEST(market_remainder_is_cancelled_never_rests) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10000, 30));
    auto r = book.addMarket(mktBuy(2, 100));
    ASSERT_EQ(r.status, OrderStatus::CANCELLED);
    ASSERT_EQ(r.filled_qty, 30u);
    ASSERT_EQ(r.leaves_qty, 0u);
    ASSERT_FALSE(book.hasBid());
    ASSERT_FALSE(book.contains(2));
    ASSERT_VALID(book);
}

TEST(market_no_liquidity) {
    OrderBook book(kSym);
    auto r = book.addMarket(mktBuy(1, 100));
    ASSERT_EQ(r.trades.size(), 0u);
    ASSERT_EQ(r.status, OrderStatus::CANCELLED);
    ASSERT_FALSE(r.tob_changed);
    ASSERT_EQ(book.orderCount(), 0u);
}

TEST(market_fok_and_post_only) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10000, 50));
    auto kill = book.addMarket(mktBuy(2, 60, TimeInForce::FOK));
    ASSERT_EQ(kill.status, OrderStatus::CANCELLED);
    ASSERT_EQ(kill.trades.size(), 0u);
    ASSERT_EQ(book.bestAskQty(), 50u);
    auto fill = book.addMarket(mktBuy(3, 50, TimeInForce::FOK));
    ASSERT_EQ(fill.status, OrderStatus::FILLED);
    ASSERT_EQ(book.addMarket(mktBuy(4, 1, TimeInForce::POST_ONLY)).reject_reason,
              RejectReason::INVALID_TIME_IN_FORCE);
}

// ═════════════════════════════════════════════════════════════════════════════
// Time in force
// ═════════════════════════════════════════════════════════════════════════════

TEST(ioc_takes_available_and_cancels_rest) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10000, 30));
    book.addLimit(sell(2, 10020, 30));   // beyond the IOC limit
    auto r = book.addLimit(buy(3, 10010, 100, TimeInForce::IOC));
    ASSERT_EQ(r.status, OrderStatus::CANCELLED);
    ASSERT_EQ(r.filled_qty, 30u);
    ASSERT_EQ(r.trades.size(), 1u);
    ASSERT_FALSE(book.hasBid());
    ASSERT_EQ(book.bestAskPrice(), 10020);
    ASSERT_VALID(book);
}

TEST(ioc_full_fill) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10000, 30));
    auto r = book.addLimit(buy(2, 10000, 30, TimeInForce::IOC));
    ASSERT_EQ(r.status, OrderStatus::FILLED);
}

TEST(fok_fills_across_levels) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10000, 30));
    book.addLimit(sell(2, 10010, 30));
    auto r = book.addLimit(buy(3, 10010, 60, TimeInForce::FOK));
    ASSERT_EQ(r.status, OrderStatus::FILLED);
    ASSERT_EQ(r.trades.size(), 2u);
    ASSERT_FALSE(book.hasAsk());
}

TEST(fok_kills_when_liquidity_within_limit_is_short) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10000, 30));
    book.addLimit(sell(2, 10010, 30));
    book.addLimit(sell(3, 10020, 100));   // plenty here, but beyond the limit
    auto r = book.addLimit(buy(4, 10010, 61, TimeInForce::FOK));
    ASSERT_EQ(r.status, OrderStatus::CANCELLED);
    ASSERT_EQ(r.trades.size(), 0u);
    ASSERT_FALSE(r.tob_changed);
    ASSERT_EQ(book.bestAskQty(), 30u);
    ASSERT_EQ(book.orderCount(), 3u);
    ASSERT_VALID(book);
}

TEST(post_only_rests_when_passive) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10010, 30));
    auto r = book.addLimit(buy(2, 10000, 30, TimeInForce::POST_ONLY));
    ASSERT_EQ(r.status, OrderStatus::ACTIVE);
    ASSERT_EQ(book.bestBidPrice(), 10000);
}

TEST(post_only_rejected_when_it_would_take) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10010, 30));
    auto r = book.addLimit(buy(2, 10010, 30, TimeInForce::POST_ONLY));
    ASSERT_EQ(r.reject_reason, RejectReason::POST_ONLY_WOULD_CROSS);
    ASSERT_EQ(r.trades.size(), 0u);
    ASSERT_EQ(book.bestAskQty(), 30u);
    ASSERT_FALSE(book.hasBid());
}

TEST(post_only_replace_that_would_cross_is_rejected) {
    OrderBook book(kSym);
    book.addLimit(sell(1, 10010, 30));
    book.addLimit(buy(2, 10000, 30, TimeInForce::POST_ONLY));
    auto r = book.replace(replace(2, 3, 10010, 30));
    ASSERT_EQ(r.reject_reason, RejectReason::POST_ONLY_WOULD_CROSS);
    ASSERT_TRUE(book.contains(2));   // original untouched
    ASSERT_EQ(book.findOrder(2)->tif, TimeInForce::POST_ONLY);
    ASSERT_VALID(book);
}

// ═════════════════════════════════════════════════════════════════════════════
// Top of book, depth, inspection
// ═════════════════════════════════════════════════════════════════════════════

TEST(tob_change_detected) {
    OrderBook book(kSym);
    ASSERT_TRUE(book.addLimit(buy(1, 10000, 100)).tob_changed);    // new best bid
    ASSERT_FALSE(book.addLimit(buy(2, 9900, 100)).tob_changed);    // behind the touch
    ASSERT_TRUE(book.addLimit(buy(3, 10000, 100)).tob_changed);    // more qty at the touch
    ASSERT_TRUE(book.addLimit(sell(4, 10000, 100)).tob_changed);   // trade at the touch
}

TEST(make_tob_reports_aggregates) {
    OrderBook book(kSym);
    book.addLimit(buy(1, 10000, 100));
    book.addLimit(buy(2, 10000, 50));
    book.addLimit(sell(3, 10010, 70));
    const TOBUpdate t = book.makeTOB(42);
    ASSERT_EQ(t.timestamp, 42u);
    ASSERT_EQ(t.symbol, kSym);
    ASSERT_EQ(t.best_bid_price, 10000);
    ASSERT_EQ(t.best_bid_qty, 150u);
    ASSERT_EQ(t.best_ask_price, 10010);
    ASSERT_EQ(t.best_ask_qty, 70u);
}

TEST(depth_snapshot_best_first) {
    OrderBook book(kSym);
    book.addLimit(buy(1, 9990, 10));
    book.addLimit(buy(2, 10000, 20));
    book.addLimit(buy(3, 10000, 5));
    book.addLimit(buy(4, 9980, 7));
    book.addLimit(sell(5, 10020, 3));
    book.addLimit(sell(6, 10010, 4));

    const auto bids = book.depth(Side::BUY, 2);
    ASSERT_EQ(bids.size(), 2u);
    ASSERT_EQ(bids[0].price, 10000);
    ASSERT_EQ(bids[0].qty, 25u);
    ASSERT_EQ(bids[0].orders, 2u);
    ASSERT_EQ(bids[1].price, 9990);

    const auto asks = book.depth(Side::SELL, 10);
    ASSERT_EQ(asks.size(), 2u);
    ASSERT_EQ(asks[0].price, 10010);
    ASSERT_EQ(asks[1].price, 10020);
}

TEST(find_order_reports_state) {
    OrderBook book(kSym);
    book.addLimit(buy(5, 10000, 100, TimeInForce::POST_ONLY));
    book.addLimit(sell(6, 10000, 40));
    const auto o = book.findOrder(5);
    ASSERT_TRUE(o.has_value());
    ASSERT_EQ(o->side, Side::BUY);
    ASSERT_EQ(o->price, 10000);
    ASSERT_EQ(o->qty, 60u);
    ASSERT_EQ(o->tif, TimeInForce::POST_ONLY);
    ASSERT_EQ(o->timestamp, 5u);
    ASSERT_FALSE(book.findOrder(6).has_value());
}

TEST(book_never_crossed_after_match) {
    OrderBook book(kSym);
    book.addLimit(buy(1, 10050, 200));
    book.addLimit(sell(2, 10000, 100));
    book.addLimit(sell(3, 10010, 100));
    book.addLimit(buy(4, 10060, 50));
    ASSERT_FALSE(book.isCrossed());
    ASSERT_VALID(book);
}

// ═════════════════════════════════════════════════════════════════════════════
// Structure under load
// ═════════════════════════════════════════════════════════════════════════════

TEST(many_levels_in_random_order_stay_sorted) {
    // Exercises both the linear-probe and binary-search paths of the ladder.
    OrderBook book(kSym);
    std::vector<Price> prices;
    for (Price p = 1; p <= 2000; ++p) prices.push_back(p * 5);
    std::mt19937 rng(7);
    std::shuffle(prices.begin(), prices.end(), rng);
    OrderId id = 1;
    for (Price p : prices) book.addLimit(buy(id++, p, 1));
    ASSERT_EQ(book.bidLevels(), 2000u);
    ASSERT_VALID(book);

    // Ids were assigned in shuffled price order, so cancelling every other id
    // removes levels scattered across the whole ladder.
    for (OrderId x = 1; x < id; x += 2) ASSERT_TRUE(book.cancel(x).accepted());
    ASSERT_EQ(book.bidLevels(), 1000u);
    ASSERT_VALID(book);

    const auto top = book.depth(Side::BUY, 1000);
    for (std::size_t i = 1; i < top.size(); ++i) ASSERT_GT(top[i - 1].price, top[i].price);
}

TEST(memory_is_recycled_under_churn) {
    OrderBook book(kSym, /*expected_orders=*/64);
    for (OrderId i = 0; i < 200000; ++i) {
        book.addLimit(buy(i, 10000 - static_cast<Price>(i % 50), 10));
        if (i >= 32) book.cancel(i - 32);
    }
    ASSERT_EQ(book.orderCount(), 32u);
    ASSERT_VALID(book);   // includes pool accounting: no leaked nodes or levels
}

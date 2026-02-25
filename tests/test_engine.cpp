#include "test_framework.h"
#include "OrderBook.h"
#include "MatchingEngine.h"
#include "MessageParser.h"
#include <cassert>

// ─── Helpers ──────────────────────────────────────────────────────────────────
static OrderEvent makeLimit(Timestamp ts, const std::string& sym,
                            OrderId id, Side side, Price price, Qty qty) {
    OrderEvent e;
    e.timestamp = ts;
    e.symbol    = sym;
    e.type      = OrderType::LIMIT;
    e.order_id  = id;
    e.side      = side;
    e.price     = price;
    e.qty       = qty;
    return e;
}

static OrderEvent makeMarket(Timestamp ts, const std::string& sym,
                             OrderId id, Side side, Qty qty) {
    OrderEvent e;
    e.timestamp = ts;
    e.symbol    = sym;
    e.type      = OrderType::MARKET;
    e.order_id  = id;
    e.side      = side;
    e.qty       = qty;
    return e;
}

static OrderEvent makeCancel(Timestamp ts, const std::string& sym, OrderId id) {
    OrderEvent e;
    e.timestamp = ts;
    e.symbol    = sym;
    e.type      = OrderType::CANCEL;
    e.order_id  = id;
    return e;
}

static OrderEvent makeReplace(Timestamp ts, const std::string& sym,
                              OrderId old_id, OrderId new_id,
                              Price new_price, Qty new_qty) {
    OrderEvent e;
    e.timestamp    = ts;
    e.symbol       = sym;
    e.type         = OrderType::REPLACE;
    e.order_id     = old_id;
    e.new_order_id = new_id;
    e.new_price    = new_price;
    e.new_qty      = new_qty;
    e.side         = Side::BUY;   // required by addLimit internally
    return e;
}

// ═════════════════════════════════════════════════════════════════════════════
// SECTION 1 – Simple matches
// ═════════════════════════════════════════════════════════════════════════════

TEST(simple_buy_sell_exact) {
    // BUY  100 @ 10050 rests, then SELL 100 @ 10050 → full match
    OrderBook book("AAPL");
    auto r1 = book.addLimit(makeLimit(1, "AAPL", 1, Side::BUY,  10050, 100));
    ASSERT_EQ(r1.trades.size(), 0u);
    ASSERT_TRUE(book.hasBid());
    ASSERT_FALSE(book.hasAsk());

    auto r2 = book.addLimit(makeLimit(2, "AAPL", 2, Side::SELL, 10050, 100));
    ASSERT_EQ(r2.trades.size(), 1u);
    ASSERT_EQ(r2.trades[0].price, 10050);
    ASSERT_EQ(r2.trades[0].qty,   100u);
    ASSERT_EQ(r2.trades[0].resting_order_id,    1u);
    ASSERT_EQ(r2.trades[0].aggressing_order_id, 2u);
    ASSERT_EQ(r2.trades[0].aggressor_side, Side::SELL);

    ASSERT_FALSE(book.hasBid());
    ASSERT_FALSE(book.hasAsk());
    ASSERT_FALSE(book.isCrossed());
}

TEST(buy_aggresses_resting_ask) {
    // ASK 50 @ 10000 rests, BUY 50 @ 10050 arrives → match at ask price (10000)
    OrderBook book("AAPL");
    book.addLimit(makeLimit(1, "AAPL", 1, Side::SELL, 10000, 50));
    ASSERT_TRUE(book.hasAsk());

    auto r = book.addLimit(makeLimit(2, "AAPL", 2, Side::BUY, 10050, 50));
    ASSERT_EQ(r.trades.size(), 1u);
    ASSERT_EQ(r.trades[0].price, 10000);   // resting price
    ASSERT_EQ(r.trades[0].qty,  50u);
    ASSERT_EQ(r.trades[0].aggressor_side, Side::BUY);
    ASSERT_FALSE(book.hasBid());
    ASSERT_FALSE(book.hasAsk());
}

TEST(no_cross_no_match) {
    // BUY 100 @ 9900, SELL 100 @ 10000 → no match
    OrderBook book("AAPL");
    book.addLimit(makeLimit(1, "AAPL", 1, Side::BUY,  9900, 100));
    auto r = book.addLimit(makeLimit(2, "AAPL", 2, Side::SELL, 10000, 100));
    ASSERT_EQ(r.trades.size(), 0u);
    ASSERT_TRUE(book.hasBid());
    ASSERT_TRUE(book.hasAsk());
    ASSERT_FALSE(book.isCrossed());
}

// ═════════════════════════════════════════════════════════════════════════════
// SECTION 2 – Partial fills
// ═════════════════════════════════════════════════════════════════════════════

TEST(partial_fill_resting) {
    // BUY 200 rests; SELL 80 aggresses → 80 filled, 120 remains on bid
    OrderBook book("AAPL");
    book.addLimit(makeLimit(1, "AAPL", 1, Side::BUY, 10000, 200));
    auto r = book.addLimit(makeLimit(2, "AAPL", 2, Side::SELL, 10000, 80));

    ASSERT_EQ(r.trades.size(), 1u);
    ASSERT_EQ(r.trades[0].qty, 80u);

    ASSERT_TRUE(book.hasBid());
    ASSERT_EQ(book.bestBidQty(), 120u);   // remainder
    ASSERT_FALSE(book.hasAsk());
}

TEST(partial_fill_aggressor) {
    // SELL 200 rests; BUY 80 aggresses → 80 filled, 120 remains on ask
    OrderBook book("AAPL");
    book.addLimit(makeLimit(1, "AAPL", 1, Side::SELL, 10000, 200));
    auto r = book.addLimit(makeLimit(2, "AAPL", 2, Side::BUY, 10000, 80));

    ASSERT_EQ(r.trades.size(), 1u);
    ASSERT_EQ(r.trades[0].qty, 80u);

    ASSERT_TRUE(book.hasAsk());
    ASSERT_EQ(book.bestAskQty(), 120u);
    ASSERT_FALSE(book.hasBid());
}

TEST(multi_level_sweep) {
    // Three ask levels; BUY 300 sweeps all of them.
    OrderBook book("AAPL");
    book.addLimit(makeLimit(1, "AAPL", 1, Side::SELL, 10000, 100));
    book.addLimit(makeLimit(2, "AAPL", 2, Side::SELL, 10010, 100));
    book.addLimit(makeLimit(3, "AAPL", 3, Side::SELL, 10020, 100));

    auto r = book.addLimit(makeLimit(4, "AAPL", 4, Side::BUY, 10050, 300));

    ASSERT_EQ(r.trades.size(), 3u);
    ASSERT_EQ(r.trades[0].price, 10000);
    ASSERT_EQ(r.trades[1].price, 10010);
    ASSERT_EQ(r.trades[2].price, 10020);
    ASSERT_FALSE(book.hasAsk());
    ASSERT_FALSE(book.hasBid());
}

TEST(fifo_within_price_level) {
    // Two SELL orders at same price; BUY matches first in (FIFO).
    OrderBook book("AAPL");
    book.addLimit(makeLimit(1, "AAPL", 10, Side::SELL, 10000, 50));
    book.addLimit(makeLimit(2, "AAPL", 20, Side::SELL, 10000, 50));

    auto r = book.addLimit(makeLimit(3, "AAPL", 30, Side::BUY, 10000, 50));

    ASSERT_EQ(r.trades.size(), 1u);
    ASSERT_EQ(r.trades[0].resting_order_id, 10u);   // first in = first matched

    // Order 20 is still on the ask side
    ASSERT_TRUE(book.hasAsk());
    ASSERT_EQ(book.bestAskQty(), 50u);
}

// ═════════════════════════════════════════════════════════════════════════════
// SECTION 3 – Cancel
// ═════════════════════════════════════════════════════════════════════════════

TEST(cancel_removes_order) {
    OrderBook book("AAPL");
    book.addLimit(makeLimit(1, "AAPL", 1, Side::BUY, 10000, 100));
    ASSERT_TRUE(book.hasBid());

    bool ok = book.cancel(1, 2);
    ASSERT_TRUE(ok);
    ASSERT_FALSE(book.hasBid());
}

TEST(cancel_nonexistent_returns_false) {
    OrderBook book("AAPL");
    bool ok = book.cancel(999, 1);
    ASSERT_FALSE(ok);
}

TEST(cancel_already_filled_returns_false) {
    OrderBook book("AAPL");
    book.addLimit(makeLimit(1, "AAPL", 1, Side::SELL, 10000, 50));
    book.addLimit(makeLimit(2, "AAPL", 2, Side::BUY,  10000, 50));
    // Order 1 is now FILLED
    bool ok = book.cancel(1, 3);
    ASSERT_FALSE(ok);
}

TEST(cancel_mid_level_fifo_preserved) {
    // Three asks at same price; cancel the middle one; FIFO of remainder intact.
    OrderBook book("AAPL");
    book.addLimit(makeLimit(1, "AAPL", 10, Side::SELL, 10000, 50));
    book.addLimit(makeLimit(2, "AAPL", 20, Side::SELL, 10000, 50));
    book.addLimit(makeLimit(3, "AAPL", 30, Side::SELL, 10000, 50));

    book.cancel(20, 4);   // cancel the middle order

    // Now ask level has [10, 30]; BUY 50 should match order 10 first.
    auto r = book.addLimit(makeLimit(4, "AAPL", 40, Side::BUY, 10000, 50));
    ASSERT_EQ(r.trades.size(), 1u);
    ASSERT_EQ(r.trades[0].resting_order_id, 10u);

    // Order 30 still rests.
    ASSERT_EQ(book.bestAskQty(), 50u);
}

// ═════════════════════════════════════════════════════════════════════════════
// SECTION 4 – Replace / Modify
// ═════════════════════════════════════════════════════════════════════════════

TEST(replace_loses_time_priority) {
    // Two buys at same price; second one is replaced → loses priority.
    OrderBook book("AAPL");
    book.addLimit(makeLimit(1, "AAPL", 1, Side::BUY, 10000, 100));
    book.addLimit(makeLimit(2, "AAPL", 2, Side::BUY, 10000, 100));

    // Replace order 2 with order 3 at same price/qty → goes to back of queue.
    OrderEvent rep;
    rep.timestamp    = 3;
    rep.symbol       = "AAPL";
    rep.type         = OrderType::REPLACE;
    rep.order_id     = 2;
    rep.new_order_id = 3;
    rep.new_price    = 10000;
    rep.new_qty      = 100;
    rep.side         = Side::BUY;
    book.replace(rep);

    // Now queue at 10000: [1, 3].  SELL 100 should match order 1.
    auto r = book.addLimit(makeLimit(4, "AAPL", 4, Side::SELL, 10000, 100));
    ASSERT_EQ(r.trades.size(), 1u);
    ASSERT_EQ(r.trades[0].resting_order_id, 1u);
}

TEST(replace_can_trigger_match) {
    // SELL rests; replace a BUY at lower price to a higher price → triggers match.
    OrderBook book("AAPL");
    book.addLimit(makeLimit(1, "AAPL", 1, Side::SELL, 10100, 50));
    book.addLimit(makeLimit(2, "AAPL", 2, Side::BUY,  9900,  50));  // no match

    OrderEvent rep;
    rep.timestamp    = 3;
    rep.symbol       = "AAPL";
    rep.type         = OrderType::REPLACE;
    rep.order_id     = 2;
    rep.new_order_id = 3;
    rep.new_price    = 10200;   // now crosses the ask
    rep.new_qty      = 50;
    rep.side         = Side::BUY;
    auto r = book.replace(rep);

    ASSERT_EQ(r.trades.size(), 1u);
    ASSERT_EQ(r.trades[0].price, 10100);   // matched at resting ask price
}

// ═════════════════════════════════════════════════════════════════════════════
// SECTION 5 – Market orders
// ═════════════════════════════════════════════════════════════════════════════

TEST(market_buy_sweeps_asks) {
    OrderBook book("AAPL");
    book.addLimit(makeLimit(1, "AAPL", 1, Side::SELL, 10000, 100));
    book.addLimit(makeLimit(2, "AAPL", 2, Side::SELL, 10010, 100));

    auto r = book.addMarket(makeMarket(3, "AAPL", 3, Side::BUY, 150));
    ASSERT_EQ(r.trades.size(), 2u);
    ASSERT_EQ(r.trades[0].qty, 100u);
    ASSERT_EQ(r.trades[1].qty,  50u);
    ASSERT_EQ(book.bestAskQty(), 50u);   // 50 left on second ask
}

TEST(market_sell_sweeps_bids) {
    OrderBook book("AAPL");
    book.addLimit(makeLimit(1, "AAPL", 1, Side::BUY, 10010, 100));
    book.addLimit(makeLimit(2, "AAPL", 2, Side::BUY, 10000, 100));

    auto r = book.addMarket(makeMarket(3, "AAPL", 3, Side::SELL, 120));
    ASSERT_EQ(r.trades.size(), 2u);
    ASSERT_EQ(r.trades[0].price, 10010);  // best bid matched first
    ASSERT_EQ(r.trades[0].qty,  100u);
    ASSERT_EQ(r.trades[1].qty,   20u);
    ASSERT_EQ(book.bestBidQty(), 80u);
}

TEST(market_no_liquidity_silent_discard) {
    OrderBook book("AAPL");
    auto r = book.addMarket(makeMarket(1, "AAPL", 1, Side::BUY, 100));
    ASSERT_EQ(r.trades.size(), 0u);
    ASSERT_FALSE(book.hasBid());
    ASSERT_FALSE(book.hasAsk());
}

// ═════════════════════════════════════════════════════════════════════════════
// SECTION 6 – Invariants
// ═════════════════════════════════════════════════════════════════════════════

TEST(book_never_crossed_after_match) {
    // Add several crossing orders and verify no crossing remains.
    OrderBook book("AAPL");
    book.addLimit(makeLimit(1, "AAPL", 1, Side::BUY,  10050, 200));
    book.addLimit(makeLimit(2, "AAPL", 2, Side::SELL, 10000, 100));
    book.addLimit(makeLimit(3, "AAPL", 3, Side::SELL, 10010, 100));
    book.addLimit(makeLimit(4, "AAPL", 4, Side::BUY,  10060,  50));
    ASSERT_FALSE(book.isCrossed());
}

TEST(tob_change_detected) {
    OrderBook book("AAPL");
    auto r1 = book.addLimit(makeLimit(1, "AAPL", 1, Side::BUY, 10000, 100));
    ASSERT_TRUE(r1.tob_changed);   // new best bid

    auto r2 = book.addLimit(makeLimit(2, "AAPL", 2, Side::BUY, 9900, 100));
    ASSERT_FALSE(r2.tob_changed);  // inferior bid, TOB unchanged

    auto r3 = book.addLimit(makeLimit(3, "AAPL", 3, Side::SELL, 10000, 100));
    ASSERT_TRUE(r3.tob_changed);   // trade occurred, bid side cleared
}

// ═════════════════════════════════════════════════════════════════════════════
// SECTION 7 – MessageParser
// ═════════════════════════════════════════════════════════════════════════════

TEST(parse_price_decimal) {
    ASSERT_EQ(MessageParser::parsePriceTicks("150.25"), 15025);
    ASSERT_EQ(MessageParser::parsePriceTicks("100.00"), 10000);
    ASSERT_EQ(MessageParser::parsePriceTicks("0.01"),       1);
    ASSERT_EQ(MessageParser::parsePriceTicks("10000"),  10000);  // integer ticks
}

TEST(parse_csv_line_limit) {
    auto ev = MessageParser::parseCSVLine(
        "1000000,AAPL,LIMIT,42,BUY,150.25,100");
    ASSERT_TRUE(ev.has_value());
    ASSERT_EQ(ev->timestamp, 1000000u);
    ASSERT_EQ(ev->symbol,    std::string("AAPL"));
    ASSERT_EQ(ev->type,      OrderType::LIMIT);
    ASSERT_EQ(ev->order_id,  42u);
    ASSERT_EQ(ev->side,      Side::BUY);
    ASSERT_EQ(ev->price,     15025);
    ASSERT_EQ(ev->qty,       100u);
}

TEST(parse_csv_line_cancel) {
    auto ev = MessageParser::parseCSVLine(
        "2000000,AAPL,CANCEL,42,BUY,0,0");
    ASSERT_TRUE(ev.has_value());
    ASSERT_EQ(ev->type,     OrderType::CANCEL);
    ASSERT_EQ(ev->order_id, 42u);
}

TEST(parse_csv_comment_ignored) {
    auto ev = MessageParser::parseCSVLine("# this is a comment");
    ASSERT_FALSE(ev.has_value());
}

TEST(parse_csv_replace) {
    auto ev = MessageParser::parseCSVLine(
        "3000000,AAPL,REPLACE,42,BUY,150.00,100,43,151.00,80");
    ASSERT_TRUE(ev.has_value());
    ASSERT_EQ(ev->type,         OrderType::REPLACE);
    ASSERT_EQ(ev->order_id,     42u);
    ASSERT_EQ(ev->new_order_id, 43u);
    ASSERT_EQ(ev->new_price,    15100);
    ASSERT_EQ(ev->new_qty,      80u);
}

// ═════════════════════════════════════════════════════════════════════════════
// SECTION 8 – MatchingEngine (multi-symbol)
// ═════════════════════════════════════════════════════════════════════════════

TEST(engine_multi_symbol_isolation) {
    MatchingEngine engine;
    int trade_count = 0;
    engine.setTradeHandler([&](const Trade&) { ++trade_count; });

    // Add liquidity to two symbols
    engine.processEvent(makeLimit(1, "AAPL", 1, Side::BUY,  15000, 100));
    engine.processEvent(makeLimit(2, "MSFT", 2, Side::SELL, 30000, 100));

    // Match on AAPL only
    engine.processEvent(makeLimit(3, "AAPL", 3, Side::SELL, 15000, 100));
    ASSERT_EQ(trade_count, 1);

    // MSFT book unchanged
    OrderBook* msft = engine.getBook("MSFT");
    ASSERT_TRUE(msft != nullptr);
    ASSERT_TRUE(msft->hasAsk());
}

TEST(engine_deterministic_replay) {
    // Same sequence of events → same trades every time.
    auto run = [&]() -> std::vector<Trade> {
        MatchingEngine engine;
        std::vector<Trade> out;
        engine.setTradeHandler([&](const Trade& t) { out.push_back(t); });

        engine.processEvent(makeLimit(1, "X", 1, Side::BUY,  10000, 200));
        engine.processEvent(makeLimit(2, "X", 2, Side::SELL, 9950,  100));
        engine.processEvent(makeLimit(3, "X", 3, Side::SELL, 10000, 150));
        return out;
    };

    auto r1 = run();
    auto r2 = run();
    ASSERT_EQ(r1.size(), r2.size());
    for (std::size_t i = 0; i < r1.size(); ++i) {
        ASSERT_EQ(r1[i].price, r2[i].price);
        ASSERT_EQ(r1[i].qty,   r2[i].qty);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
int main() { return run_all_tests(); }

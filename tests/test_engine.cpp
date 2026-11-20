// MatchingEngine: symbol routing, callback publication, determinism.

#include "helpers.h"
#include "lob/MatchingEngine.h"

#include <random>
#include <string>
#include <vector>

using namespace lob;

namespace {

OrderEvent lim(Timestamp ts, const char* sym, OrderId id, Side side, Price px, Qty qty) {
    return OrderEvent::limit(ts, Symbol(sym), id, side, px, qty);
}

}  // namespace

TEST(engine_routes_by_symbol) {
    MatchingEngine engine;
    int trades = 0;
    engine.setTradeHandler([&](const Trade&) { ++trades; });

    engine.processEvent(lim(1, "AAPL", 1, Side::BUY, 15000, 100));
    engine.processEvent(lim(2, "MSFT", 2, Side::SELL, 30000, 100));
    engine.processEvent(lim(3, "AAPL", 3, Side::SELL, 15000, 100));
    ASSERT_EQ(trades, 1);
    ASSERT_EQ(engine.bookCount(), 2u);

    OrderBook* msft = engine.getBook("MSFT");
    ASSERT_TRUE(msft != nullptr);
    ASSERT_TRUE(msft->hasAsk());
    ASSERT_FALSE(engine.getBook("AAPL")->hasBid());
    ASSERT_TRUE(engine.getBook("NOPE") == nullptr);
}

TEST(engine_order_ids_are_scoped_per_symbol) {
    MatchingEngine engine;
    ASSERT_TRUE(engine.processEvent(lim(1, "AAPL", 1, Side::BUY, 100, 1)).accepted());
    ASSERT_TRUE(engine.processEvent(lim(2, "MSFT", 1, Side::BUY, 100, 1)).accepted());
}

TEST(engine_publishes_trades_then_tob) {
    MatchingEngine engine;
    std::vector<std::string> log;
    engine.setTradeHandler([&](const Trade& t) { log.push_back("T" + std::to_string(t.qty)); });
    engine.setTOBHandler([&](const TOBUpdate& u) {
        log.push_back("B" + std::to_string(u.best_ask_qty));
    });

    engine.processEvent(lim(1, "X", 1, Side::SELL, 100, 10));
    engine.processEvent(lim(2, "X", 2, Side::SELL, 100, 10));
    engine.processEvent(lim(3, "X", 3, Side::BUY, 100, 15));
    const std::vector<std::string> expected = {"B10", "B20", "T10", "T5", "B5"};
    ASSERT_EQ(log, expected);
}

TEST(engine_tob_only_on_change) {
    MatchingEngine engine;
    int tob = 0;
    engine.setTOBHandler([&](const TOBUpdate&) { ++tob; });
    engine.processEvent(lim(1, "X", 1, Side::BUY, 100, 10));   // new best
    engine.processEvent(lim(2, "X", 2, Side::BUY, 90, 10));    // behind the touch
    engine.processEvent(OrderEvent::cancel(3, Symbol("X"), 2));          // deep cancel
    engine.processEvent(OrderEvent::cancel(4, Symbol("X"), 999));        // rejected
    ASSERT_EQ(tob, 1);
    engine.processEvent(OrderEvent::cancel(5, Symbol("X"), 1));          // best level gone
    ASSERT_EQ(tob, 2);
}

TEST(engine_tob_carries_event_timestamp) {
    MatchingEngine engine;
    TOBUpdate last{};
    engine.setTOBHandler([&](const TOBUpdate& u) { last = u; });
    engine.processEvent(lim(777, "X", 1, Side::BUY, 100, 10));
    ASSERT_EQ(last.timestamp, 777u);
    ASSERT_EQ(last.symbol, Symbol("X"));
    ASSERT_EQ(last.best_bid_qty, 10u);
}

TEST(engine_cancel_for_unknown_symbol_creates_no_book) {
    MatchingEngine engine;
    auto r = engine.processEvent(OrderEvent::cancel(1, Symbol("GHOST"), 1));
    ASSERT_EQ(r.reject_reason, RejectReason::UNKNOWN_ORDER);
    auto q = engine.processEvent(OrderEvent::replace(2, Symbol("GHOST"), 1, 2, 100, 1));
    ASSERT_EQ(q.reject_reason, RejectReason::UNKNOWN_ORDER);
    ASSERT_EQ(engine.bookCount(), 0u);
}

TEST(engine_deterministic_replay) {
    // Two engines fed the same random stream produce identical output.
    std::vector<OrderEvent> events;
    std::mt19937_64 rng(1234);
    const char* syms[] = {"AAA", "BBB", "CCC"};
    for (OrderId id = 1; id <= 20000; ++id) {
        const char* sym = syms[rng() % 3];
        const Side  side = (rng() & 1) != 0 ? Side::BUY : Side::SELL;
        const auto  px   = static_cast<Price>(990 + rng() % 21);
        const auto  qty  = static_cast<Qty>(1 + rng() % 50);
        if (rng() % 4 == 0)
            events.push_back(OrderEvent::cancel(id, Symbol(sym), static_cast<OrderId>(1 + rng() % id)));
        else
            events.push_back(lim(id, sym, id, side, px, qty));
    }

    auto run = [&]() {
        MatchingEngine engine;
        std::vector<Trade>     trades;
        std::vector<TOBUpdate> tobs;
        engine.setTradeHandler([&](const Trade& t) { trades.push_back(t); });
        engine.setTOBHandler([&](const TOBUpdate& u) { tobs.push_back(u); });
        for (const auto& ev : events) engine.processEvent(ev);
        return std::make_pair(trades, tobs);
    };
    const auto a = run();
    const auto b = run();
    ASSERT_GT(a.first.size(), 1000u);
    ASSERT_EQ(a.first.size(), b.first.size());
    ASSERT_TRUE(a.first == b.first);
    ASSERT_EQ(a.second.size(), b.second.size());
    for (std::size_t i = 0; i < a.second.size(); ++i) {
        ASSERT_EQ(a.second[i].best_bid_price, b.second[i].best_bid_price);
        ASSERT_EQ(a.second[i].best_ask_qty, b.second[i].best_ask_qty);
    }
}

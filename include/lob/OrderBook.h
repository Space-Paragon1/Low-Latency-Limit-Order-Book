#pragma once

#include "lob/OrderEvent.h"
#include "lob/Types.h"
#include "lob/detail/ObjectPool.h"
#include "lob/detail/OrderIdMap.h"
#include "lob/detail/PriceLadder.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace lob {

// Snapshot of one working order, as returned by OrderBook::findOrder().
struct OrderInfo {
    OrderId     id{0};
    Side        side{Side::BUY};
    OrderType   type{OrderType::LIMIT};   // LIMIT, or STOP_* while the stop is pending
    TimeInForce tif{TimeInForce::GTC};
    Price       price{0};                 // limit price, or stop price for a pending stop
    Price       limit_price{0};           // STOP_LIMIT only
    Qty         qty{0};                   // open quantity
    Timestamp   timestamp{0};             // time of entry (kept across priority-preserving replaces)
};

// One aggregated price level of an L2 snapshot.
struct DepthLevel {
    Price         price{0};
    Qty           qty{0};
    std::uint32_t orders{0};
};

// ─── Per-symbol limit order book with price-time priority matching ───────────
//
// Single-threaded by design: an exchange matching core is a deterministic
// state machine, and one thread per book (sharded by symbol) avoids every lock
// and cache-line handoff on the hot path.
//
// Complexity (L = number of price levels on a side, k = distance of the
// touched level from the top of the book, f = number of fills):
//   add (passive)            O(1) expected at an existing level; O(k + log L) to open one
//   cancel / reduce-replace  O(1) expected; O(k + log L) when it empties a level
//   match                    O(f)
//   best bid/ask price & qty O(1)
// No heap allocation on the hot path once the pools are warm.
class OrderBook {
public:
    explicit OrderBook(Symbol symbol, std::size_t expected_orders = 1024);

    OrderBook(const OrderBook&)            = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) noexcept            = default;
    OrderBook& operator=(OrderBook&&) noexcept = default;
    ~OrderBook()                               = default;

    // ── Mutations ─────────────────────────────────────────────────────────────
    MatchResult apply    (const OrderEvent& event);   // dispatches on event.type
    MatchResult addLimit (const OrderEvent& event);
    MatchResult addMarket(const OrderEvent& event);
    MatchResult addStop  (const OrderEvent& event);   // STOP_MARKET or STOP_LIMIT
    MatchResult cancel   (OrderId id);                // resting order or pending stop
    MatchResult replace  (const OrderEvent& event);

    // ── Top of book ───────────────────────────────────────────────────────────
    [[nodiscard]] bool  hasBid()       const noexcept { return !bids_.empty(); }
    [[nodiscard]] bool  hasAsk()       const noexcept { return !asks_.empty(); }
    [[nodiscard]] Price bestBidPrice() const noexcept { return bids_.empty() ? 0 : bids_.bestPrice(); }
    [[nodiscard]] Price bestAskPrice() const noexcept { return asks_.empty() ? 0 : asks_.bestPrice(); }
    [[nodiscard]] Qty   bestBidQty()   const noexcept { return bids_.empty() ? 0 : bids_.best()->total_qty; }
    [[nodiscard]] Qty   bestAskQty()   const noexcept { return asks_.empty() ? 0 : asks_.best()->total_qty; }
    [[nodiscard]] TOBUpdate makeTOB(Timestamp ts) const noexcept;

    // True if best bid >= best ask.  Never true between operations.
    [[nodiscard]] bool isCrossed() const noexcept;

    // Price of the most recent trade; 0 before the first trade.
    [[nodiscard]] Price lastTradePrice() const noexcept { return last_trade_price_; }

    // ── Inspection ────────────────────────────────────────────────────────────
    [[nodiscard]] const Symbol& symbol()           const noexcept { return symbol_; }
    [[nodiscard]] std::size_t   bidLevels()        const noexcept { return bids_.size(); }
    [[nodiscard]] std::size_t   askLevels()        const noexcept { return asks_.size(); }
    [[nodiscard]] std::size_t   orderCount()       const noexcept { return orders_.size(); }  // incl. stops
    [[nodiscard]] std::size_t   pendingStopCount() const noexcept { return pending_stops_; }
    [[nodiscard]] bool          contains(OrderId id) const noexcept { return orders_.find(id) != nullptr; }
    [[nodiscard]] std::optional<OrderInfo> findOrder(OrderId id) const;

    // Aggregated L2 depth, best level first, up to `max_levels` levels.
    [[nodiscard]] std::vector<DepthLevel> depth(Side side, std::size_t max_levels) const;

    // Full O(n) consistency check of every internal invariant (level ordering,
    // FIFO links, aggregate quantities, id-map coverage, pool accounting, no
    // crossed book).  For tests and debugging; returns false and fills `error`
    // on the first violation found.
    bool validate(std::string* error = nullptr) const;

private:
    using BidLadder = detail::PriceLadder<true>;    // highest first
    using AskLadder = detail::PriceLadder<false>;   // lowest first

    struct Top {
        Price bid_px, ask_px;
        Qty   bid_qty, ask_qty;
        bool operator==(const Top& o) const noexcept {
            return bid_px == o.bid_px && ask_px == o.ask_px && bid_qty == o.bid_qty &&
                   ask_qty == o.ask_qty;
        }
    };

    [[nodiscard]] Top top() const noexcept;
    void              beginOp() noexcept;
    MatchResult       finish(MatchResult r, const Top& before) noexcept;
    MatchResult       reject(RejectReason why, const Top& before) noexcept;

    [[nodiscard]] RejectReason checkNewOrder(OrderId id, Qty qty) const noexcept;
    [[nodiscard]] bool         wouldCross(Side side, Price price) const noexcept;
    [[nodiscard]] bool         canFill(Side side, Price limit, bool limited, Qty qty) const noexcept;

    void executeLimit (OrderId id, Side side, Price price, Qty qty, TimeInForce tif,
                       Timestamp ts, MatchResult& r);
    void executeMarket(OrderId id, Side side, Qty qty, TimeInForce tif, Timestamp ts,
                       MatchResult& r);
    void releaseStop  (const detail::OrderNode& stop, Timestamp ts, MatchResult& r);
    void rest         (OrderId id, Side side, Price price, Qty qty, TimeInForce tif, Timestamp ts);
    void removeNode   (detail::OrderNode* n) noexcept;
    void recordTrade  (Price px) noexcept;
    void runStops     (Timestamp ts);

    template <class Ladder>
    Qty match(Ladder& book, Side aggressor, Price limit, bool limited, Qty qty, OrderId id,
              Timestamp ts);
    template <class Ladder, class Elected>
    void collectStops(Ladder& stops, Elected elected);

    Symbol    symbol_;
    BidLadder bids_;
    AskLadder asks_;
    AskLadder buy_stops_;    // elected when a trade prints at or above the stop: lowest first
    BidLadder sell_stops_;   // elected when a trade prints at or below the stop: highest first

    detail::OrderIdMap<detail::OrderNode*> orders_;   // every working order, incl. stops
    detail::ObjectPool<detail::OrderNode>  order_pool_;
    detail::ObjectPool<detail::PriceLevel> level_pool_;

    std::vector<Trade>             trades_;    // per-operation output buffer (capacity reused)
    std::vector<detail::OrderNode> elected_;   // scratch: stops released this round

    std::size_t pending_stops_{0};
    Price       last_trade_price_{0};
    // Trade price range of the current stop-election round.
    Price       round_high_{0};
    Price       round_low_{0};
    bool        round_traded_{false};
};

}  // namespace lob

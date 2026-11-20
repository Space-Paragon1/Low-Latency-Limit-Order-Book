// Differential fuzz test.
//
// A deliberately naive reference book (flat vectors, linear scans, no
// intrusive lists, pools or hash maps) implements the same documented
// semantics as OrderBook.  Random order flow is driven through both, and after
// every operation the test compares status, reject reason, filled/leaves qty,
// every trade, the top of book and the tob_changed flag, then runs
// OrderBook::validate().  Any divergence prints the seed and step to replay.
//
// Scale with LOB_FUZZ_SEEDS (default 200 seeds x 2500 ops).

#include "helpers.h"

#include <algorithm>
#include <cstdlib>
#include <random>
#include <sstream>
#include <vector>

using namespace lob;

namespace {

class ReferenceBook {
public:
    struct Result {
        OrderStatus        status{OrderStatus::ACTIVE};
        RejectReason       reason{RejectReason::NONE};
        Qty                filled{0};
        Qty                leaves{0};
        std::vector<Trade> trades;
    };

    explicit ReferenceBook(Symbol sym) : sym_(sym) {}

    Result apply(const OrderEvent& ev) {
        trades_.clear();
        traded_ = false;
        Result r = dispatch(ev);
        r.trades = trades_;
        return r;
    }

    TOBUpdate top(Timestamp ts) const {
        TOBUpdate t{ts, sym_, 0, 0, 0, 0};
        for (const auto& o : orders_) {
            if (o.side == Side::BUY && (t.best_bid_price == 0 || o.price > t.best_bid_price))
                t.best_bid_price = o.price;
            if (o.side == Side::SELL && (t.best_ask_price == 0 || o.price < t.best_ask_price))
                t.best_ask_price = o.price;
        }
        for (const auto& o : orders_) {
            if (o.side == Side::BUY && o.price == t.best_bid_price) t.best_bid_qty += o.qty;
            if (o.side == Side::SELL && o.price == t.best_ask_price) t.best_ask_qty += o.qty;
        }
        return t;
    }

    std::size_t liveCount() const { return orders_.size() + stops_.size(); }

private:
    struct Order {
        OrderId       id;
        Side          side;
        Price         price;
        Qty           qty;
        std::uint64_t seq;
        TimeInForce   tif;
    };
    struct Stop {
        OrderId       id;
        OrderType     type;
        Side          side;
        Price         stop;
        Price         limit;
        Qty           qty;
        std::uint64_t seq;
    };

    static Result rej(RejectReason why) {
        Result r;
        r.status = OrderStatus::REJECTED;
        r.reason = why;
        return r;
    }

    bool live(OrderId id) const {
        for (const auto& o : orders_) if (o.id == id) return true;
        for (const auto& s : stops_)  if (s.id == id) return true;
        return false;
    }

    // Best resting order opposite an aggressor: best price, then earliest seq.
    int bestOpposite(Side aggressor) const {
        int best = -1;
        for (std::size_t i = 0; i < orders_.size(); ++i) {
            const Order& o = orders_[i];
            if (o.side == aggressor) continue;
            if (best < 0) { best = static_cast<int>(i); continue; }
            const Order& b = orders_[static_cast<std::size_t>(best)];
            const bool better = aggressor == Side::BUY ? o.price < b.price : o.price > b.price;
            if (better || (o.price == b.price && o.seq < b.seq)) best = static_cast<int>(i);
        }
        return best;
    }

    static bool reaches(Side aggressor, Price limit, Price px) {
        return aggressor == Side::BUY ? limit >= px : limit <= px;
    }

    bool wouldCross(Side side, Price price) const {
        const int i = bestOpposite(side);
        return i >= 0 && reaches(side, price, orders_[static_cast<std::size_t>(i)].price);
    }

    bool canFill(Side side, bool limited, Price limit, Qty qty) const {
        Qty sum = 0;
        for (const auto& o : orders_)
            if (o.side != side && (!limited || reaches(side, limit, o.price))) sum += o.qty;
        return sum >= qty;
    }

    void note(Price px) {
        last_ = px;
        if (!traded_) { hi_ = lo_ = px; traded_ = true; }
        else { hi_ = std::max(hi_, px); lo_ = std::min(lo_, px); }
    }

    Qty match(OrderId id, Side side, bool limited, Price limit, Qty qty, Timestamp ts) {
        while (qty > 0) {
            const int i = bestOpposite(side);
            if (i < 0) break;
            Order& o = orders_[static_cast<std::size_t>(i)];
            if (limited && !reaches(side, limit, o.price)) break;
            const Qty f = std::min(qty, o.qty);
            trades_.push_back(Trade{ts, sym_, o.price, f, side, o.id, id});
            note(o.price);
            qty -= f;
            o.qty -= f;
            if (o.qty == 0) orders_.erase(orders_.begin() + i);
        }
        return qty;
    }

    Result execLimit(OrderId id, Side side, Price px, Qty qty, TimeInForce tif, Timestamp ts) {
        if (tif == TimeInForce::POST_ONLY && wouldCross(side, px))
            return rej(RejectReason::POST_ONLY_WOULD_CROSS);
        Result r;
        if (tif == TimeInForce::FOK && !canFill(side, true, px, qty)) {
            r.status = OrderStatus::CANCELLED;
            return r;
        }
        const Qty rem = match(id, side, true, px, qty, ts);
        r.filled = qty - rem;
        if (rem == 0) r.status = OrderStatus::FILLED;
        else if (tif == TimeInForce::IOC || tif == TimeInForce::FOK) r.status = OrderStatus::CANCELLED;
        else {
            orders_.push_back(Order{id, side, px, rem, seq_++, tif});
            r.status = r.filled > 0 ? OrderStatus::PARTIALLY_FILLED : OrderStatus::ACTIVE;
            r.leaves = rem;
        }
        return r;
    }

    Result execMarket(OrderId id, Side side, Qty qty, TimeInForce tif, Timestamp ts) {
        Result r;
        if (tif == TimeInForce::FOK && !canFill(side, false, 0, qty)) {
            r.status = OrderStatus::CANCELLED;
            return r;
        }
        const Qty rem = match(id, side, false, 0, qty, ts);
        r.filled = qty - rem;
        r.status = rem == 0 ? OrderStatus::FILLED : OrderStatus::CANCELLED;
        return r;
    }

    Result release(const Stop& s, Timestamp ts) {
        return s.type == OrderType::STOP_MARKET
                   ? execMarket(s.id, s.side, s.qty, TimeInForce::GTC, ts)
                   : execLimit(s.id, s.side, s.limit, s.qty, TimeInForce::GTC, ts);
    }

    void runStops(Timestamp ts) {
        while (traded_ && !stops_.empty()) {
            const Price hi = hi_, lo = lo_;
            traded_ = false;
            std::vector<Stop> buys, sells, keep;
            for (const auto& s : stops_) {
                if (s.side == Side::BUY && s.stop <= hi)       buys.push_back(s);
                else if (s.side == Side::SELL && s.stop >= lo) sells.push_back(s);
                else                                           keep.push_back(s);
            }
            if (buys.empty() && sells.empty()) break;
            stops_ = keep;
            std::sort(buys.begin(), buys.end(), [](const Stop& a, const Stop& b) {
                return a.stop != b.stop ? a.stop < b.stop : a.seq < b.seq;
            });
            std::sort(sells.begin(), sells.end(), [](const Stop& a, const Stop& b) {
                return a.stop != b.stop ? a.stop > b.stop : a.seq < b.seq;
            });
            for (const auto& s : buys)  release(s, ts);
            for (const auto& s : sells) release(s, ts);
        }
    }

    Result dispatch(const OrderEvent& ev) {
        switch (ev.type) {
            case OrderType::LIMIT: {
                if (ev.qty == 0)      return rej(RejectReason::INVALID_QUANTITY);
                if (live(ev.order_id)) return rej(RejectReason::DUPLICATE_ORDER_ID);
                if (ev.price <= 0)    return rej(RejectReason::INVALID_PRICE);
                Result r = execLimit(ev.order_id, ev.side, ev.price, ev.qty, ev.tif, ev.timestamp);
                runStops(ev.timestamp);
                return r;
            }
            case OrderType::MARKET: {
                if (ev.qty == 0)      return rej(RejectReason::INVALID_QUANTITY);
                if (live(ev.order_id)) return rej(RejectReason::DUPLICATE_ORDER_ID);
                if (ev.tif == TimeInForce::POST_ONLY) return rej(RejectReason::INVALID_TIME_IN_FORCE);
                Result r = execMarket(ev.order_id, ev.side, ev.qty, ev.tif, ev.timestamp);
                runStops(ev.timestamp);
                return r;
            }
            case OrderType::CANCEL: {
                for (std::size_t i = 0; i < orders_.size(); ++i)
                    if (orders_[i].id == ev.order_id) {
                        orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i));
                        Result r; r.status = OrderStatus::CANCELLED; return r;
                    }
                for (std::size_t i = 0; i < stops_.size(); ++i)
                    if (stops_[i].id == ev.order_id) {
                        stops_.erase(stops_.begin() + static_cast<std::ptrdiff_t>(i));
                        Result r; r.status = OrderStatus::CANCELLED; return r;
                    }
                return rej(RejectReason::UNKNOWN_ORDER);
            }
            case OrderType::REPLACE: {
                std::size_t i = 0;
                while (i < orders_.size() && orders_[i].id != ev.order_id) ++i;
                if (i == orders_.size()) return rej(RejectReason::UNKNOWN_ORDER);
                if (ev.new_qty == 0)    return rej(RejectReason::INVALID_QUANTITY);
                if (ev.new_price <= 0)  return rej(RejectReason::INVALID_PRICE);
                if (ev.new_order_id != ev.order_id && live(ev.new_order_id))
                    return rej(RejectReason::DUPLICATE_ORDER_ID);
                Order& o = orders_[i];
                if (ev.new_price == o.price && ev.new_qty <= o.qty) {
                    o.qty = ev.new_qty;
                    o.id  = ev.new_order_id;
                    Result r; r.status = OrderStatus::ACTIVE; r.leaves = o.qty; return r;
                }
                const Side side = o.side;
                const TimeInForce tif = o.tif;
                if (tif == TimeInForce::POST_ONLY && wouldCross(side, ev.new_price))
                    return rej(RejectReason::POST_ONLY_WOULD_CROSS);
                orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i));
                Result r = execLimit(ev.new_order_id, side, ev.new_price, ev.new_qty, tif, ev.timestamp);
                runStops(ev.timestamp);
                return r;
            }
            case OrderType::STOP_MARKET:
            case OrderType::STOP_LIMIT: {
                if (ev.qty == 0)      return rej(RejectReason::INVALID_QUANTITY);
                if (live(ev.order_id)) return rej(RejectReason::DUPLICATE_ORDER_ID);
                if (ev.price <= 0)    return rej(RejectReason::INVALID_PRICE);
                if (ev.type == OrderType::STOP_LIMIT && ev.limit_price <= 0)
                    return rej(RejectReason::INVALID_PRICE);
                const Stop s{ev.order_id, ev.type, ev.side, ev.price,
                             ev.type == OrderType::STOP_LIMIT ? ev.limit_price : 0, ev.qty, seq_++};
                if (last_ != 0 && (ev.side == Side::BUY ? last_ >= ev.price : last_ <= ev.price)) {
                    Result r = release(s, ev.timestamp);
                    runStops(ev.timestamp);
                    return r;
                }
                stops_.push_back(s);
                Result r; r.status = OrderStatus::ACTIVE; r.leaves = ev.qty; return r;
            }
        }
        return rej(RejectReason::NONE);
    }

    Symbol             sym_;
    std::vector<Order> orders_;
    std::vector<Stop>  stops_;
    std::vector<Trade> trades_;
    std::uint64_t      seq_{0};
    Price              last_{0}, hi_{0}, lo_{0};
    bool               traded_{false};
};

// Random order flow concentrated in a narrow price band so that crossing,
// partial fills, multi-level sweeps and stop cascades are all frequent.
class FlowGenerator {
public:
    explicit FlowGenerator(std::uint64_t seed) : rng_(seed) {}

    OrderEvent next(Timestamp ts) {
        const int   roll = pct();
        const Side  side = coin() ? Side::BUY : Side::SELL;
        const Price px   = price(95, 105);
        const Qty   qty  = pct() < 2 ? 0 : static_cast<Qty>(between(1, 20));
        const Symbol& s  = th::kSym;

        if (roll < 40) return OrderEvent::limit(ts, s, newId(), side, px, qty, tif());
        if (roll < 50) {
            const int t = pct();
            const TimeInForce mtif = t < 80 ? TimeInForce::GTC : t < 90 ? TimeInForce::IOC
                                   : t < 95 ? TimeInForce::FOK : TimeInForce::POST_ONLY;
            return OrderEvent::market(ts, s, newId(), side, qty, mtif);
        }
        if (roll < 70) return OrderEvent::cancel(ts, s, existingId());
        if (roll < 85) {
            const OrderId old_id = existingId();
            const int     pick   = pct();
            const OrderId new_id = pick < 50 ? old_id : pick < 90 ? newId() : existingId();
            return OrderEvent::replace(ts, s, old_id, new_id, px, qty);
        }
        const Price stop = price(93, 107);
        if (coin()) return OrderEvent::stopMarket(ts, s, newId(), side, stop, qty);
        return OrderEvent::stopLimit(ts, s, newId(), side, stop, price(93, 107), qty);
    }

private:
    int  pct()  { return static_cast<int>(rng_() % 100); }
    bool coin() { return (rng_() & 1) != 0; }
    std::uint64_t between(std::uint64_t lo, std::uint64_t hi) { return lo + rng_() % (hi - lo + 1); }
    Price price(Price lo, Price hi) {
        if (pct() == 0) return 0;   // exercise INVALID_PRICE
        return lo + static_cast<Price>(rng_() % static_cast<std::uint64_t>(hi - lo + 1));
    }
    TimeInForce tif() {
        const int t = pct();
        return t < 70 ? TimeInForce::GTC : t < 80 ? TimeInForce::IOC
             : t < 90 ? TimeInForce::FOK : TimeInForce::POST_ONLY;
    }
    OrderId newId() {
        if (!used_.empty() && pct() < 5) return used_[rng_() % used_.size()];   // collide / reuse
        used_.push_back(next_id_);
        return next_id_++;
    }
    OrderId existingId() {
        if (!used_.empty() && pct() < 85) return used_[rng_() % used_.size()];
        return between(1, next_id_ + 10);
    }

    std::mt19937_64      rng_;
    OrderId              next_id_{1};
    std::vector<OrderId> used_;
};

std::string describe(const OrderEvent& ev) {
    std::ostringstream os;
    os << ev.type << " id=" << ev.order_id << ' ' << ev.side << " px=" << ev.price
       << " qty=" << ev.qty << " tif=" << ev.tif << " limit=" << ev.limit_price
       << " new_id=" << ev.new_order_id << " new_px=" << ev.new_price << " new_qty=" << ev.new_qty;
    return os.str();
}

bool sameTop(const TOBUpdate& a, const TOBUpdate& b) {
    return a.best_bid_price == b.best_bid_price && a.best_bid_qty == b.best_bid_qty &&
           a.best_ask_price == b.best_ask_price && a.best_ask_qty == b.best_ask_qty;
}

}  // namespace

TEST(fuzz_order_book_matches_reference_model) {
    std::uint64_t seeds = 200;
    if (const char* env = std::getenv("LOB_FUZZ_SEEDS")) seeds = std::strtoull(env, nullptr, 10);
    constexpr int kOps = 2500;

    std::uint64_t total_trades = 0, total_rejects = 0, total_ops = 0;
    for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
        OrderBook     book(th::kSym, 64);
        ReferenceBook ref(th::kSym);
        FlowGenerator gen(seed);

        for (int step = 0; step < kOps; ++step) {
            const auto       ts       = static_cast<Timestamp>(step + 1);
            const OrderEvent ev       = gen.next(ts);
            const TOBUpdate  ref_prev = ref.top(ts);
            const MatchResult got     = book.apply(ev);
            const auto        want    = ref.apply(ev);
            const TOBUpdate   ref_now = ref.top(ts);

            auto fail = [&](const std::string& what) {
                FAIL("seed " << seed << " step " << step << ": " << what << "\n         event: "
                             << describe(ev));
            };
            if (got.status != want.status)
                fail("status " + std::string(toString(got.status)) + " vs reference " +
                     std::string(toString(want.status)));
            if (got.reject_reason != want.reason)
                fail("reject " + std::string(toString(got.reject_reason)) + " vs reference " +
                     std::string(toString(want.reason)));
            if (got.filled_qty != want.filled || got.leaves_qty != want.leaves)
                fail("filled/leaves " + std::to_string(got.filled_qty) + "/" +
                     std::to_string(got.leaves_qty) + " vs reference " +
                     std::to_string(want.filled) + "/" + std::to_string(want.leaves));
            if (got.trades.size() != want.trades.size())
                fail("trade count " + std::to_string(got.trades.size()) + " vs reference " +
                     std::to_string(want.trades.size()));
            for (std::size_t i = 0; i < want.trades.size(); ++i)
                if (got.trades[i] != want.trades[i]) fail("trade #" + std::to_string(i) + " differs");
            if (!sameTop(book.makeTOB(ts), ref_now)) fail("top of book differs");
            if (got.tob_changed != !sameTop(ref_prev, ref_now)) fail("tob_changed flag differs");
            if (book.orderCount() != ref.liveCount()) fail("live order count differs");
            std::string err;
            if (!book.validate(&err)) fail("invariant violated: " + err);

            total_trades += want.trades.size();
            total_rejects += got.accepted() ? 0 : 1;
            ++total_ops;
        }
    }
    // Guard against a generator that silently stops exercising the engine.
    ASSERT_GT(total_trades, total_ops / 4);
    ASSERT_GT(total_rejects, total_ops / 50);
}

// lob_bench — throughput and per-operation latency of the matching engine.
//
//   lob_bench [events]
//   lob_bench --events N --scenario realistic|deep --seed S --repeat R
//
// Scenarios
//   realistic  Random-walk mid price; passive orders cluster near the touch
//              (geometric distance), ~8% marketable limits, ~35% cancels of
//              live orders, qty-down and price-move replaces, market orders and
//              stops.  The book stays at a steady size, like a real session.
//   deep       The original v1 workload: non-crossing limits on 20 levels per
//              side, 20% cancels, 10% markets, 10% replaces.  The book grows to
//              hundreds of thousands of orders, which stresses cancel at depth.
//
// The event stream is generated up front against a shadow book so cancels and
// replaces target orders that are actually live; replaying it is then fully
// deterministic.  Throughput is measured without per-event timing (median of
// --repeat runs); latency is measured in a separate pass with two clock reads
// per event, so it includes clock overhead (printed below the table).

#include "lob/MatchingEngine.h"
#include "lob/OrderBook.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using namespace lob;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
    std::size_t events   = 1'000'000;
    std::string scenario = "realistic";
    unsigned    seed     = 42;
    int         repeat   = 5;
};

struct Workload {
    std::vector<OrderEvent> events;
    std::size_t             peak_orders = 0;
    std::uint64_t           trades      = 0;
    std::size_t             final_orders = 0;
    std::size_t             final_levels = 0;
};

const Symbol kSym("SYM");

// ─── Workload generation ──────────────────────────────────────────────────────

class Generator {
public:
    Generator(const Options& opt) : rng_(opt.seed), shadow_(kSym, 1 << 16) {}

    Workload realistic(std::size_t n) {
        Workload w;
        w.events.reserve(n);
        Price mid = 100'000;   // $1000.00
        for (std::size_t i = 0; i < n; ++i) {
            const auto ts = static_cast<Timestamp>(i) * 50;
            if (rng_() % 64 == 0) mid += coin() ? 1 : -1;
            const int roll = pct();
            OrderEvent ev;
            if (roll < 40) {
                const Side  side = coin() ? Side::BUY : Side::SELL;
                const Price d    = geometric(0.25, 200);
                const Price px   = side == Side::BUY ? mid - 1 - d : mid + 1 + d;
                ev = OrderEvent::limit(ts, kSym, next_id_++, side, px, lot());
            } else if (roll < 48) {
                const Side  side = coin() ? Side::BUY : Side::SELL;
                const Price px   = side == Side::BUY ? mid + 3 : mid - 3;
                ev = OrderEvent::limit(ts, kSym, next_id_++, side, px, lot(),
                                       coin() ? TimeInForce::IOC : TimeInForce::GTC);
            } else if (roll < 83) {
                if (!pickLive(ev, ts, /*replace=*/false, mid)) continue;
            } else if (roll < 88) {
                ev = OrderEvent::market(ts, kSym, next_id_++, coin() ? Side::BUY : Side::SELL,
                                        lot() / 2 + 1);
            } else if (roll < 99) {
                if (!pickLive(ev, ts, /*replace=*/true, mid)) continue;
            } else {
                const Side  side = coin() ? Side::BUY : Side::SELL;
                const Price off  = 5 + geometric(0.2, 50);
                ev = side == Side::BUY
                         ? OrderEvent::stopMarket(ts, kSym, next_id_++, side, mid + off, lot())
                         : OrderEvent::stopMarket(ts, kSym, next_id_++, side, mid - off, lot());
            }
            push(w, ev);
        }
        return finish(w);
    }

    Workload deep(std::size_t n) {
        Workload w;
        w.events.reserve(n);
        constexpr Price mid = 10'000, half_spread = 50;
        for (std::size_t i = 0; i < n; ++i) {
            const auto ts   = static_cast<Timestamp>(i) * 100;
            const int  roll = static_cast<int>(rng_() % 10);
            OrderEvent ev;
            if (roll < 6 || live_.empty()) {
                const Side  side = coin() ? Side::BUY : Side::SELL;
                const Price off  = static_cast<Price>(rng_() % 20) * 10;
                const Price px   = side == Side::BUY ? mid - half_spread - off : mid + half_spread + off;
                ev = OrderEvent::limit(ts, kSym, next_id_++, side, px, 10 + rng_() % 491);
            } else if (roll < 8) {
                if (!pickLive(ev, ts, false, mid)) continue;
            } else if (roll < 9) {
                ev = OrderEvent::market(ts, kSym, next_id_++, coin() ? Side::BUY : Side::SELL,
                                        10 + rng_() % 491);
            } else {
                if (!pickLive(ev, ts, true, mid, /*deep=*/true)) continue;
            }
            push(w, ev);
        }
        return finish(w);
    }

private:
    int  pct()  { return static_cast<int>(rng_() % 100); }
    bool coin() { return (rng_() & 1) != 0; }
    Qty  lot()  { return 1 + rng_() % 10 * 10; }

    Price geometric(double p, Price cap) {
        std::geometric_distribution<int> g(p);
        return std::min<Price>(g(rng_), cap);
    }

    // Picks a live resting order (lazily discarding dead ids) and builds a
    // cancel or replace for it.
    bool pickLive(OrderEvent& ev, Timestamp ts, bool replace, Price mid, bool deep = false) {
        while (!live_.empty()) {
            const std::size_t i  = rng_() % live_.size();
            const OrderId     id = live_[i];
            const auto        o  = shadow_.findOrder(id);
            if (!o || o->type != OrderType::LIMIT) {
                live_[i] = live_.back();
                live_.pop_back();
                continue;
            }
            if (!replace) {
                ev = OrderEvent::cancel(ts, kSym, id);
                return true;
            }
            if (!deep && coin() && o->qty > 1) {   // qty-down in place: keeps priority
                ev = OrderEvent::replace(ts, kSym, id, id, o->price, o->qty - 1 - rng_() % (o->qty - 1));
                return true;
            }
            Price px;
            if (deep) {
                const Price off = static_cast<Price>(rng_() % 20) * 10;
                px = o->side == Side::BUY ? 10'000 - 50 - off : 10'000 + 50 + off;
            } else {
                const Price d = geometric(0.25, 200);
                px = o->side == Side::BUY ? mid - 1 - d : mid + 1 + d;
            }
            ev = OrderEvent::replace(ts, kSym, id, next_id_++, px, lot());
            return true;
        }
        return false;
    }

    void push(Workload& w, const OrderEvent& ev) {
        const MatchResult r = shadow_.apply(ev);
        w.trades += r.trades.size();
        if (r.leaves_qty > 0) {
            const OrderId id = ev.type == OrderType::REPLACE ? ev.new_order_id : ev.order_id;
            if (ev.type != OrderType::REPLACE || id != ev.order_id) live_.push_back(id);
        }
        w.peak_orders = std::max(w.peak_orders, shadow_.orderCount());
        w.events.push_back(ev);
    }

    Workload& finish(Workload& w) {
        w.final_orders = shadow_.orderCount();
        w.final_levels = shadow_.bidLevels() + shadow_.askLevels();
        return w;
    }

    std::mt19937_64      rng_;
    OrderBook            shadow_;
    std::vector<OrderId> live_;
    OrderId              next_id_{1};
};

// ─── Measurement ──────────────────────────────────────────────────────────────

std::uint64_t g_sink = 0;   // keeps callback work observable

MatchingEngine makeEngine(const Workload& w) {
    MatchingEngine engine(w.peak_orders + w.peak_orders / 4 + 1024);
    engine.setTradeHandler([](const Trade& t) { g_sink += t.qty; });
    engine.setTOBHandler([](const TOBUpdate& u) { g_sink += u.best_bid_qty; });
    engine.getOrCreateBook(kSym);   // allocate (and pre-fault) outside the timed region
    return engine;
}

double throughputRun(const Workload& w) {
    MatchingEngine engine = makeEngine(w);
    const auto t0 = Clock::now();
    for (const auto& ev : w.events) g_sink += engine.processEvent(ev).filled_qty;
    const auto t1 = Clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

enum Op { kLimit, kMarket, kCancel, kReplace, kStop, kOpCount };
constexpr std::array<const char*, kOpCount> kOpNames = {"limit", "market", "cancel", "replace", "stop"};

Op opOf(OrderType t) {
    switch (t) {
        case OrderType::LIMIT:       return kLimit;
        case OrderType::MARKET:      return kMarket;
        case OrderType::CANCEL:      return kCancel;
        case OrderType::REPLACE:     return kReplace;
        case OrderType::STOP_MARKET:
        case OrderType::STOP_LIMIT:  return kStop;
    }
    return kLimit;
}

std::array<std::vector<std::int64_t>, kOpCount> latencyRun(const Workload& w) {
    std::array<std::vector<std::int64_t>, kOpCount> lat;
    for (auto& v : lat) v.reserve(w.events.size() / 2);
    MatchingEngine engine = makeEngine(w);
    for (const auto& ev : w.events) {
        const auto t0 = Clock::now();
        g_sink += engine.processEvent(ev).filled_qty;
        const auto t1 = Clock::now();
        lat[opOf(ev.type)].push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
    return lat;
}

std::int64_t pctile(const std::vector<std::int64_t>& sorted, double p) {
    if (sorted.empty()) return 0;
    auto idx = static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(sorted.size())));
    return sorted[std::min(sorted.size() - 1, idx == 0 ? 0 : idx - 1)];
}

void printRow(const char* name, std::vector<std::int64_t>& v) {
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (auto x : v) sum += static_cast<double>(x);
    std::printf("  %-8s %10zu %8.0f %7lld %7lld %7lld %8lld %9lld\n", name, v.size(),
                sum / static_cast<double>(v.size()), static_cast<long long>(pctile(v, 50)),
                static_cast<long long>(pctile(v, 90)), static_cast<long long>(pctile(v, 99)),
                static_cast<long long>(pctile(v, 99.9)), static_cast<long long>(v.back()));
}

void clockCharacteristics(double& overhead_ns, std::int64_t& resolution_ns) {
    constexpr int kN = 1'000'000;
    std::int64_t  min_step = INT64_MAX;
    auto          prev     = Clock::now();
    const auto    start    = prev;
    for (int i = 0; i < kN; ++i) {
        const auto now  = Clock::now();
        const auto step = std::chrono::duration_cast<std::chrono::nanoseconds>(now - prev).count();
        if (step > 0) min_step = std::min(min_step, step);
        prev = now;
    }
    overhead_ns   = std::chrono::duration<double, std::nano>(prev - start).count() / kN;
    resolution_ns = min_step;
}

const char* compilerName() {
#if defined(__clang__)
    return "clang " __clang_version__;
#elif defined(__GNUC__)
    return "gcc " __VERSION__;
#elif defined(_MSC_VER)
    static std::string s = "msvc " + std::to_string(_MSC_FULL_VER);
    return s.c_str();
#else
    return "unknown compiler";
#endif
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--events" || a == "-n") {
            const char* v = value();
            if (!v) return false;
            o.events = std::strtoull(v, nullptr, 10);
        } else if (a == "--scenario") {
            const char* v = value();
            if (!v) return false;
            o.scenario = v;
        } else if (a == "--seed") {
            const char* v = value();
            if (!v) return false;
            o.seed = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
        } else if (a == "--repeat") {
            const char* v = value();
            if (!v) return false;
            o.repeat = std::max(1, std::atoi(v));
        } else if (!a.empty() && a[0] != '-') {
            o.events = std::strtoull(argv[i], nullptr, 10);
        } else {
            return false;
        }
    }
    return o.events > 0 && (o.scenario == "realistic" || o.scenario == "deep");
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) {
        std::cerr << "usage: " << argv[0]
                  << " [events] [--events N] [--scenario realistic|deep] [--seed S] [--repeat R]\n";
        return 1;
    }

    Generator gen(opt);
    Workload  w = opt.scenario == "deep" ? gen.deep(opt.events) : gen.realistic(opt.events);

    std::array<std::size_t, kOpCount> mix{};
    for (const auto& ev : w.events) ++mix[opOf(ev.type)];

    std::printf("Matching engine benchmark\n");
    std::printf("  scenario   : %s (seed %u)\n", opt.scenario.c_str(), opt.seed);
    std::printf("  compiler   : %s\n", compilerName());
    std::printf("  events     : %zu  (", w.events.size());
    for (int i = 0; i < kOpCount; ++i)
        std::printf("%s%s %.1f%%", i ? ", " : "", kOpNames[static_cast<std::size_t>(i)],
                    100.0 * static_cast<double>(mix[static_cast<std::size_t>(i)]) /
                        static_cast<double>(w.events.size()));
    std::printf(")\n");
    std::printf("  trades     : %llu\n", static_cast<unsigned long long>(w.trades));
    std::printf("  book       : peak %zu orders; final %zu orders on %zu levels\n\n",
                w.peak_orders, w.final_orders, w.final_levels);

    throughputRun(w);   // warm-up: caches, branch predictors, allocator
    std::vector<double> runs;
    for (int i = 0; i < opt.repeat; ++i) runs.push_back(throughputRun(w));
    std::sort(runs.begin(), runs.end());
    const double median_ns = runs[runs.size() / 2];
    const double per_event = median_ns / static_cast<double>(w.events.size());
    std::printf("Throughput (median of %d): %.2f M events/s   (%.1f ns/event, best %.1f ns)\n\n",
                opt.repeat, 1e3 / per_event, per_event,
                runs.front() / static_cast<double>(w.events.size()));

    auto lat = latencyRun(w);
    std::vector<std::int64_t> all;
    for (const auto& v : lat) all.insert(all.end(), v.begin(), v.end());

    std::printf("Latency per event, ns\n");
    std::printf("  %-8s %10s %8s %7s %7s %7s %8s %9s\n", "op", "count", "mean", "p50", "p90",
                "p99", "p99.9", "max");
    for (int i = 0; i < kOpCount; ++i) printRow(kOpNames[static_cast<std::size_t>(i)], lat[static_cast<std::size_t>(i)]);
    printRow("all", all);

    double       overhead = 0;
    std::int64_t resolution = 0;
    clockCharacteristics(overhead, resolution);
    std::printf("\n  steady_clock: ~%.0f ns per read, %lld ns resolution (latencies include one read)\n",
                overhead, static_cast<long long>(resolution));
    return g_sink == 42 ? 2 : 0;   // never true in practice; defeats dead-code elimination
}

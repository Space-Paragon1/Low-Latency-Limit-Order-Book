#include "MatchingEngine.h"
#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>
#include <iostream>
#include <random>
#include <iomanip>
#include <cmath>

// ─── Timing helpers ───────────────────────────────────────────────────────────
using Clock     = std::chrono::high_resolution_clock;
using Duration  = std::chrono::nanoseconds;
using TimePoint = Clock::time_point;

static inline TimePoint now() { return Clock::now(); }
static inline int64_t   ns(TimePoint a, TimePoint b) {
    return std::chrono::duration_cast<Duration>(b - a).count();
}

// ─── Percentile (in-place, destructive) ──────────────────────────────────────
static int64_t percentile(std::vector<int64_t>& v, double pct) {
    if (v.empty()) return 0;
    std::size_t idx = static_cast<std::size_t>(
        std::ceil(pct / 100.0 * static_cast<double>(v.size())) ) - 1;
    std::nth_element(v.begin(), v.begin() + static_cast<ptrdiff_t>(idx), v.end());
    return v[idx];
}

// ─── Synthetic event generator ────────────────────────────────────────────────
// Mix: 60% LIMIT, 20% CANCEL, 10% MARKET, 10% REPLACE
// Prices centered around 10000 ticks ± small spread to generate frequent matches.
struct BenchConfig {
    int      total_events  = 1'000'000;
    int      symbol_count  = 1;          // single-symbol for worst-case depth
    Price    mid_price     = 10000;
    Price    half_spread   = 50;         // 50 ticks = $0.50
    int      price_levels  = 20;         // ±20 levels around mid
    Qty      min_qty       = 10;
    Qty      max_qty       = 500;
    unsigned seed          = 42;
};

static std::vector<OrderEvent>
generateEvents(const BenchConfig& cfg) {
    std::mt19937                          rng(cfg.seed);
    std::uniform_int_distribution<int>   type_die(0, 9);
    std::uniform_int_distribution<int>   level_die(0, cfg.price_levels - 1);
    std::uniform_int_distribution<Qty>   qty_die(cfg.min_qty, cfg.max_qty);
    std::uniform_int_distribution<int>   side_die(0, 1);

    std::vector<OrderEvent> events;
    events.reserve(static_cast<std::size_t>(cfg.total_events));

    std::vector<OrderId> live_orders;   // pool to cancel/replace from
    live_orders.reserve(100'000);

    OrderId next_id = 1;
    Timestamp ts    = 1'000'000'000;    // 1 s in ns

    for (int i = 0; i < cfg.total_events; ++i) {
        OrderEvent ev;
        ev.timestamp = ts + static_cast<Timestamp>(i) * 100;  // 100 ns spacing
        ev.symbol    = "SYM";

        int roll = type_die(rng);

        if (roll < 6 || live_orders.empty()) {
            // ── LIMIT order ─────────────────────────────────────────────────
            ev.type     = OrderType::LIMIT;
            ev.order_id = next_id++;
            ev.side     = (side_die(rng) == 0) ? Side::BUY : Side::SELL;
            ev.qty      = qty_die(rng);

            int lvl = level_die(rng);
            Price offset = static_cast<Price>(lvl) * 10;
            if (ev.side == Side::BUY)
                ev.price = cfg.mid_price - cfg.half_spread - offset;
            else
                ev.price = cfg.mid_price + cfg.half_spread + offset;

            live_orders.push_back(ev.order_id);

        } else if (roll < 8) {
            // ── CANCEL ──────────────────────────────────────────────────────
            ev.type = OrderType::CANCEL;
            std::uniform_int_distribution<std::size_t>
                pick(0, live_orders.size() - 1);
            std::size_t idx = pick(rng);
            ev.order_id = live_orders[idx];
            live_orders.erase(live_orders.begin() + static_cast<ptrdiff_t>(idx));

        } else if (roll < 9) {
            // ── MARKET ──────────────────────────────────────────────────────
            ev.type     = OrderType::MARKET;
            ev.order_id = next_id++;
            ev.side     = (side_die(rng) == 0) ? Side::BUY : Side::SELL;
            ev.qty      = qty_die(rng);

        } else {
            // ── REPLACE ─────────────────────────────────────────────────────
            ev.type = OrderType::REPLACE;
            std::uniform_int_distribution<std::size_t>
                pick(0, live_orders.size() - 1);
            std::size_t idx = pick(rng);
            ev.order_id     = live_orders[idx];
            ev.new_order_id = next_id++;
            ev.new_qty      = qty_die(rng);
            int lvl         = level_die(rng);
            Price offset    = static_cast<Price>(lvl) * 10;
            // Reuse same side heuristic (BUY if even ID)
            if (ev.order_id % 2 == 0) {
                ev.side      = Side::BUY;
                ev.new_price = cfg.mid_price - cfg.half_spread - offset;
            } else {
                ev.side      = Side::SELL;
                ev.new_price = cfg.mid_price + cfg.half_spread + offset;
            }
            live_orders[idx] = ev.new_order_id;
        }

        events.push_back(ev);
    }
    return events;
}

// ─── Run one benchmark pass ───────────────────────────────────────────────────
static void runBenchmark(const BenchConfig& cfg, bool warmup = false) {
    auto events = generateEvents(cfg);

    MatchingEngine engine;
    // Discard callbacks (no I/O overhead)
    engine.setTradeHandler([](const Trade&)     {});
    engine.setTOBHandler  ([](const TOBUpdate&) {});

    std::vector<int64_t> latencies;
    latencies.reserve(events.size());

    // ── Timed loop ────────────────────────────────────────────────────────────
    auto wall_start = now();

    for (const auto& ev : events) {
        auto t0 = now();
        engine.processEvent(ev);
        auto t1 = now();
        latencies.push_back(ns(t0, t1));
    }

    auto wall_end = now();

    if (warmup) return;

    // ── Statistics ────────────────────────────────────────────────────────────
    double   wall_ms  = static_cast<double>(ns(wall_start, wall_end)) / 1e6;
    double   msgs_sec = static_cast<double>(events.size()) / (wall_ms / 1000.0);

    int64_t sum  = std::accumulate(latencies.begin(), latencies.end(), int64_t(0));
    int64_t mean = sum / static_cast<int64_t>(latencies.size());

    // Compute percentiles (nth_element is in-place / destructive → copy first)
    std::vector<int64_t> lat_copy = latencies;
    int64_t p50 = percentile(lat_copy, 50);
    lat_copy    = latencies;
    int64_t p95 = percentile(lat_copy, 95);
    lat_copy    = latencies;
    int64_t p99 = percentile(lat_copy, 99);
    lat_copy    = latencies;
    int64_t p999= percentile(lat_copy, 99.9);
    int64_t p_max = *std::max_element(latencies.begin(), latencies.end());

    std::cout
        << "╔══════════════════════════════════════════════════╗\n"
        << "║           Matching Engine Benchmark              ║\n"
        << "╠══════════════════════════════════════════════════╣\n"
        << "║ Events processed  : " << std::setw(12) << events.size()          << "             ║\n"
        << "║ Wall time         : " << std::setw(10) << std::fixed
                                    << std::setprecision(2) << wall_ms << " ms"<< "           ║\n"
        << "║ Throughput        : " << std::setw(10)
                                    << static_cast<uint64_t>(msgs_sec)
                                    << " msgs/sec      ║\n"
        << "╠══════════════════════════════════════════════════╣\n"
        << "║ Latency per event (ns)                           ║\n"
        << "║   Mean            : " << std::setw(12) << mean                   << "             ║\n"
        << "║   p50             : " << std::setw(12) << p50                    << "             ║\n"
        << "║   p95             : " << std::setw(12) << p95                    << "             ║\n"
        << "║   p99             : " << std::setw(12) << p99                    << "             ║\n"
        << "║   p99.9           : " << std::setw(12) << p999                   << "             ║\n"
        << "║   max             : " << std::setw(12) << p_max                  << "             ║\n"
        << "╚══════════════════════════════════════════════════╝\n";
}

// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    BenchConfig cfg;

    // Simple arg parsing: lob_bench [total_events]
    if (argc >= 2) cfg.total_events = std::stoi(argv[1]);

    std::cout << "[INFO] Warming up (" << cfg.total_events / 10 << " events)…\n";
    BenchConfig warm = cfg;
    warm.total_events /= 10;
    runBenchmark(warm, /*warmup=*/true);

    std::cout << "[INFO] Running benchmark (" << cfg.total_events << " events)…\n\n";
    runBenchmark(cfg);

    return 0;
}

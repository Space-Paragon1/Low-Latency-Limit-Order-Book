# Low-Latency Limit Order Book + Matching Engine

> "Built a deterministic, price-time priority matching engine with an in-memory L2 book,
> replay + validation harness, and performance benchmarks."

---

## Overview

This project implements a production-style **exchange matching core** in **C++17**.
It accepts a stream of order messages, maintains a live per-symbol order book, matches
buyers/sellers, and outputs trade prints and top-of-book (TOB) market data.

### Key properties

| Property | Detail |
|---|---|
| **Priority** | Price-time (FIFO within each price level) |
| **Order types** | LIMIT, MARKET, CANCEL, REPLACE, STOP_MARKET, STOP_LIMIT |
| **Fill types** | Full fill & partial fill |
| **Trade price** | Resting order's price |
| **Market orders** | Never rest; unmatched qty silently discarded |
| **Replace** | Cancels original (loses time priority) → submits new limit |
| **Determinism** | Same input → identical output, always |

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    lob_main / lob_bench                  │
└────────────────┬────────────────────────────────────────┘
                 │ OrderEvent stream
                 ▼
        ┌─────────────────┐
        │ MatchingEngine  │   owns one OrderBook per symbol
        └────────┬────────┘
                 │ callbacks
       ┌─────────┴──────────┐
       ▼                    ▼
 TradeHandler          TOBHandler
       │                    │
       ▼                    ▼
 MarketDataPublisher (trades.csv / tob.csv)
```

### Core data structures

```
OrderBook (per symbol)
├── bids_      : std::map<Price, std::deque<OrderId>, std::greater<Price>>
│                 highest bid price → front; FIFO deque per level
├── asks_      : std::map<Price, std::deque<OrderId>>
│                 lowest ask price → front; FIFO deque per level
├── orders_    : std::unordered_map<OrderId, Order>
│                 authoritative order state (price, qty_remaining, status)
├── buy_stops_ : std::map<Price, StopOrder>   (ascending — trigger lowest first)
└── sell_stops_: std::map<Price, StopOrder, std::greater<Price>>
                  (descending — trigger highest first)
```

`std::map` gives **O(log n)** best-price access and level iteration.
`std::unordered_map` gives **O(1)** average order lookup for cancel/replace.
Price levels are cleaned up automatically when their deque empties.

---

## Build

### Prerequisites

- CMake ≥ 3.15
- C++17 compiler (GCC 9+, Clang 10+, MSVC 2019+)

### Steps

**Linux / macOS (GCC or Clang)**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
# Executables: build/lob_main, build/lob_tests, build/lob_bench
```

**Windows (MSVC via PowerShell)**
```powershell
# Loads VS build tools, configures, builds, and runs all tests:
powershell -ExecutionPolicy Bypass -File configure.ps1
```

---

## Usage

### Step 1 — Build everything + run all tests

```powershell
powershell -ExecutionPolicy Bypass -File configure.ps1
```

This loads the VS 2022 build tools, compiles all targets (`lob_core`, `lob_main`,
`lob_tests`, `lob_bench`) and runs the full unit test suite automatically.

---

### Step 2 — Run unit tests

```powershell
.\build\lob_tests.exe
```

Expected output:
```
[ RUN  ] simple_buy_sell_exact
[ OK   ] simple_buy_sell_exact
[ RUN  ] buy_aggresses_resting_ask
[ OK   ] buy_aggresses_resting_ask
...
34 / 34 tests passed
```

---

### Step 3 — Replay mode

```powershell
.\build\lob_main.exe data\sample_orders.csv output\trades.csv output\tob.csv
```

Sample output:
```
[INFO ] Parsed 24 events from data\sample_orders.csv
[INFO ] Replay complete in 0.48 ms
[INFO ] Throughput      : 50209 msgs/sec
[INFO ] Trades emitted  : 12
[INFO ] TOB updates     : 16
[INFO ] trades  → output\trades.csv
[INFO ] tob     → output\tob.csv
```

Results are written to:
- `output\trades.csv` — execution reports
- `output\tob.csv`   — top-of-book snapshots

---

### Step 4 — Benchmark

```powershell
# 1,000,000 synthetic events (default)
.\build\lob_bench.exe 1000000

# Custom event count
.\build\lob_bench.exe 5000000
```

Sample output (MSVC 19.44 /O2, Windows 11, measured on this machine):
```
╔══════════════════════════════════════════════════╗
║           Matching Engine Benchmark              ║
╠══════════════════════════════════════════════════╣
║ Events processed  :      1000000                ║
║ Wall time         :   20121.79 ms               ║
║ Throughput        :      49697 msgs/sec          ║
╠══════════════════════════════════════════════════╣
║ Latency per event (ns)                           ║
║   Mean            :        20087                ║
║   p50             :         2000                ║
║   p95             :        87500                ║
║   p99             :       167500                ║
║   p99.9           :       398000                ║
║   max             :     49827000                ║
╚══════════════════════════════════════════════════╝
```

> **Note on benchmark design:** The synthetic load uses a 100-tick bid-ask spread, so most
> limit orders rest rather than match.  Over 1 M events the book grows to ~600 K resting
> orders spread across 40 price levels.  The p50 of **2 µs** reflects typical add/match
> operations; the mean (~20 µs) and long-tail are dominated by cancel operations that must
> linearly scan the price-level deque (O(depth)).  Replacing the deque with a doubly-linked
> list (O(1) cancel) would cut the tail latency dramatically—see *Extending the Engine* below.

---

## Input CSV Format

```
# Standard orders
timestamp,symbol,type,order_id,side,price,qty

# REPLACE
timestamp,symbol,REPLACE,old_id,side,price,qty,new_id,new_price,new_qty

# STOP_MARKET
timestamp,symbol,STOP_MARKET,id,side,stop_price,qty,0

# STOP_LIMIT
timestamp,symbol,STOP_LIMIT,id,side,stop_price,qty,limit_price
```

| Field | Type | Notes |
|---|---|---|
| `timestamp` | uint64 | Nanoseconds (monotonic) |
| `symbol` | string | E.g. `AAPL`, `MSFT` |
| `type` | string | `LIMIT` \| `MARKET` \| `CANCEL` \| `REPLACE` \| `STOP_MARKET` \| `STOP_LIMIT` |
| `order_id` | uint64 | Unique order identifier |
| `side` | string | `BUY` \| `SELL` |
| `price` | decimal | Dollars e.g. `150.25` (stored as 15025 ticks = $0.01 units) |
| `qty` | uint64 | Quantity; ignored for CANCEL |
| `stop_price` | decimal | STOP_MARKET / STOP_LIMIT: price that triggers the order |
| `limit_price` | decimal | STOP_LIMIT only: limit price used after trigger |
| `new_order_id` | uint64 | REPLACE only |
| `new_price` | decimal | REPLACE only |
| `new_qty` | uint64 | REPLACE only |

**Stop order trigger rules:**
- **Buy stop**: triggers when `last_trade_price >= stop_price` → submits a market or limit buy
- **Sell stop**: triggers when `last_trade_price <= stop_price` → submits a market or limit sell
- Stops are checked after every `LIMIT` or `MARKET` order; cascading triggers are handled iteratively.
- No trigger occurs until at least one trade has printed (cold-book guard).

Lines beginning with `#` are treated as comments.

---

## Output CSV Format

### `trades.csv`

```
timestamp,symbol,price,qty,aggressor_side,resting_order_id,aggressing_order_id
```

### `tob.csv` (emitted only when TOB changes)

```
timestamp,symbol,best_bid_price,best_bid_qty,best_ask_price,best_ask_qty
```

Prices are written in dollar notation (`150.25`).
`0.00` in best_bid_price / best_ask_price means that side is empty.

---

## Matching Rules

1. **Price-time priority**: within a price level, earlier orders match first (FIFO).
2. **Aggressor crosses resting price**:
   - BUY limit: matches if `bid_price >= best_ask_price`
   - SELL limit: matches if `ask_price <= best_bid_price`
3. **Trade price = resting order's price** (maker-takes-maker price).
4. **Partial fills** are supported on both sides.
5. **Market orders** sweep all available liquidity; no residual rests.
6. **REPLACE** = cancel original (loses queue position) + submit new limit.
7. **The book is never in a crossed state** after a completed match cycle.

---

## Project Structure

```
.
├── CMakeLists.txt
├── README.md
├── include/
│   ├── Types.h              ← OrderId, Price, Qty, Trade, TOBUpdate, MatchResult
│   ├── Order.h              ← Order struct, OrderEvent struct
│   ├── OrderBook.h          ← Per-symbol book (addLimit/Market/cancel/replace)
│   ├── MatchingEngine.h     ← Multi-symbol engine with callback hooks
│   ├── MessageParser.h      ← CSV parse/serialise
│   └── MarketDataPublisher.h← Writes trades.csv + tob.csv
├── src/
│   ├── OrderBook.cpp
│   ├── MatchingEngine.cpp
│   ├── MessageParser.cpp
│   ├── MarketDataPublisher.cpp
│   └── main.cpp             ← Replay entry point
├── tests/
│   ├── test_framework.h     ← Zero-dependency test macros
│   └── test_engine.cpp      ← 34 unit tests (25 core + 9 stop-order)
├── benchmark/
│   └── benchmark.cpp        ← Synthetic load generator + p50/p95/p99 stats
└── data/
    └── sample_orders.csv    ← Annotated replay file
```

---

## Extending the Engine

| Feature | Where to add |
|---|---|
| Full L2 depth snapshot | `OrderBook::getDepth(int levels)` |
| IOC / FOK order types | `OrderBook::addLimit()` with fill-or-cancel flag |
| ~~Stop orders~~ | ✅ Implemented — `buy_stops_` / `sell_stops_` in `OrderBook`; cascading via `checkAndTriggerStops()` |
| Socket feed (live mode) | New `NetworkFeed` class calling `engine.processEvent()` |
| Memory pooling | Replace `new Order` with a `boost::pool` or custom slab |
| Lock-free structures | Replace `std::map` with a skip-list or Abseil's B-tree |

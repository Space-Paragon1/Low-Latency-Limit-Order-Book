# Low-Latency Limit Order Book

[![CI](https://github.com/Space-Paragon1/Low-Latency-Limit-Order-Book/actions/workflows/ci.yml/badge.svg)](https://github.com/Space-Paragon1/Low-Latency-Limit-Order-Book/actions/workflows/ci.yml)

A deterministic, price-time priority matching engine in C++17. It provides
limit, market, stop-market and stop-limit orders, IOC / FOK / post-only time
in force, cancel/replace with exchange-style queue priority, and an
allocation-free hot path that processes **27 M events/s at ~37 ns per event**
on a single core.

Correctness is checked by a differential fuzz test that compares the engine
against a naive reference model, operation by operation, across 500 k random
orders on every CI run.

| Apple M2, one core, `-O3` | v1 (`std::map` + `std::deque`) | v2 (this version) |
|---|---|---|
| Throughput, v1 benchmark workload | 0.46 M msgs/s | **8.6 M msgs/s** (18×) |
| Mean latency, same workload | 2,136 ns | **99 ns** |
| p99 latency, same workload | 14,292 ns | **292 ns** |
| Throughput, realistic workload | — | **27.1 M events/s** |
| p99 / p99.9, realistic workload | — | **84 ns / 167 ns** |

---

## Contents

- [Quick start](#quick-start)
- [Design](#design)
- [Performance](#performance)
- [Matching semantics](#matching-semantics)
- [Correctness](#correctness)
- [File formats](#file-formats)
- [Library usage](#library-usage)
- [Project layout](#project-layout)
- [Changes from v1](#changes-from-v1)
- [Not in scope (yet)](#not-in-scope-yet)

---

## Quick start

Requires CMake ≥ 3.21 (for presets; plain `cmake -B build` works with ≥ 3.16)
and a C++17 compiler: GCC 9+, Clang 10+, Apple Clang 12+ or MSVC 2019+.
There are no third-party dependencies.

```bash
cmake --preset release            # configure  → build/release
cmake --build --preset release    # build lob_core, lob_main, lob_tests, lob_bench
ctest --preset release            # unit + fuzz + golden replay + benchmark smoke
```

| Preset | Purpose |
|---|---|
| `release` | Optimised build, default for everything |
| `debug` | Unoptimised, assertions on |
| `asan` | Debug with AddressSanitizer + UBSan and `-Werror` (GCC/Clang) |
| `bench` | Release with LTO and `-march=native`, for benchmarking |

Visual Studio and CLion pick up the presets automatically.

**Replay a file**

```bash
./build/release/lob_main data/sample_orders.csv output/trades.csv output/tob.csv
```
```
[WARN ] data/sample_orders.csv:58: CANCEL 999 rejected: UNKNOWN_ORDER
[INFO ] Events         : 24 (from data/sample_orders.csv)
[INFO ] Malformed lines: 0
[INFO ] Rejected       : 1
[INFO ]   UNKNOWN_ORDER: 1
[INFO ] Trades         : 12  -> output/trades.csv
[INFO ] TOB updates    : 15  -> output/tob.csv
```

Malformed lines are reported with line number and reason (`--strict` turns
them into a non-zero exit). Rejected orders are reported with their reason
(`--quiet` suppresses this). Output directories are created as needed.

**Benchmark**

```bash
./build/release/lob_bench                      # realistic workload, 1 M events
./build/release/lob_bench --scenario deep      # v1 workload: ~290 k resting orders
./build/release/lob_bench --events 5000000 --seed 7 --repeat 9
```

---

## Design

```
            OrderEvent stream (CSV, or your own gateway)
                               │
                               ▼
               ┌───────────────────────────────┐
               │        MatchingEngine         │  symbol → OrderBook, one-entry cache
               └───────────────┬───────────────┘
                               │ apply(event) → MatchResult {status, reject, fills, trades}
                               ▼
  ┌──────────────────────────── OrderBook (per symbol) ────────────────────────────┐
  │                                                                                │
  │  bids_  PriceLadder ─► [ 99.98 │ 99.99 │ 100.00 ]  ◄─ best at the back         │
  │                                            │                                   │
  │                                            ▼  PriceLevel {total_qty, count}    │
  │                                     ┌──────┐   ┌──────┐   ┌──────┐            │
  │                                     │ Node │ ⇄ │ Node │ ⇄ │ Node │  FIFO       │
  │                                     └──────┘   └──────┘   └──────┘            │
  │  asks_, buy_stops_, sell_stops_: same structure                                │
  │                                                                                │
  │  orders_  OrderIdMap   id → Node*   (open addressing, backward-shift delete)   │
  │  pools    ObjectPool<OrderNode>, ObjectPool<PriceLevel>   (no malloc when warm)│
  └────────────────────────────────────────────────────────────────────────────────┘
                               │ trade + TOB callbacks
                               ▼
                    MarketDataPublisher → trades.csv, tob.csv
```

### Data structures and why

| Component | Choice | Reason |
|---|---|---|
| Order storage | `OrderNode`, **exactly 64 bytes** (one cache line), from an `ObjectPool` | No allocator calls on the hot path; LIFO reuse keeps memory cache-hot |
| Queue at a price | Intrusive doubly linked FIFO | O(1) cancel from a pointer; no search, no shifting |
| Level aggregates | `total_qty` and `order_count` kept incrementally | Top-of-book quantity is O(1) (v1 summed the level on every call) |
| Price levels | Sorted contiguous array of `{price, level*}`, **best at the back** | Activity clusters at the inside. Best-level access and removal are `back()`/`pop_back()`, and inserting k levels from the top is an O(k) memmove of 16-byte entries |
| Level lookup | Linear probe of the 8 levels nearest the top, then binary search | Near-touch prices resolve in a few predictable compares; deep prices stay O(log L) |
| Order id → node | Open-addressing hash map, linear probing, load ≤ 50%, backward-shift deletion | One or two adjacent 16-byte slots per lookup; no tombstones, so constant add/cancel churn can't degrade it |
| Symbols | Fixed 16-byte `Symbol`, compared as two words | `Trade`, `TOBUpdate` and `OrderEvent` are trivially copyable PODs |
| Trade output | Per-book reusable buffer, returned as a `TradeSpan` view | No `std::vector` allocation per call |

Stop orders use the same ladder and node types (buy stops ordered lowest
trigger first, sell stops highest first), so they are cancellable in O(1) like
any other order.

### Complexity

L = price levels on a side, k = distance of the touched level from the top of
the book, f = number of fills.

| Operation | Cost |
|---|---|
| Add passive limit at an existing level | O(1) expected |
| Add limit opening a new level | O(k + log L) |
| Cancel / quantity-down replace | O(1) expected; O(k + log L) if it empties a level |
| Aggressive order | O(f) plus level removals at the top (O(1) each) |
| Best bid/ask price and quantity | O(1) |
| FOK liquidity check | O(levels inside the limit) |

### Threading model

Each book is single-threaded on purpose. A matching core is a deterministic
state machine, and locks or atomics on the hot path would only add latency
and nondeterminism. Real venues scale by sharding symbols across cores, each
with its own engine. That fits this design directly, because books share no
state.

---

## Performance

Measured on an Apple M2 (one performance core, macOS, no core pinning) with
Apple Clang 21 and the `release` preset (`-O3`). Reproduce with
`./build/release/lob_bench`.

**Realistic workload** (default). A random-walk mid price, passive orders at
geometrically distributed distances from the touch, 8% marketable limits, 35%
cancels of live orders, qty-down and price-move replaces, market orders and
stops. The book holds steady at a few hundred orders, as a live instrument's
book does.

```
Throughput (median of 5): 27.13 M events/s   (36.9 ns/event)

  op            count     mean     p50     p90     p99    p99.9
  limit        480110       36      42      42      84      166
  market        49852       42      42      83     167      292
  cancel       333358       31      41      42      83       84
  replace      104723       47      42      83     125      167
  stop           9859       39      42      42      84      208
  all          977902       36      42      42      84      167
```

**Deep workload** (`--scenario deep`, the v1 benchmark's shape). Non-crossing
limits on 20 levels per side, so the book grows to about 290 k resting orders,
roughly 7,000 per level. Throughput is 14.4 M events/s (69 ns/event), with
p99 250 ns and p99.9 334 ns. Cancel cost does not depend on queue depth. In
v1 this was the slowest path, because it scanned the deque.

**v1 vs v2 on the identical event stream.** The v1 benchmark source, compiled
against each engine:

| | v1 | v2 |
|---|---|---|
| Throughput | 464 k/s | 8.59 M/s |
| Mean / p50 | 2,136 / 459 ns | 99 / 42 ns |
| p99 / p99.9 | 14,292 / 22,875 ns | 292 / 5,000 ns |

The v1 harness times every event and does not pre-size the engine. Its v2
p99.9 is therefore dominated by the id map rehashing as the book grows to
600 k orders. Pass `expected_orders_per_book` to `MatchingEngine` (as
`lob_bench` does) to remove these growth spikes.

### Methodology and caveats

- The event stream is generated before measurement, against a shadow book,
  so cancels and replaces hit orders that are actually live. The timed runs
  are deterministic replays of that stream.
- Throughput runs make no per-event clock reads. The reported figure is the
  median of `--repeat` runs, after one warm-up run.
- The latency pass reads the clock twice per event. **On Apple Silicon,
  `steady_clock` has 41.7 ns resolution** (24 MHz timebase) and costs ~19 ns
  per read, so the 42 ns p50 means "within one clock tick". The
  throughput-derived ns/event and the means are the precise figures. On
  x86-64 Linux, `steady_clock` resolves to 1 ns.
- `max` latencies (a few µs) come from scheduling noise on an untuned desktop
  OS. On a tuned host (isolated cores, pinned thread, fixed frequency) expect
  a tighter tail.
- The `bench` preset (LTO + `-march=native`) adds about 15% on the deep
  workload and nothing measurable on the realistic one.

---

## Matching semantics

**Priority and pricing**
1. Price-time priority: better price first, then FIFO within a price.
2. A trade prints at the **resting** order's price.
3. Partial fills on both sides. An aggressor sweeps levels until it is filled
   or reaches its limit.
4. The book is never crossed between operations. `validate()` checks this
   along with every other invariant.

**Order types and time in force**

| | Behaviour |
|---|---|
| `LIMIT` + `GTC` (default) | Matches what it can, rests the remainder |
| `LIMIT` + `IOC` | Matches what it can; the remainder is cancelled |
| `LIMIT` + `FOK` | Executes in full immediately, or is cancelled with no trades. Only liquidity within the limit counts |
| `LIMIT` + `POST_ONLY` | Rests only. Rejected (`POST_ONLY_WOULD_CROSS`) if any part would take liquidity |
| `MARKET` | Sweeps at any price and never rests; the remainder is cancelled. `FOK` is supported, `POST_ONLY` is rejected |
| `STOP_MARKET` / `STOP_LIMIT` | Pending until elected, then released as a market / limit (GTC) order with the same id |

**Cancel/replace.** Side and time in force are inherited from the original
order. A replace that **keeps the price and does not increase the quantity**
is applied in place and **keeps queue priority**, as on most exchanges. Any
other replace cancels the original and enters a new order at the back of the
queue, which may trade immediately. A replace is validated in full (target
live, new id unique, price/qty valid, post-only not crossing) before the
original is touched, so a rejected replace leaves the book unchanged. A
replace of a filled or cancelled order is rejected, so it can't create a new
order (an overfill).

**Stop orders**
- A buy stop is *elected* by any trade at or above its stop price; a sell stop
  by any trade at or below it.
- Election uses the full price range traded by the operation. A sweep that
  prints 101.00 then 99.00 elects buy stops ≤ 101.00, not just those ≤ 99.00.
- Elected stops are released in priority order: buy stops lowest-trigger
  first, then sell stops highest-trigger first, FIFO within a trigger price.
  Their trades can elect further stops, and the cascade runs until it settles.
- A stop submitted when the last trade has already passed its trigger is
  released immediately. Before the first trade there is no reference price,
  so stops wait.
- Pending stops can be cancelled, and their ids count as live for duplicate
  checks. They cannot be replaced.

**Rejects.** Every operation returns a `MatchResult` with a final
`OrderStatus` and, when rejected, a `RejectReason`:

| Reason | When |
|---|---|
| `INVALID_QUANTITY` | qty (or replace new_qty) is 0 |
| `INVALID_PRICE` | price, stop price, limit price or replace price ≤ 0 |
| `INVALID_TIME_IN_FORCE` | `POST_ONLY` on a market order |
| `DUPLICATE_ORDER_ID` | id is already live in the book (resting or pending stop) |
| `UNKNOWN_ORDER` | cancel/replace target is not live (or a replace targets a stop) |
| `POST_ONLY_WOULD_CROSS` | a post-only order (or replace) would take liquidity |

Ids are unique among *live* orders, per symbol. The id of a filled or
cancelled order may be reused.

**Determinism.** The same input always produces byte-identical output, since
the engine has no clocks, randomness, threads or iteration over unordered
containers on any path that affects results.

---

## Correctness

| Layer | What it covers |
|---|---|
| **Unit tests** (85) | Matching, partial fills, FIFO, cancels at head/middle/tail, all replace paths, every TIF, stops (cascades, election range, ordering, cancel), parser strictness and error reporting, engine routing and callback ordering |
| **Differential fuzz test** | 200 seeds × 2,500 random operations, run in a narrow price band so crossing, sweeps and stop cascades are frequent. After **every** operation it compares status, reject reason, filled/leaves qty, every trade, the top of book and the `tob_changed` flag against a naive reference model (flat vectors, linear scans), then runs the full O(n) `validate()` invariant check. Scale it with `LOB_FUZZ_SEEDS=5000 ./lob_tests fuzz` |
| **Golden replay** | `lob_main` on `data/sample_orders.csv` must reproduce `tests/golden/*.csv` byte for byte |
| **Sanitizers** | The whole suite under ASan + UBSan in CI (`asan` preset locally) |
| **CI** | Linux GCC, Linux Clang, macOS Apple Clang, Windows MSVC, plus the sanitizer job. Clang builds use `-Werror` with `-Wconversion -Wsign-conversion -Wshadow -Wold-style-cast` |

The fuzz test has been mutation-tested: each of the following deliberately
injected bugs makes it fail. Using last-trade price instead of the traded
range for stop election; LIFO instead of FIFO within a level; FOK ignoring
its limit; keeping priority on a qty-increase replace; not updating a level's
aggregate qty; disabling the hash map's backward shift; an off-by-one in the
ladder search; wrong stop-release order; printing at the aggressor's price.

---

## File formats

### Input CSV

One event per line. Lines starting with `#` and blank lines are ignored.
Fields are trimmed, and keywords are case-insensitive.

```
timestamp,symbol,LIMIT,id,side,price,qty[,tif]
timestamp,symbol,MARKET,id,side,price,qty[,tif]            price ignored
timestamp,symbol,CANCEL,id,side,price,qty                  side/price/qty ignored
timestamp,symbol,REPLACE,id,side,price,qty,new_id,new_price,new_qty
timestamp,symbol,STOP_MARKET,id,side,stop_price,qty[,0]
timestamp,symbol,STOP_LIMIT,id,side,stop_price,qty,limit_price
```

| Field | Format |
|---|---|
| `timestamp` | unsigned integer, nanoseconds (any monotonic origin) |
| `symbol` | 1–16 characters |
| `side` | `BUY` \| `SELL` |
| `tif` | `GTC` (default) \| `IOC` \| `FOK` \| `POST_ONLY` |
| prices | decimal with at most two places: `150`, `150.5`, `150.25` = 15000, 15050, 15025 ticks (1 tick = 0.01). Sub-tick (`150.255`), negative and malformed values are rejected, never rounded |
| `qty`, ids | unsigned integers |

### Output CSV

```
trades.csv  timestamp,symbol,price,qty,aggressor_side,resting_order_id,aggressing_order_id
tob.csv     timestamp,symbol,best_bid_price,best_bid_qty,best_ask_price,best_ask_qty
```

A `tob.csv` row is written only when the best bid/ask price or the aggregate
quantity at the top changes. `0.00` / `0` means that side is empty. Line
endings are `\n` on every platform.

---

## Library usage

Link `lob::core` (`add_subdirectory` this repo, then
`target_link_libraries(app PRIVATE lob::core)`):

```cpp
#include "lob/MatchingEngine.h"

lob::MatchingEngine engine(/*expected_orders_per_book=*/100'000);
engine.setTradeHandler([](const lob::Trade& t) { /* publish */ });
engine.setTOBHandler([](const lob::TOBUpdate& u) { /* publish */ });

using lob::OrderEvent; using lob::Side; using lob::TimeInForce;
engine.processEvent(OrderEvent::limit(1, "AAPL", 1, Side::SELL, 15025, 100));
auto r = engine.processEvent(OrderEvent::limit(2, "AAPL", 2, Side::BUY, 15030, 150,
                                               TimeInForce::IOC));
// r.status == CANCELLED (IOC remainder), r.filled_qty == 100, r.trades.size() == 1

lob::OrderBook* book = engine.getBook("AAPL");
auto depth = book->depth(Side::BUY, 10);      // L2 snapshot, best first
auto info  = book->findOrder(1);              // std::optional<OrderInfo>
```

`OrderBook` can also be used on its own. `MatchResult::trades` is a view into
a buffer owned by the book: it stays valid until the next operation on that
book.

---

## Project layout

```
include/lob/
  Types.h                 primitives, enums, Symbol, Trade, TOBUpdate, MatchResult
  OrderEvent.h            inbound event + factories
  OrderBook.h             per-symbol book
  MatchingEngine.h        multi-symbol router + callbacks
  MessageParser.h         CSV codec
  MarketDataPublisher.h   buffered CSV writer
  detail/
    ObjectPool.h          stable-address pool allocator
    OrderIdMap.h          open-addressing id → node map
    PriceLadder.h         OrderNode, PriceLevel, sorted level array
src/                      implementations + main.cpp (lob_main)
benchmark/benchmark.cpp   lob_bench
tests/
  test_order_book.cpp  test_stops.cpp  test_parser.cpp  test_engine.cpp
  test_fuzz.cpp           reference model + differential test
  golden/                 expected lob_main output for data/sample_orders.csv
cmake/GoldenTest.cmake    golden replay driver
```

---

## Changes from v1

v2 replaces the core data structures and fixes several correctness bugs. If
you have inputs or code written against v1, note these behaviour changes:

- **Integer prices mean whole units.** `100` is now 100.00, the same as
  `100.00`. In v1 it meant 100 *ticks* (1.00), a 100× trap. Sub-tick prices
  are rejected instead of truncated.
- **Duplicate live ids are rejected.** In v1 a duplicate silently overwrote
  the live order while its id stayed queued, which corrupted the book.
- **Replace** inherits the original's side (v1 used whatever side was on the
  request). It is rejected if the original is no longer live (v1 created a
  fresh order). It keeps priority on a qty-down at the same price.
- **Stops** can be cancelled, and they are elected by every price the
  operation traded through, not only the last one.
- **TOB updates** are emitted only on real changes. v1 emitted one for every
  successful cancel, including deep ones.
- **Rejects and malformed lines are reported** instead of silently dropped.
- Everything is in `namespace lob`, and headers are under `include/lob/`.
- The Windows-only `configure.ps1`, `run_bench.ps1` and `build_and_test.bat`
  (which hardcoded one machine's paths) are replaced by CMake presets.

---

## Not in scope (yet)

- Self-trade prevention (needs a participant id on orders)
- Iceberg / reserve quantity, and good-till-date expiry
- A binary wire protocol (ITCH/OUCH-style) and a network gateway
- Journaling / snapshot + replay for recovery
- Price bands and circuit breakers

## License

MIT, see [LICENSE](LICENSE).

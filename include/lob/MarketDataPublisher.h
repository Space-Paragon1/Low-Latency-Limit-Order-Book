#pragma once

#include "lob/Types.h"

#include <cstdint>
#include <fstream>
#include <string>

namespace lob {

// ─── Market data publisher ────────────────────────────────────────────────────
// Writes the two output streams as CSV:
//   trades : timestamp,symbol,price,qty,aggressor_side,resting_order_id,aggressing_order_id
//   tob    : timestamp,symbol,best_bid_price,best_bid_qty,best_ask_price,best_ask_qty
// Rows are formatted into in-memory buffers and written in large blocks, so a
// callback costs a few dozen nanoseconds rather than a stream round-trip.
// Output always uses '\n' line endings, on every platform.
class MarketDataPublisher {
public:
    // Throws std::runtime_error if either file cannot be opened.
    MarketDataPublisher(const std::string& trades_path, const std::string& tob_path);
    ~MarketDataPublisher();

    MarketDataPublisher(const MarketDataPublisher&)            = delete;
    MarketDataPublisher& operator=(const MarketDataPublisher&) = delete;

    void onTrade(const Trade& trade);
    void onTOB(const TOBUpdate& tob);

    // Writes buffered rows to disk.  Throws std::runtime_error on I/O failure.
    void flush();

    [[nodiscard]] std::uint64_t tradeCount() const noexcept { return trade_count_; }
    [[nodiscard]] std::uint64_t tobCount()   const noexcept { return tob_count_; }

private:
    struct Sink {
        std::string   path;
        std::ofstream out;
        std::string   buf;
        void open(const std::string& p, std::string_view header);
        void maybeDrain();
        void drain();
    };

    Sink          trades_;
    Sink          tob_;
    std::uint64_t trade_count_{0};
    std::uint64_t tob_count_{0};
};

}  // namespace lob

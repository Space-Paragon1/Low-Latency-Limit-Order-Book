#pragma once
#include "Types.h"
#include <fstream>
#include <string>

// ─── Market data publisher ────────────────────────────────────────────────────
// Writes two log files:
//   trades.csv : timestamp,symbol,price,qty,aggressor_side,
//                resting_order_id,aggressing_order_id
//   tob.csv    : timestamp,symbol,best_bid_price,best_bid_qty,
//                best_ask_price,best_ask_qty
//
// Prices are written as decimal strings (e.g. "150.25").
class MarketDataPublisher {
public:
    MarketDataPublisher(const std::string& trades_path,
                        const std::string& tob_path);
    ~MarketDataPublisher();

    void onTrade(const Trade&     trade);
    void onTOB  (const TOBUpdate& tob);
    void flush();

    // Statistics
    uint64_t tradeCount() const { return trade_count_; }
    uint64_t tobCount()   const { return tob_count_; }

private:
    std::ofstream trades_out_;
    std::ofstream tob_out_;
    uint64_t      trade_count_{0};
    uint64_t      tob_count_{0};
};

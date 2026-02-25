#include "MarketDataPublisher.h"
#include "MessageParser.h"
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────────────────
MarketDataPublisher::MarketDataPublisher(const std::string& trades_path,
                                         const std::string& tob_path)
    : trades_out_(trades_path), tob_out_(tob_path) {
    if (!trades_out_.is_open())
        throw std::runtime_error("Cannot open trades output: " + trades_path);
    if (!tob_out_.is_open())
        throw std::runtime_error("Cannot open tob output: " + tob_path);

    // Write CSV headers.
    trades_out_ << "timestamp,symbol,price,qty,aggressor_side,"
                   "resting_order_id,aggressing_order_id\n";
    tob_out_    << "timestamp,symbol,best_bid_price,best_bid_qty,"
                   "best_ask_price,best_ask_qty\n";
}

MarketDataPublisher::~MarketDataPublisher() { flush(); }

void MarketDataPublisher::onTrade(const Trade& trade) {
    trades_out_ << MessageParser::tradeToCsv(trade) << '\n';
    ++trade_count_;
}

void MarketDataPublisher::onTOB(const TOBUpdate& tob) {
    tob_out_ << MessageParser::tobToCsv(tob) << '\n';
    ++tob_count_;
}

void MarketDataPublisher::flush() {
    trades_out_.flush();
    tob_out_.flush();
}

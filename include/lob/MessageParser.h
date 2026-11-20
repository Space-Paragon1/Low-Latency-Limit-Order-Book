#pragma once

#include "lob/OrderEvent.h"
#include "lob/Types.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lob {

// ─── CSV message codec ────────────────────────────────────────────────────────
//
// Input, one event per line ('#' comments and blank lines are skipped):
//
//   timestamp,symbol,LIMIT,id,side,price,qty[,tif]
//   timestamp,symbol,MARKET,id,side,price,qty[,tif]          price ignored
//   timestamp,symbol,CANCEL,id,side,price,qty                side/price/qty ignored
//   timestamp,symbol,REPLACE,id,side,price,qty,new_id,new_price,new_qty
//   timestamp,symbol,STOP_MARKET,id,side,stop_price,qty[,0]
//   timestamp,symbol,STOP_LIMIT,id,side,stop_price,qty,limit_price
//
//   side   BUY | SELL                      (case-insensitive)
//   tif    GTC | IOC | FOK | POST_ONLY     (case-insensitive, default GTC)
//   price  decimal with at most 2 decimals: "150", "150.2", "150.25"
//          → 15000, 15020, 15025 ticks.  Finer precision ("150.255") and
//          negative values are rejected rather than silently rounded.
//
// Parsing never throws on bad input: malformed lines are reported with a
// reason so the caller can surface them.
class MessageParser {
public:
    struct LineResult {
        enum class Kind { EVENT, SKIP, INVALID };   // (avoids the Windows ERROR macro)
        Kind             kind{Kind::SKIP};
        OrderEvent       event;
        std::string_view error;   // static string; set when kind == INVALID
    };

    struct ParseError {
        std::size_t line{0};
        std::string message;
        std::string text;
    };

    struct ParsedFile {
        std::vector<OrderEvent>  events;
        std::vector<std::size_t> line_numbers;   // source line of events[i]
        std::vector<ParseError>  errors;
    };

    static LineResult parseLine(std::string_view line);

    // Convenience: the event, or nullopt for comments, blanks and bad lines.
    static std::optional<OrderEvent> parseCSVLine(std::string_view line);

    // Throws std::runtime_error if the file cannot be read.
    static ParsedFile parseCSVFile(const std::string& path);

    // Strict decimal price → ticks; nullopt if malformed or out of range.
    static std::optional<Price> parsePrice(std::string_view s);
    static std::string          priceToString(Price ticks);

    // ── Output ────────────────────────────────────────────────────────────────
    static constexpr std::string_view kTradeHeader =
        "timestamp,symbol,price,qty,aggressor_side,resting_order_id,aggressing_order_id";
    static constexpr std::string_view kTOBHeader =
        "timestamp,symbol,best_bid_price,best_bid_qty,best_ask_price,best_ask_qty";

    // Append one CSV row (no trailing newline) to `out`.  Allocation-free once
    // `out` has capacity.
    static void appendTrade(std::string& out, const Trade& t);
    static void appendTOB  (std::string& out, const TOBUpdate& t);
    static void appendPrice(std::string& out, Price ticks);

    static std::string tradeToCsv(const Trade& t);
    static std::string tobToCsv  (const TOBUpdate& t);
};

}  // namespace lob

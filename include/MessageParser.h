#pragma once
#include "Order.h"
#include "Types.h"
#include <string>
#include <vector>
#include <optional>

// ─── CSV message parser ────────────────────────────────────────────────────────
// Input CSV schema (no header row needed, '#' lines are comments):
//
//   timestamp,symbol,type,order_id,side,price,qty[,new_order_id,new_price,new_qty]
//
//   timestamp  : uint64 nanoseconds
//   symbol     : string (e.g. AAPL)
//   type       : LIMIT | MARKET | CANCEL | REPLACE
//   order_id   : uint64
//   side       : BUY | SELL  (ignored for CANCEL)
//   price      : decimal dollars (e.g. 150.25 → 15025 ticks internally)
//                or integer ticks; ignored for MARKET/CANCEL
//   qty        : uint64; ignored for CANCEL
//   new_order_id, new_price, new_qty : REPLACE only
//
// Output CSV helpers (for trades.csv / tob.csv):
//   Price output format: ticks → "150.25" (divided by 100, 2 decimal places)
class MessageParser {
public:
    static std::optional<OrderEvent> parseCSVLine(const std::string& line);
    static std::vector<OrderEvent>   parseCSVFile(const std::string& path);

    // Price conversion
    static Price       parsePriceTicks(const std::string& s);
    static std::string priceToString  (Price ticks);

    // Serialise output events
    static std::string tradeToCsv(const Trade&     t);
    static std::string tobToCsv  (const TOBUpdate& t);

private:
    static std::vector<std::string> split(const std::string& s, char delim);
    static std::string              trim (const std::string& s);
};

#include "MessageParser.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cctype>
#include <iomanip>

// ─── String utilities ─────────────────────────────────────────────────────────
std::string MessageParser::trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> MessageParser::split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, delim))
        out.push_back(trim(tok));
    return out;
}

// ─── Price parsing ────────────────────────────────────────────────────────────
// Accepts both "150.25" and "15025" (already ticks).
// If the string contains a '.', multiplies the integer part by 100 and adds the
// fractional part (padded/truncated to 2 decimal places).
Price MessageParser::parsePriceTicks(const std::string& raw) {
    std::string s = trim(raw);
    auto dot = s.find('.');
    if (dot == std::string::npos) {
        // Treat as integer ticks
        return static_cast<Price>(std::stoull(s));
    }
    std::string int_part  = s.substr(0, dot);
    std::string frac_part = s.substr(dot + 1);
    // Pad or truncate fractional to exactly 2 digits.
    while (frac_part.size() < 2) frac_part += '0';
    frac_part = frac_part.substr(0, 2);
    Price ticks = static_cast<Price>(std::stoull(int_part)) * 100
                + static_cast<Price>(std::stoull(frac_part));
    return ticks;
}

std::string MessageParser::priceToString(Price ticks) {
    // Convert ticks back to "DDD.cc" format.
    Price dollars  = ticks / 100;
    Price cents    = ticks % 100;
    std::ostringstream oss;
    oss << dollars << '.' << std::setw(2) << std::setfill('0') << cents;
    return oss.str();
}

// ─── Single-line parser ───────────────────────────────────────────────────────
std::optional<OrderEvent> MessageParser::parseCSVLine(const std::string& line) {
    std::string l = trim(line);
    if (l.empty() || l[0] == '#') return std::nullopt;

    auto fields = split(l, ',');
    if (fields.size() < 7) return std::nullopt;

    OrderEvent ev;

    try {
        ev.timestamp = std::stoull(fields[0]);
        ev.symbol    = fields[1];

        // type
        std::string tp = fields[2];
        for (auto& c : tp) c = static_cast<char>(std::toupper(c));
        if      (tp == "LIMIT")        ev.type = OrderType::LIMIT;
        else if (tp == "MARKET")       ev.type = OrderType::MARKET;
        else if (tp == "CANCEL")       ev.type = OrderType::CANCEL;
        else if (tp == "REPLACE")      ev.type = OrderType::REPLACE;
        else if (tp == "STOP_MARKET")  ev.type = OrderType::STOP_MARKET;
        else if (tp == "STOP_LIMIT")   ev.type = OrderType::STOP_LIMIT;
        else return std::nullopt;

        ev.order_id = std::stoull(fields[3]);

        // side
        std::string sd = fields[4];
        for (auto& c : sd) c = static_cast<char>(std::toupper(c));
        if      (sd == "BUY")  ev.side = Side::BUY;
        else if (sd == "SELL") ev.side = Side::SELL;
        else return std::nullopt;

        ev.price = parsePriceTicks(fields[5]);
        ev.qty   = std::stoull(fields[6]);

        // Stop order: field[5]=stop_price (stored in ev.price), field[7]=limit_price.
        // For STOP_MARKET, limit_price is 0 (CSV conventionally contains "0").
        // For STOP_LIMIT,  limit_price is the price of the triggered limit order.
        if ((ev.type == OrderType::STOP_MARKET || ev.type == OrderType::STOP_LIMIT)
            && fields.size() >= 8) {
            ev.limit_price = parsePriceTicks(fields[7]);
        }

        // REPLACE optional trailing fields
        if (ev.type == OrderType::REPLACE && fields.size() >= 10) {
            ev.new_order_id = std::stoull(fields[7]);
            ev.new_price    = parsePriceTicks(fields[8]);
            ev.new_qty      = std::stoull(fields[9]);
        }
    } catch (...) {
        return std::nullopt;
    }

    return ev;
}

// ─── File parser ─────────────────────────────────────────────────────────────
std::vector<OrderEvent> MessageParser::parseCSVFile(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open())
        throw std::runtime_error("Cannot open input file: " + path);

    std::vector<OrderEvent> events;
    std::string line;
    while (std::getline(in, line)) {
        auto ev = parseCSVLine(line);
        if (ev) events.push_back(*ev);
    }
    return events;
}

// ─── Output serialisers ───────────────────────────────────────────────────────
std::string MessageParser::tradeToCsv(const Trade& t) {
    std::ostringstream oss;
    oss << t.timestamp << ','
        << t.symbol    << ','
        << priceToString(t.price) << ','
        << t.qty       << ','
        << (t.aggressor_side == Side::BUY ? "BUY" : "SELL") << ','
        << t.resting_order_id    << ','
        << t.aggressing_order_id;
    return oss.str();
}

std::string MessageParser::tobToCsv(const TOBUpdate& t) {
    std::ostringstream oss;
    oss << t.timestamp << ','
        << t.symbol    << ','
        << priceToString(t.best_bid_price) << ','
        << t.best_bid_qty << ','
        << priceToString(t.best_ask_price) << ','
        << t.best_ask_qty;
    return oss.str();
}

#include "lob/MessageParser.h"

#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace lob {

namespace {

using Kind = MessageParser::LineResult::Kind;

std::string_view trim(std::string_view s) noexcept {
    constexpr std::string_view ws = " \t\r\n";
    const std::size_t a = s.find_first_not_of(ws);
    if (a == std::string_view::npos) return {};
    const std::size_t b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char c = a[i];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (c != b[i]) return false;   // `b` is always an upper-case literal
    }
    return true;
}

template <typename T>
bool parseUnsigned(std::string_view s, T& out) noexcept {
    if (s.empty()) return false;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && end == s.data() + s.size();
}

bool parseSide(std::string_view s, Side& out) noexcept {
    if (iequals(s, "BUY"))  { out = Side::BUY;  return true; }
    if (iequals(s, "SELL")) { out = Side::SELL; return true; }
    return false;
}

bool parseType(std::string_view s, OrderType& out) noexcept {
    constexpr std::array<OrderType, 6> all = {OrderType::LIMIT,  OrderType::MARKET,
                                              OrderType::CANCEL, OrderType::REPLACE,
                                              OrderType::STOP_MARKET, OrderType::STOP_LIMIT};
    for (const OrderType t : all) {
        if (iequals(s, toString(t))) {
            out = t;
            return true;
        }
    }
    return false;
}

bool parseTif(std::string_view s, TimeInForce& out) noexcept {
    constexpr std::array<TimeInForce, 4> all = {TimeInForce::GTC, TimeInForce::IOC,
                                                TimeInForce::FOK, TimeInForce::POST_ONLY};
    for (const TimeInForce t : all) {
        if (iequals(s, toString(t))) {
            out = t;
            return true;
        }
    }
    return false;
}

MessageParser::LineResult invalid(std::string_view why) noexcept {
    MessageParser::LineResult r;
    r.kind  = Kind::INVALID;
    r.error = why;
    return r;
}

void appendUnsigned(std::string& out, std::uint64_t v) {
    char buf[24];
    const auto res = std::to_chars(buf, buf + sizeof buf, v);
    out.append(buf, static_cast<std::size_t>(res.ptr - buf));
}

}  // namespace

// ─── Prices ───────────────────────────────────────────────────────────────────

std::optional<Price> MessageParser::parsePrice(std::string_view s) {
    s = trim(s);
    const std::size_t      dot   = s.find('.');
    const std::string_view whole = s.substr(0, dot);
    std::uint64_t          units = 0;
    if (!parseUnsigned(whole, units)) return std::nullopt;   // also rejects '-', '+', ""

    std::uint64_t cents = 0;
    if (dot != std::string_view::npos) {
        const std::string_view frac = s.substr(dot + 1);
        if (frac.empty()) return std::nullopt;
        for (std::size_t i = 0; i < frac.size(); ++i) {
            const char c = frac[i];
            if (c < '0' || c > '9') return std::nullopt;
            if (i < 2)          cents = cents * 10 + static_cast<std::uint64_t>(c - '0');
            else if (c != '0')  return std::nullopt;   // finer than one tick
        }
        if (frac.size() == 1) cents *= 10;
    }

    constexpr auto kMaxUnits =
        static_cast<std::uint64_t>(std::numeric_limits<Price>::max() / kTicksPerUnit) - 1;
    if (units > kMaxUnits) return std::nullopt;
    return static_cast<Price>(units) * kTicksPerUnit + static_cast<Price>(cents);
}

void MessageParser::appendPrice(std::string& out, Price ticks) {
    std::uint64_t mag = 0;
    if (ticks < 0) {
        out.push_back('-');
        mag = static_cast<std::uint64_t>(-(ticks + 1)) + 1;   // safe for INT64_MIN
    } else {
        mag = static_cast<std::uint64_t>(ticks);
    }
    constexpr auto unit = static_cast<std::uint64_t>(kTicksPerUnit);
    appendUnsigned(out, mag / unit);
    const auto cents = static_cast<unsigned>(mag % unit);
    out.push_back('.');
    out.push_back(static_cast<char>('0' + cents / 10));
    out.push_back(static_cast<char>('0' + cents % 10));
}

std::string MessageParser::priceToString(Price ticks) {
    std::string s;
    appendPrice(s, ticks);
    return s;
}

// ─── Lines ────────────────────────────────────────────────────────────────────

MessageParser::LineResult MessageParser::parseLine(std::string_view line) {
    line = trim(line);
    if (line.empty() || line.front() == '#') return {};

    std::array<std::string_view, 10> f{};
    std::size_t n = 0;
    for (std::size_t start = 0;;) {
        const std::size_t comma = line.find(',', start);
        if (n == f.size()) return invalid("too many fields");
        f[n++] = trim(line.substr(start, comma == std::string_view::npos ? comma : comma - start));
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    if (n < 7) return invalid("expected at least 7 fields");

    LineResult  r;
    OrderEvent& ev = r.event;
    r.kind         = Kind::EVENT;

    if (!parseUnsigned(f[0], ev.timestamp)) return invalid("invalid timestamp");
    if (f[1].empty() || !Symbol::tryParse(f[1], ev.symbol))
        return invalid("invalid symbol (must be 1-16 characters)");
    if (!parseType(f[2], ev.type)) return invalid("unknown order type");
    if (!parseUnsigned(f[3], ev.order_id)) return invalid("invalid order id");

    auto side = [&]() { return parseSide(f[4], ev.side); };
    auto qty  = [&]() { return parseUnsigned(f[6], ev.qty); };
    auto price_at = [&](std::size_t i, Price& out) {
        const auto p = parsePrice(f[i]);
        if (p) out = *p;
        return p.has_value();
    };
    auto tif_at = [&](std::size_t i) { return n <= i || parseTif(f[i], ev.tif); };

    switch (ev.type) {
        case OrderType::LIMIT:
            if (n > 8)              return invalid("too many fields for LIMIT");
            if (!side())            return invalid("invalid side");
            if (!price_at(5, ev.price)) return invalid("invalid price");
            if (!qty())             return invalid("invalid quantity");
            if (!tif_at(7))         return invalid("invalid time in force");
            break;
        case OrderType::MARKET:
            if (n > 8)              return invalid("too many fields for MARKET");
            if (!side())            return invalid("invalid side");
            if (!qty())             return invalid("invalid quantity");
            if (!tif_at(7))         return invalid("invalid time in force");
            break;
        case OrderType::CANCEL:
            if (n > 7)              return invalid("too many fields for CANCEL");
            break;
        case OrderType::REPLACE:
            if (n != 10)            return invalid("REPLACE needs 10 fields");
            if (!side())            return invalid("invalid side");
            if (!parseUnsigned(f[7], ev.new_order_id)) return invalid("invalid new order id");
            if (!price_at(8, ev.new_price))            return invalid("invalid new price");
            if (!parseUnsigned(f[9], ev.new_qty))      return invalid("invalid new quantity");
            break;
        case OrderType::STOP_MARKET:
            if (n > 8)              return invalid("too many fields for STOP_MARKET");
            if (!side())            return invalid("invalid side");
            if (!price_at(5, ev.price)) return invalid("invalid stop price");
            if (!qty())             return invalid("invalid quantity");
            break;
        case OrderType::STOP_LIMIT:
            if (n != 8)             return invalid("STOP_LIMIT needs 8 fields");
            if (!side())            return invalid("invalid side");
            if (!price_at(5, ev.price))       return invalid("invalid stop price");
            if (!qty())                       return invalid("invalid quantity");
            if (!price_at(7, ev.limit_price)) return invalid("invalid limit price");
            break;
    }
    return r;
}

std::optional<OrderEvent> MessageParser::parseCSVLine(std::string_view line) {
    LineResult r = parseLine(line);
    if (r.kind != Kind::EVENT) return std::nullopt;
    return r.event;
}

MessageParser::ParsedFile MessageParser::parseCSVFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open input file: " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    ParsedFile out;
    out.events.reserve(text.size() / 32);
    out.line_numbers.reserve(text.size() / 32);
    std::size_t line_no = 0;
    for (std::size_t pos = 0; pos < text.size();) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        const std::string_view line(text.data() + pos, eol - pos);
        ++line_no;
        LineResult r = parseLine(line);
        if (r.kind == Kind::EVENT) {
            out.events.push_back(r.event);
            out.line_numbers.push_back(line_no);
        } else if (r.kind == Kind::INVALID) {
            out.errors.push_back(ParseError{line_no, std::string(r.error), std::string(trim(line))});
        }
        pos = eol + 1;
    }
    return out;
}

// ─── Output ───────────────────────────────────────────────────────────────────

void MessageParser::appendTrade(std::string& out, const Trade& t) {
    appendUnsigned(out, t.timestamp);
    out.push_back(',');
    out.append(t.symbol.view());
    out.push_back(',');
    appendPrice(out, t.price);
    out.push_back(',');
    appendUnsigned(out, t.qty);
    out.push_back(',');
    out.append(toString(t.aggressor_side));
    out.push_back(',');
    appendUnsigned(out, t.resting_order_id);
    out.push_back(',');
    appendUnsigned(out, t.aggressing_order_id);
}

void MessageParser::appendTOB(std::string& out, const TOBUpdate& t) {
    appendUnsigned(out, t.timestamp);
    out.push_back(',');
    out.append(t.symbol.view());
    out.push_back(',');
    appendPrice(out, t.best_bid_price);
    out.push_back(',');
    appendUnsigned(out, t.best_bid_qty);
    out.push_back(',');
    appendPrice(out, t.best_ask_price);
    out.push_back(',');
    appendUnsigned(out, t.best_ask_qty);
}

std::string MessageParser::tradeToCsv(const Trade& t) {
    std::string s;
    appendTrade(s, t);
    return s;
}

std::string MessageParser::tobToCsv(const TOBUpdate& t) {
    std::string s;
    appendTOB(s, t);
    return s;
}

}  // namespace lob

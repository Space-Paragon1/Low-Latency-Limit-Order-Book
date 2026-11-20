// CSV codec: strict price parsing, line validation, output formatting.

#include "helpers.h"
#include "lob/MessageParser.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace lob;
using Kind = MessageParser::LineResult::Kind;

// ─── Prices ───────────────────────────────────────────────────────────────────

TEST(parse_price_decimal) {
    ASSERT_EQ(MessageParser::parsePrice("150.25"), std::optional<Price>(15025));
    ASSERT_EQ(MessageParser::parsePrice("100.00"), std::optional<Price>(10000));
    ASSERT_EQ(MessageParser::parsePrice("0.01"), std::optional<Price>(1));
    ASSERT_EQ(MessageParser::parsePrice("150.5"), std::optional<Price>(15050));
    ASSERT_EQ(MessageParser::parsePrice("150.250"), std::optional<Price>(15025));   // trailing zeros ok
    ASSERT_EQ(MessageParser::parsePrice(" 7.10 "), std::optional<Price>(710));
}

TEST(parse_price_integer_means_whole_units) {
    // "100" and "100.00" must agree; integers are not tick counts.
    ASSERT_EQ(MessageParser::parsePrice("100"), MessageParser::parsePrice("100.00"));
    ASSERT_EQ(MessageParser::parsePrice("100"), std::optional<Price>(10000));
    ASSERT_EQ(MessageParser::parsePrice("0"), std::optional<Price>(0));
}

TEST(parse_price_rejects_malformed) {
    for (const char* bad : {"", "abc", "-1", "+1", "1.", ".5", "1e3", "1.2.3", "150.255",
                            "1,5", "99999999999999999999", "92233720368547758.07"}) {
        if (MessageParser::parsePrice(bad).has_value()) FAIL("accepted bad price '" << bad << "'");
    }
}

TEST(price_to_string) {
    ASSERT_EQ(MessageParser::priceToString(15025), std::string("150.25"));
    ASSERT_EQ(MessageParser::priceToString(5), std::string("0.05"));
    ASSERT_EQ(MessageParser::priceToString(0), std::string("0.00"));
    ASSERT_EQ(MessageParser::priceToString(10000), std::string("100.00"));
    ASSERT_EQ(MessageParser::priceToString(-150), std::string("-1.50"));
}

TEST(price_round_trip) {
    for (Price p : {Price{1}, Price{99}, Price{100}, Price{12345}, Price{987654321}}) {
        ASSERT_EQ(MessageParser::parsePrice(MessageParser::priceToString(p)), std::optional<Price>(p));
    }
}

// ─── Lines ────────────────────────────────────────────────────────────────────

TEST(parse_limit_line) {
    auto ev = MessageParser::parseCSVLine("1000000,AAPL,LIMIT,42,BUY,150.25,100");
    ASSERT_TRUE(ev.has_value());
    ASSERT_EQ(ev->timestamp, 1000000u);
    ASSERT_EQ(ev->symbol, Symbol("AAPL"));
    ASSERT_EQ(ev->type, OrderType::LIMIT);
    ASSERT_EQ(ev->order_id, 42u);
    ASSERT_EQ(ev->side, Side::BUY);
    ASSERT_EQ(ev->price, 15025);
    ASSERT_EQ(ev->qty, 100u);
    ASSERT_EQ(ev->tif, TimeInForce::GTC);
}

TEST(parse_limit_with_time_in_force) {
    ASSERT_EQ(MessageParser::parseCSVLine("1,X,LIMIT,1,BUY,1.00,1,IOC")->tif, TimeInForce::IOC);
    ASSERT_EQ(MessageParser::parseCSVLine("1,X,LIMIT,1,BUY,1.00,1,fok")->tif, TimeInForce::FOK);
    ASSERT_EQ(MessageParser::parseCSVLine("1,X,LIMIT,1,BUY,1.00,1,POST_ONLY")->tif,
              TimeInForce::POST_ONLY);
    ASSERT_EQ(MessageParser::parseCSVLine("1,X,MARKET,1,SELL,0,1,FOK")->tif, TimeInForce::FOK);
    ASSERT_EQ(MessageParser::parseLine("1,X,LIMIT,1,BUY,1.00,1,DAY").kind, Kind::INVALID);
}

TEST(parse_is_case_insensitive_and_trims) {
    auto ev = MessageParser::parseCSVLine("  5 , MSFT , limit , 7 , sell , 300.50 , 10 \r");
    ASSERT_TRUE(ev.has_value());
    ASSERT_EQ(ev->symbol, Symbol("MSFT"));
    ASSERT_EQ(ev->side, Side::SELL);
    ASSERT_EQ(ev->price, 30050);
}

TEST(parse_cancel_line) {
    auto ev = MessageParser::parseCSVLine("2000000,AAPL,CANCEL,42,BUY,0,0");
    ASSERT_TRUE(ev.has_value());
    ASSERT_EQ(ev->type, OrderType::CANCEL);
    ASSERT_EQ(ev->order_id, 42u);
}

TEST(parse_comments_and_blanks_are_skipped) {
    ASSERT_EQ(MessageParser::parseLine("# a comment").kind, Kind::SKIP);
    ASSERT_EQ(MessageParser::parseLine("   ").kind, Kind::SKIP);
    ASSERT_EQ(MessageParser::parseLine("").kind, Kind::SKIP);
    ASSERT_FALSE(MessageParser::parseCSVLine("# a comment").has_value());
}

TEST(parse_replace_line) {
    auto ev = MessageParser::parseCSVLine("3000000,AAPL,REPLACE,42,BUY,150.00,100,43,151.00,80");
    ASSERT_TRUE(ev.has_value());
    ASSERT_EQ(ev->type, OrderType::REPLACE);
    ASSERT_EQ(ev->order_id, 42u);
    ASSERT_EQ(ev->new_order_id, 43u);
    ASSERT_EQ(ev->new_price, 15100);
    ASSERT_EQ(ev->new_qty, 80u);
    ASSERT_EQ(MessageParser::parseLine("3,AAPL,REPLACE,42,BUY,150.00,100").kind, Kind::INVALID);
}

TEST(parse_stop_market_line) {
    auto ev = MessageParser::parseCSVLine("5000000,AAPL,STOP_MARKET,99,BUY,101.00,100,0");
    ASSERT_TRUE(ev.has_value());
    ASSERT_EQ(ev->type, OrderType::STOP_MARKET);
    ASSERT_EQ(ev->price, 10100);
    ASSERT_EQ(ev->qty, 100u);
    ASSERT_EQ(ev->limit_price, 0);
    ASSERT_TRUE(MessageParser::parseCSVLine("5,AAPL,STOP_MARKET,99,BUY,101.00,100").has_value());
}

TEST(parse_stop_limit_line) {
    auto ev = MessageParser::parseCSVLine("6000000,AAPL,STOP_LIMIT,100,SELL,99.00,100,98.50");
    ASSERT_TRUE(ev.has_value());
    ASSERT_EQ(ev->type, OrderType::STOP_LIMIT);
    ASSERT_EQ(ev->side, Side::SELL);
    ASSERT_EQ(ev->price, 9900);
    ASSERT_EQ(ev->limit_price, 9850);
    ASSERT_EQ(MessageParser::parseLine("6,AAPL,STOP_LIMIT,100,SELL,99.00,100").kind, Kind::INVALID);
}

TEST(parse_reports_reasons_for_bad_lines) {
    struct Case { const char* line; const char* reason; };
    const Case cases[] = {
        {"1,AAPL,LIMIT,1,BUY,1.00", "expected at least 7 fields"},
        {"x,AAPL,LIMIT,1,BUY,1.00,1", "invalid timestamp"},
        {"1,,LIMIT,1,BUY,1.00,1", "invalid symbol (must be 1-16 characters)"},
        {"1,ABCDEFGHIJKLMNOPQ,LIMIT,1,BUY,1.00,1", "invalid symbol (must be 1-16 characters)"},
        {"1,AAPL,ICEBERG,1,BUY,1.00,1", "unknown order type"},
        {"1,AAPL,LIMIT,-1,BUY,1.00,1", "invalid order id"},
        {"1,AAPL,LIMIT,1,HOLD,1.00,1", "invalid side"},
        {"1,AAPL,LIMIT,1,BUY,1.005,1", "invalid price"},
        {"1,AAPL,LIMIT,1,BUY,1.00,-5", "invalid quantity"},
        {"1,AAPL,LIMIT,1,BUY,1.00,1,GTC,extra", "too many fields for LIMIT"},
        {"1,AAPL,CANCEL,1,BUY,0,0,0", "too many fields for CANCEL"},
        {"1,A,LIMIT,1,BUY,1,1,a,b,c,d", "too many fields"},
    };
    for (const auto& c : cases) {
        const auto r = MessageParser::parseLine(c.line);
        if (r.kind != Kind::INVALID) FAIL("accepted: " << c.line);
        if (r.error != c.reason) FAIL(c.line << "\n  reason: " << r.error << "\n  wanted: " << c.reason);
    }
}

TEST(parse_file_tracks_line_numbers_and_errors) {
    const auto path = std::filesystem::temp_directory_path() / "lob_parser_test.csv";
    {
        std::ofstream out(path, std::ios::binary);
        out << "# header\r\n"
               "1,AAPL,LIMIT,1,BUY,1.00,10\r\n"
               "\r\n"
               "2,AAPL,LIMIT,2,BUY,oops,10\r\n"
               "3,AAPL,CANCEL,1,BUY,0,0";   // no trailing newline
    }
    const auto parsed = MessageParser::parseCSVFile(path.string());
    std::filesystem::remove(path);

    ASSERT_EQ(parsed.events.size(), 2u);
    ASSERT_EQ(parsed.line_numbers[0], 2u);
    ASSERT_EQ(parsed.line_numbers[1], 5u);
    ASSERT_EQ(parsed.errors.size(), 1u);
    ASSERT_EQ(parsed.errors[0].line, 4u);
    ASSERT_EQ(parsed.errors[0].message, std::string("invalid price"));
}

TEST(parse_file_missing_throws) {
    bool threw = false;
    try {
        (void)MessageParser::parseCSVFile("/definitely/not/here.csv");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

// ─── Output ───────────────────────────────────────────────────────────────────

TEST(trade_and_tob_csv_format) {
    const Trade t{1000, Symbol("AAPL"), 15025, 300, Side::SELL, 7, 9};
    ASSERT_EQ(MessageParser::tradeToCsv(t), std::string("1000,AAPL,150.25,300,SELL,7,9"));
    const TOBUpdate u{2000, Symbol("AAPL"), 15000, 10, 0, 0};
    ASSERT_EQ(MessageParser::tobToCsv(u), std::string("2000,AAPL,150.00,10,0.00,0"));
}

TEST(symbol_value_semantics) {
    const Symbol a("BTC-USDT");
    ASSERT_EQ(a.view(), std::string_view("BTC-USDT"));
    ASSERT_EQ(a.length(), 8u);
    ASSERT_EQ(a, Symbol("BTC-USDT"));
    ASSERT_NE(a, Symbol("BTC-USD"));
    ASSERT_EQ(a.hash(), Symbol("BTC-USDT").hash());
    ASSERT_EQ(Symbol("ABCDEFGHIJKLMNOP").length(), 16u);   // exactly at capacity
    Symbol out;
    ASSERT_FALSE(Symbol::tryParse("ABCDEFGHIJKLMNOPQ", out));
    ASSERT_TRUE(Symbol().empty());
}

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lob {

// ─── Primitive aliases ────────────────────────────────────────────────────────
using OrderId   = std::uint64_t;
using Price     = std::int64_t;    // integer ticks: 1 tick = 0.01, so 150.25 → 15025
using Qty       = std::uint64_t;
using Timestamp = std::uint64_t;   // nanoseconds, any monotonic origin

inline constexpr Price kTicksPerUnit = 100;

// ─── Enumerations ─────────────────────────────────────────────────────────────
enum class Side : std::uint8_t { BUY = 0, SELL = 1 };

enum class OrderType : std::uint8_t {
    LIMIT,
    MARKET,
    CANCEL,
    REPLACE,
    STOP_MARKET,   // releases a market order once the stop price trades
    STOP_LIMIT,    // releases a limit order once the stop price trades
};

enum class TimeInForce : std::uint8_t {
    GTC,        // rest any unfilled remainder (default)
    IOC,        // immediate-or-cancel: take what is available, cancel the rest
    FOK,        // fill-or-kill: fill the whole quantity immediately or do nothing
    POST_ONLY,  // maker-only: rejected if any part would execute on entry
};

enum class OrderStatus : std::uint8_t {
    ACTIVE,            // working (resting limit or pending stop), nothing filled
    PARTIALLY_FILLED,  // some quantity filled, remainder resting
    FILLED,            // fully executed
    CANCELLED,         // removed: user cancel, or unfilled IOC/FOK/market remainder
    REJECTED,          // not accepted; see RejectReason
};

enum class RejectReason : std::uint8_t {
    NONE,
    INVALID_QUANTITY,       // qty == 0
    INVALID_PRICE,          // price / stop price / limit price <= 0
    INVALID_TIME_IN_FORCE,  // e.g. POST_ONLY on a market order
    DUPLICATE_ORDER_ID,     // id collides with a live order (resting or pending stop)
    UNKNOWN_ORDER,          // cancel/replace target is not live
    POST_ONLY_WOULD_CROSS,  // post-only order would have taken liquidity
};
inline constexpr std::size_t kRejectReasonCount = 7;

constexpr Side opposite(Side s) noexcept {
    return s == Side::BUY ? Side::SELL : Side::BUY;
}

constexpr bool isStop(OrderType t) noexcept {
    return t == OrderType::STOP_MARKET || t == OrderType::STOP_LIMIT;
}

constexpr std::string_view toString(Side s) noexcept {
    return s == Side::BUY ? "BUY" : "SELL";
}

constexpr std::string_view toString(OrderType t) noexcept {
    switch (t) {
        case OrderType::LIMIT:       return "LIMIT";
        case OrderType::MARKET:      return "MARKET";
        case OrderType::CANCEL:      return "CANCEL";
        case OrderType::REPLACE:     return "REPLACE";
        case OrderType::STOP_MARKET: return "STOP_MARKET";
        case OrderType::STOP_LIMIT:  return "STOP_LIMIT";
    }
    return "?";
}

constexpr std::string_view toString(TimeInForce t) noexcept {
    switch (t) {
        case TimeInForce::GTC:       return "GTC";
        case TimeInForce::IOC:       return "IOC";
        case TimeInForce::FOK:       return "FOK";
        case TimeInForce::POST_ONLY: return "POST_ONLY";
    }
    return "?";
}

constexpr std::string_view toString(OrderStatus s) noexcept {
    switch (s) {
        case OrderStatus::ACTIVE:           return "ACTIVE";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderStatus::FILLED:           return "FILLED";
        case OrderStatus::CANCELLED:        return "CANCELLED";
        case OrderStatus::REJECTED:         return "REJECTED";
    }
    return "?";
}

constexpr std::string_view toString(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::NONE:                  return "NONE";
        case RejectReason::INVALID_QUANTITY:      return "INVALID_QUANTITY";
        case RejectReason::INVALID_PRICE:         return "INVALID_PRICE";
        case RejectReason::INVALID_TIME_IN_FORCE: return "INVALID_TIME_IN_FORCE";
        case RejectReason::DUPLICATE_ORDER_ID:    return "DUPLICATE_ORDER_ID";
        case RejectReason::UNKNOWN_ORDER:         return "UNKNOWN_ORDER";
        case RejectReason::POST_ONLY_WOULD_CROSS: return "POST_ONLY_WOULD_CROSS";
    }
    return "?";
}

std::ostream& operator<<(std::ostream& os, Side s);
std::ostream& operator<<(std::ostream& os, OrderType t);
std::ostream& operator<<(std::ostream& os, TimeInForce t);
std::ostream& operator<<(std::ostream& os, OrderStatus s);
std::ostream& operator<<(std::ostream& os, RejectReason r);

// ─── Symbol ───────────────────────────────────────────────────────────────────
// Fixed-capacity instrument name (up to 16 bytes, zero-padded).  Trivially
// copyable and compared/hashed as two machine words, so trades, TOB updates and
// events stay allocation-free POD types.
class Symbol {
public:
    static constexpr std::size_t kMaxLength = 16;

    constexpr Symbol() noexcept = default;

    // Throws std::length_error if `name` is longer than kMaxLength.
    Symbol(std::string_view name) {   // NOLINT(google-explicit-constructor): value type
        if (!tryAssign(name))
            throw std::length_error("lob::Symbol: name longer than 16 characters");
    }
    Symbol(const char* name) : Symbol(std::string_view(name)) {}   // NOLINT

    // Non-throwing construction for parsers.  Fails on over-long names or
    // names containing NUL.
    [[nodiscard]] static bool tryParse(std::string_view name, Symbol& out) noexcept {
        Symbol s;
        if (!s.tryAssign(name)) return false;
        out = s;
        return true;
    }

    [[nodiscard]] std::size_t length() const noexcept {
        std::size_t n = 0;
        while (n < kMaxLength && data_[n] != '\0') ++n;
        return n;
    }
    [[nodiscard]] bool             empty() const noexcept { return data_[0] == '\0'; }
    [[nodiscard]] std::string_view view()  const noexcept { return {data_, length()}; }
    [[nodiscard]] std::string      str()   const { return std::string(view()); }

    [[nodiscard]] std::size_t hash() const noexcept {
        std::uint64_t a = 0;
        std::uint64_t b = 0;
        std::memcpy(&a, data_, 8);
        std::memcpy(&b, data_ + 8, 8);
        std::uint64_t h = a * 0x9E3779B97F4A7C15ULL ^ (b + 0x632BE59BD9B4E019ULL);
        h ^= h >> 32;
        return static_cast<std::size_t>(h);
    }

    friend bool operator==(const Symbol& x, const Symbol& y) noexcept {
        return std::memcmp(x.data_, y.data_, kMaxLength) == 0;
    }
    friend bool operator!=(const Symbol& x, const Symbol& y) noexcept { return !(x == y); }

private:
    bool tryAssign(std::string_view name) noexcept {
        if (name.size() > kMaxLength) return false;
        if (name.find('\0') != std::string_view::npos) return false;
        std::memset(data_, 0, kMaxLength);
        if (!name.empty()) std::memcpy(data_, name.data(), name.size());
        return true;
    }

    char data_[kMaxLength] = {};
};

std::ostream& operator<<(std::ostream& os, const Symbol& s);

struct SymbolHash {
    std::size_t operator()(const Symbol& s) const noexcept { return s.hash(); }
};

// ─── Output structures ────────────────────────────────────────────────────────
struct Trade {
    Timestamp timestamp{0};
    Symbol    symbol;
    Price     price{0};                // always the resting order's price
    Qty       qty{0};
    Side      aggressor_side{Side::BUY};
    OrderId   resting_order_id{0};
    OrderId   aggressing_order_id{0};

    friend bool operator==(const Trade& a, const Trade& b) noexcept {
        return a.timestamp == b.timestamp && a.symbol == b.symbol && a.price == b.price &&
               a.qty == b.qty && a.aggressor_side == b.aggressor_side &&
               a.resting_order_id == b.resting_order_id &&
               a.aggressing_order_id == b.aggressing_order_id;
    }
    friend bool operator!=(const Trade& a, const Trade& b) noexcept { return !(a == b); }
};

struct TOBUpdate {
    Timestamp timestamp{0};
    Symbol    symbol;
    Price     best_bid_price{0};   // 0 if no bids
    Qty       best_bid_qty{0};     // aggregate qty at the best bid
    Price     best_ask_price{0};   // 0 if no asks
    Qty       best_ask_qty{0};     // aggregate qty at the best ask
};

// Non-owning view over the trades produced by one book operation.
class TradeSpan {
public:
    constexpr TradeSpan() noexcept = default;
    constexpr TradeSpan(const Trade* data, std::size_t size) noexcept : data_(data), size_(size) {}

    [[nodiscard]] constexpr const Trade* begin() const noexcept { return data_; }
    [[nodiscard]] constexpr const Trade* end()   const noexcept { return data_ + size_; }
    [[nodiscard]] constexpr std::size_t  size()  const noexcept { return size_; }
    [[nodiscard]] constexpr bool         empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr const Trade& operator[](std::size_t i) const noexcept { return data_[i]; }
    [[nodiscard]] constexpr const Trade& front() const noexcept { return data_[0]; }
    [[nodiscard]] constexpr const Trade& back()  const noexcept { return data_[size_ - 1]; }

private:
    const Trade* data_{nullptr};
    std::size_t  size_{0};
};

// Outcome of one OrderBook / MatchingEngine operation.
//
// `trades` contains every execution caused by the call, including those of
// stop orders it triggered.  It points into a buffer owned by the book and is
// valid until the next mutating call on the same book; copy it out if you need
// it longer.
struct MatchResult {
    OrderStatus  status{OrderStatus::ACTIVE};   // final state of the submitted order
    RejectReason reject_reason{RejectReason::NONE};
    Qty          filled_qty{0};                 // qty of the submitted order executed
    Qty          leaves_qty{0};                 // qty of the submitted order still working
    TradeSpan    trades;
    bool         tob_changed{false};            // best bid/ask price or qty changed

    [[nodiscard]] bool accepted() const noexcept { return status != OrderStatus::REJECTED; }
};

}  // namespace lob

template <>
struct std::hash<lob::Symbol> {
    std::size_t operator()(const lob::Symbol& s) const noexcept { return s.hash(); }
};

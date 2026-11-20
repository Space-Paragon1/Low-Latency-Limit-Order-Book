#pragma once

#include "lob/Types.h"
#include "lob/detail/ObjectPool.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lob::detail {

struct PriceLevel;

// A working order.  Lives in an ObjectPool and is threaded onto its price
// level's FIFO through intrusive prev/next links, so it can be unlinked in O(1)
// from a pointer found in the id map.  Exactly one cache line.
struct alignas(64) OrderNode {
    OrderNode*  prev{nullptr};
    OrderNode*  next{nullptr};
    PriceLevel* level{nullptr};        // level->price is the limit (or stop) price
    OrderId     id{0};
    Qty         qty{0};                // open quantity
    Price       limit_price{0};        // STOP_LIMIT: limit of the order released on trigger
    Timestamp   timestamp{0};
    Side        side{Side::BUY};
    OrderType   type{OrderType::LIMIT};
    TimeInForce tif{TimeInForce::GTC};
};
static_assert(sizeof(OrderNode) == 64, "OrderNode should occupy exactly one cache line");

// All orders at one price, in time priority.  total_qty is maintained
// incrementally so top-of-book quantity is O(1).
struct PriceLevel {
    Price         price{0};
    Qty           total_qty{0};
    OrderNode*    head{nullptr};
    OrderNode*    tail{nullptr};
    std::uint32_t order_count{0};

    [[nodiscard]] bool empty() const noexcept { return head == nullptr; }

    void pushBack(OrderNode* n) noexcept {
        n->level = this;
        n->prev  = tail;
        n->next  = nullptr;
        if (tail != nullptr) tail->next = n;
        else                 head       = n;
        tail = n;
        total_qty += n->qty;
        ++order_count;
    }

    void unlink(OrderNode* n) noexcept {
        assert(n->level == this);
        if (n->prev != nullptr) n->prev->next = n->next;
        else                    head          = n->next;
        if (n->next != nullptr) n->next->prev = n->prev;
        else                    tail          = n->prev;
        total_qty -= n->qty;
        --order_count;
        n->prev = n->next = nullptr;
        n->level = nullptr;
    }
};

// One side of a book: the non-empty price levels, kept in a contiguous array
// sorted worst → best so the best level is at the back.
//
// Market activity clusters at the inside of the book, and this layout makes
// that the cheap end: best-level access and removal are O(1) (back /
// pop_back), and inserting or erasing a level k positions from the top costs an
// O(k) memmove of 16-byte entries.  Lookups scan linearly from the top for a
// few entries, then fall back to binary search, so deep levels are O(log n).
//
// HighestIsBest = true  → bids, sell stops   (highest price has priority)
// HighestIsBest = false → asks, buy stops    (lowest price has priority)
template <bool HighestIsBest>
class PriceLadder {
public:
    static constexpr bool isBetter(Price a, Price b) noexcept {
        if constexpr (HighestIsBest) return a > b;
        else                         return a < b;
    }

    [[nodiscard]] bool        empty() const noexcept { return levels_.empty(); }
    [[nodiscard]] std::size_t size()  const noexcept { return levels_.size(); }

    // Preconditions: !empty().
    [[nodiscard]] PriceLevel* best()      const noexcept { return levels_.back().level; }
    [[nodiscard]] Price       bestPrice() const noexcept { return levels_.back().price; }

    // i = 0 is the best level.  Precondition: i < size().
    [[nodiscard]] PriceLevel* at(std::size_t i) const noexcept {
        return levels_[levels_.size() - 1 - i].level;
    }

    [[nodiscard]] PriceLevel* find(Price p) const noexcept {
        const std::size_t i = boundary(p);
        return (i > 0 && levels_[i - 1].price == p) ? levels_[i - 1].level : nullptr;
    }

    // Returns the level at `p`, creating an empty one (from `pool`) if needed.
    PriceLevel* findOrCreate(Price p, ObjectPool<PriceLevel>& pool) {
        const std::size_t i = boundary(p);
        if (i > 0 && levels_[i - 1].price == p) return levels_[i - 1].level;
        PriceLevel* lvl = pool.acquire();
        *lvl            = PriceLevel{};
        lvl->price      = p;
        levels_.insert(levels_.begin() + static_cast<std::ptrdiff_t>(i), Entry{p, lvl});
        return lvl;
    }

    // Precondition: !empty().
    void popBest() noexcept { levels_.pop_back(); }

    // Removes `lvl` from the ladder (does not return it to the pool).
    void erase(const PriceLevel* lvl) noexcept {
        if (levels_.back().level == lvl) {
            levels_.pop_back();
            return;
        }
        const std::size_t i = boundary(lvl->price);
        assert(i > 0 && levels_[i - 1].level == lvl);
        levels_.erase(levels_.begin() + static_cast<std::ptrdiff_t>(i - 1));
    }

private:
    struct Entry {
        Price       price;
        PriceLevel* level;
    };

    static constexpr std::size_t kLinearProbe = 8;

    // Index of the first entry strictly better than `p`.  Every entry before it
    // is equal to or worse than `p`, so a level at `p`, if any, sits at i - 1.
    [[nodiscard]] std::size_t boundary(Price p) const noexcept {
        std::size_t       i    = levels_.size();
        const std::size_t stop = i > kLinearProbe ? i - kLinearProbe : 0;
        while (i > stop && isBetter(levels_[i - 1].price, p)) --i;
        if (i == stop && stop > 0 && isBetter(levels_[i - 1].price, p)) {
            const auto it = std::partition_point(
                levels_.begin(), levels_.begin() + static_cast<std::ptrdiff_t>(i),
                [p](const Entry& e) { return !isBetter(e.price, p); });
            i = static_cast<std::size_t>(it - levels_.begin());
        }
        return i;
    }

    std::vector<Entry> levels_;
};

}  // namespace lob::detail

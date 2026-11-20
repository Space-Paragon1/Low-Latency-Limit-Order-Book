#pragma once

#include "lob/Types.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace lob::detail {

// Open-addressing hash map from OrderId to a non-null pointer.
//
// Linear probing over a power-of-two table, load factor <= 50%, and
// backward-shift deletion (no tombstones, so probe chains never degrade under
// heavy add/cancel churn).  Lookups touch one or two adjacent 16-byte slots in
// the common case, which is what makes cancel/replace O(1) in practice rather
// than just on paper.  An empty slot is marked by a null value, so every key
// (including 0) is usable.
template <typename T>
class OrderIdMap {
    static_assert(std::is_pointer_v<T>, "OrderIdMap stores pointers");

public:
    explicit OrderIdMap(std::size_t expected = 0) { rehash(capacityFor(expected)); }

    [[nodiscard]] T find(OrderId key) const noexcept {
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (s.value == nullptr) return nullptr;
            if (s.key == key) return s.value;
        }
    }

    // Inserts key → value; returns false (map unchanged) if the key exists.
    bool insert(OrderId key, T value) {
        if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.value == nullptr) {
                s.key   = key;
                s.value = value;
                ++size_;
                return true;
            }
            if (s.key == key) return false;
        }
    }

    bool erase(OrderId key) noexcept {
        std::size_t hole = home(key);
        for (;; hole = (hole + 1) & mask_) {
            if (slots_[hole].value == nullptr) return false;
            if (slots_[hole].key == key) break;
        }
        // Backward shift: walk the rest of the cluster and pull back any entry
        // whose home slot lies cyclically outside (hole, j]; such an entry can
        // legally occupy the hole without breaking its own probe chain.
        for (std::size_t j = (hole + 1) & mask_; slots_[j].value != nullptr; j = (j + 1) & mask_) {
            const std::size_t h = home(slots_[j].key);
            const bool h_in_range = (hole <= j) ? (hole < h && h <= j) : (hole < h || h <= j);
            if (!h_in_range) {
                slots_[hole] = slots_[j];
                hole         = j;
            }
        }
        slots_[hole].value = nullptr;
        --size_;
        return true;
    }

    void reserve(std::size_t expected) {
        const std::size_t cap = capacityFor(expected);
        if (cap > slots_.size()) rehash(cap);
    }

    [[nodiscard]] std::size_t size()  const noexcept { return size_; }
    [[nodiscard]] bool        empty() const noexcept { return size_ == 0; }

private:
    struct Slot {
        OrderId key{0};
        T       value{nullptr};
    };

    static std::size_t capacityFor(std::size_t expected) noexcept {
        std::size_t cap = 16;
        while (cap < expected * 2) cap <<= 1;
        return cap;
    }

    // splitmix64 finaliser: sequential ids spread across the whole table.
    static std::uint64_t mix(std::uint64_t x) noexcept {
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBULL;
        x ^= x >> 31;
        return x;
    }

    [[nodiscard]] std::size_t home(OrderId key) const noexcept {
        return static_cast<std::size_t>(mix(key)) & mask_;
    }

    void rehash(std::size_t new_capacity) {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(new_capacity, Slot{});
        mask_ = new_capacity - 1;
        size_ = 0;
        for (const Slot& s : old)
            if (s.value != nullptr) insert(s.key, s.value);
    }

    std::vector<Slot> slots_;
    std::size_t       mask_{0};
    std::size_t       size_{0};
};

}  // namespace lob::detail

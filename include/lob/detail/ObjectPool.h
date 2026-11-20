#pragma once

#include <cstddef>
#include <memory>
#include <vector>

namespace lob::detail {

// Fixed-type object pool with stable addresses and O(1) acquire/release.
//
// Objects are carved out of fixed-size chunks that live until the pool is
// destroyed, so pointers stay valid.  Released objects are recycled LIFO, which
// keeps recently touched (cache-hot) memory in circulation.  Once warmed up (or
// after reserve()), acquire() and release() never call the system allocator.
template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(std::size_t chunk_size = 4096)
        : chunk_size_(chunk_size == 0 ? 1 : chunk_size) {}

    ObjectPool(const ObjectPool&)            = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) noexcept            = default;
    ObjectPool& operator=(ObjectPool&&) noexcept = default;
    ~ObjectPool()                                = default;

    // Returns an object whose contents are unspecified; the caller initialises it.
    [[nodiscard]] T* acquire() {
        if (free_.empty()) grow(chunk_size_);
        T* obj = free_.back();
        free_.pop_back();
        return obj;
    }

    // `free_` always has capacity for every object the pool owns, so this
    // push_back can never reallocate.
    void release(T* obj) noexcept { free_.push_back(obj); }

    // Pre-allocates (and, through value-initialisation, pre-faults) storage for
    // at least `n` objects in total.
    void reserve(std::size_t n) {
        if (n > capacity_) grow(n - capacity_);
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t inUse()    const noexcept { return capacity_ - free_.size(); }

private:
    void grow(std::size_t n) {
        auto chunk = std::make_unique<T[]>(n);
        free_.reserve(capacity_ + n);
        for (std::size_t i = n; i-- > 0;) free_.push_back(&chunk[i]);   // chunk[0] handed out first
        capacity_ += n;
        chunks_.push_back(std::move(chunk));
    }

    std::vector<std::unique_ptr<T[]>> chunks_;
    std::vector<T*>                   free_;
    std::size_t                       chunk_size_;
    std::size_t                       capacity_{0};
};

}  // namespace lob::detail

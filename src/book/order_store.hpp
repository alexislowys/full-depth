#pragma once

#include <cstdint>
#include <vector>

namespace book {

// Flat open-addressing hash table: order reference number → order.
// Linear probing with backward-shift deletion (no tombstones — this
// table sees ~186M insert/erase cycles per replayed day, tombstone
// accumulation would rot it). Fibonacci hashing spreads the
// mostly-sequential daily reference numbers.
//
// Grows at 0.7 load. Pointers returned by find() are invalidated by the
// next insert() or erase() — callers copy what they need first.
class OrderStore {
public:
    struct Order {
        std::uint32_t shares;
        std::uint32_t price;
        std::uint16_t locate;
        char side;
    };

    explicit OrderStore(std::size_t initial_slots = 1u << 22) {
        cap_ = normalize(initial_slots);
        keys_.assign(cap_, 0);
        vals_.resize(cap_);
        full_.assign(cap_, 0);
    }

    bool insert(std::uint64_t ref, const Order& o) {
        if ((size_ + 1) * 10 >= cap_ * 7) grow();
        std::size_t i = slot(ref);
        while (full_[i]) {
            if (keys_[i] == ref) return false; // duplicate
            i = (i + 1) & (cap_ - 1);
        }
        keys_[i] = ref;
        vals_[i] = o;
        full_[i] = 1;
        ++size_;
        return true;
    }

    // Pull the probe target toward the caches ahead of find/insert —
    // the table is far larger than L2, so an unprefetched probe is a
    // DRAM stall on the hot path.
    void prefetch(std::uint64_t ref) const {
        const std::size_t i = slot(ref);
        __builtin_prefetch(&full_[i]);
        __builtin_prefetch(&keys_[i]);
        __builtin_prefetch(&vals_[i]);
    }

    Order* find(std::uint64_t ref) {
        std::size_t i = slot(ref);
        while (full_[i]) {
            if (keys_[i] == ref) return &vals_[i];
            i = (i + 1) & (cap_ - 1);
        }
        return nullptr;
    }

    const Order* find(std::uint64_t ref) const {
        return const_cast<OrderStore*>(this)->find(ref);
    }

    // No-op if ref is absent.
    void erase(std::uint64_t ref) {
        std::size_t i = slot(ref);
        while (full_[i]) {
            if (keys_[i] == ref) {
                backshift(i);
                --size_;
                return;
            }
            i = (i + 1) & (cap_ - 1);
        }
    }

    std::uint64_t size() const { return size_; }

    template <class F>
    void for_each(F&& f) const {
        for (std::size_t i = 0; i < cap_; ++i)
            if (full_[i]) f(keys_[i], vals_[i]);
    }

private:
    static std::size_t normalize(std::size_t n) {
        std::size_t c = 16;
        while (c < n) c <<= 1;
        return c;
    }

    std::size_t slot(std::uint64_t ref) const {
        return (ref * 0x9E3779B97F4A7C15ULL) >> shift();
    }
    int shift() const { return 64 - __builtin_ctzll(cap_); }

    // Backward-shift deletion: close the gap at `hole` by walking the
    // probe chain and pulling back every entry whose ideal slot allows
    // it, preserving the invariant that every key is reachable from its
    // ideal slot without crossing an empty slot.
    void backshift(std::size_t hole) {
        std::size_t i = hole;
        for (;;) {
            std::size_t j = (i + 1) & (cap_ - 1);
            while (full_[j]) {
                const std::size_t ideal = slot(keys_[j]);
                // Move keys_[j] into the hole iff the hole lies within
                // [ideal, j] cyclically — otherwise moving it would put
                // it before its ideal slot.
                const std::size_t dist_hole = (i - ideal) & (cap_ - 1);
                const std::size_t dist_j = (j - ideal) & (cap_ - 1);
                if (dist_hole <= dist_j) {
                    keys_[i] = keys_[j];
                    vals_[i] = vals_[j];
                    i = j;
                    j = (i + 1) & (cap_ - 1);
                    continue;
                }
                j = (j + 1) & (cap_ - 1);
            }
            full_[i] = 0;
            return;
        }
    }

    void grow() {
        std::vector<std::uint64_t> old_keys = std::move(keys_);
        std::vector<Order> old_vals = std::move(vals_);
        std::vector<std::uint8_t> old_full = std::move(full_);
        const std::size_t old_cap = cap_;
        cap_ <<= 1;
        keys_.assign(cap_, 0);
        vals_.assign(cap_, Order{});
        full_.assign(cap_, 0);
        size_ = 0;
        for (std::size_t i = 0; i < old_cap; ++i)
            if (old_full[i]) insert(old_keys[i], old_vals[i]);
    }

    std::size_t cap_ = 0;
    std::uint64_t size_ = 0;
    std::vector<std::uint64_t> keys_;
    std::vector<Order> vals_;
    std::vector<std::uint8_t> full_;
};

} // namespace book

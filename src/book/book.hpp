#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace book {

struct Level {
    std::uint64_t shares = 0;
    std::uint32_t orders = 0;

    bool operator==(const Level&) const = default;
};

struct Quote {
    std::uint32_t price;
    Level level;
};

// One symbol's displayed limit order book, structure-of-arrays: prices
// in their own contiguous u32 vector (16 per cache line — the binary
// search that dominated profiles touches ~6x fewer lines than with
// array-of-structs), levels in a parallel vector. Both sides
// price-ascending — all orientation combinations were measured on
// full-day replay and ascending-both won by 1.7–2.5x; best bid is the
// last bid element, best ask the first ask element.
class Book {
public:
    // Returns false on bookkeeping underflow (removing more shares than
    // the level holds, or removing from a missing level) — a violation
    // the caller counts; the level is clamped/erased so the book stays
    // usable. `order_gone` marks the removal that takes its order fully
    // off the book (delete, replace-out, final fill) so the per-level
    // order count stays exact.
    bool add(char side, std::uint32_t price, std::uint32_t shares);
    bool remove(char side, std::uint32_t price, std::uint32_t shares,
                bool order_gone);

    std::optional<Quote> best_bid() const;
    std::optional<Quote> best_ask() const;

    // Displayed book crossed or locked: best bid >= best ask. Legitimate
    // while a stock is halted/paused/quote-only or a reopening auction
    // is pending; a violation while trading normally.
    bool crossed_or_locked() const;

    std::size_t bid_levels() const { return bid_px_.size(); }
    std::size_t ask_levels() const { return ask_px_.size(); }

    // Depth accessors, i = 0 at the touch.
    Quote bid_at(std::size_t i) const {
        const std::size_t k = bid_px_.size() - 1 - i;
        return {bid_px_[k], bid_lv_[k]};
    }
    Quote ask_at(std::size_t i) const { return {ask_px_[i], ask_lv_[i]}; }

    bool operator==(const Book&) const = default;

private:
    static bool add_to(std::vector<std::uint32_t>& px,
                       std::vector<Level>& lv, std::uint32_t price,
                       std::uint32_t shares);
    static bool remove_from(std::vector<std::uint32_t>& px,
                            std::vector<Level>& lv, std::uint32_t price,
                            std::uint32_t shares, bool order_gone);

    std::vector<std::uint32_t> bid_px_, ask_px_; // ascending
    std::vector<Level> bid_lv_, ask_lv_;         // parallel to *_px_
};

} // namespace book

#pragma once

#include <cstdint>
#include <map>
#include <optional>

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

// One symbol's displayed limit order book: aggregated price levels per
// side. Correct-first implementation (std::map ladders); the
// performance pass replaces the containers, not the interface.
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
    // while a stock is halted/paused/quote-only; a violation while
    // trading. The caller knows the trading state.
    bool crossed_or_locked() const;

    std::size_t bid_levels() const { return bids_.size(); }
    std::size_t ask_levels() const { return asks_.size(); }

    const std::map<std::uint32_t, Level, std::greater<std::uint32_t>>&
    bids() const {
        return bids_;
    }
    const std::map<std::uint32_t, Level>& asks() const { return asks_; }

private:
    template <class Ladder>
    static bool add_to(Ladder& ladder, std::uint32_t price,
                       std::uint32_t shares);
    template <class Ladder>
    static bool remove_from(Ladder& ladder, std::uint32_t price,
                            std::uint32_t shares, bool order_gone);

    std::map<std::uint32_t, Level, std::greater<std::uint32_t>> bids_;
    std::map<std::uint32_t, Level> asks_;
};

} // namespace book

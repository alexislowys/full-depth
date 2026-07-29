#include "book/book.hpp"

namespace book {

template <class Ladder>
bool Book::add_to(Ladder& ladder, std::uint32_t price,
                  std::uint32_t shares) {
    auto& lvl = ladder[price];
    lvl.shares += shares;
    ++lvl.orders;
    return true;
}

template <class Ladder>
bool Book::remove_from(Ladder& ladder, std::uint32_t price,
                       std::uint32_t shares, bool order_gone) {
    const auto it = ladder.find(price);
    if (it == ladder.end()) return false;
    auto& lvl = it->second;
    bool ok = true;
    if (shares > lvl.shares) {
        lvl.shares = 0;
        ok = false;
    } else {
        lvl.shares -= shares;
    }
    if (order_gone && lvl.orders > 0) --lvl.orders;
    if (lvl.shares == 0) {
        ladder.erase(it);
    }
    return ok;
}

bool Book::add(char side, std::uint32_t price, std::uint32_t shares) {
    return side == 'B' ? add_to(bids_, price, shares)
                       : add_to(asks_, price, shares);
}

bool Book::remove(char side, std::uint32_t price, std::uint32_t shares,
                  bool order_gone) {
    return side == 'B' ? remove_from(bids_, price, shares, order_gone)
                       : remove_from(asks_, price, shares, order_gone);
}

std::optional<Quote> Book::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    const auto& [price, level] = *bids_.begin();
    return Quote{price, level};
}

std::optional<Quote> Book::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    const auto& [price, level] = *asks_.begin();
    return Quote{price, level};
}

bool Book::crossed_or_locked() const {
    if (bids_.empty() || asks_.empty()) return false;
    return bids_.begin()->first >= asks_.begin()->first;
}

} // namespace book

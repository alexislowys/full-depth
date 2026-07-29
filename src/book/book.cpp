#include "book/book.hpp"

#include <algorithm>

namespace book {

bool Book::add_to(std::vector<std::uint32_t>& px, std::vector<Level>& lv,
                  std::uint32_t price, std::uint32_t shares) {
    // Fast paths at the back — measured hot end (bid touch, ask deep).
    if (!px.empty() && px.back() == price) {
        lv.back().shares += shares;
        ++lv.back().orders;
        return true;
    }
    if (px.empty() || px.back() < price) {
        px.push_back(price);
        lv.push_back(Level{shares, 1});
        return true;
    }
    const auto it = std::lower_bound(px.begin(), px.end(), price);
    const auto k = static_cast<std::size_t>(it - px.begin());
    if (it != px.end() && *it == price) {
        lv[k].shares += shares;
        ++lv[k].orders;
    } else {
        px.insert(it, price);
        lv.insert(lv.begin() + static_cast<std::ptrdiff_t>(k),
                  Level{shares, 1});
    }
    return true;
}

bool Book::remove_from(std::vector<std::uint32_t>& px,
                       std::vector<Level>& lv, std::uint32_t price,
                       std::uint32_t shares, bool order_gone) {
    std::size_t k;
    if (!px.empty() && px.back() == price) {
        k = px.size() - 1;
    } else {
        const auto it = std::lower_bound(px.begin(), px.end(), price);
        if (it == px.end() || *it != price) return false;
        k = static_cast<std::size_t>(it - px.begin());
    }
    auto& level = lv[k];
    bool ok = true;
    if (shares > level.shares) {
        level.shares = 0;
        ok = false;
    } else {
        level.shares -= shares;
    }
    if (order_gone && level.orders > 0) --level.orders;
    if (level.shares == 0) {
        px.erase(px.begin() + static_cast<std::ptrdiff_t>(k));
        lv.erase(lv.begin() + static_cast<std::ptrdiff_t>(k));
    }
    return ok;
}

bool Book::add(char side, std::uint32_t price, std::uint32_t shares) {
    return side == 'B' ? add_to(bid_px_, bid_lv_, price, shares)
                       : add_to(ask_px_, ask_lv_, price, shares);
}

bool Book::remove(char side, std::uint32_t price, std::uint32_t shares,
                  bool order_gone) {
    return side == 'B'
               ? remove_from(bid_px_, bid_lv_, price, shares, order_gone)
               : remove_from(ask_px_, ask_lv_, price, shares, order_gone);
}

std::optional<Quote> Book::best_bid() const {
    if (bid_px_.empty()) return std::nullopt;
    return Quote{bid_px_.back(), bid_lv_.back()};
}

std::optional<Quote> Book::best_ask() const {
    if (ask_px_.empty()) return std::nullopt;
    return Quote{ask_px_.front(), ask_lv_.front()};
}

bool Book::crossed_or_locked() const {
    if (bid_px_.empty() || ask_px_.empty()) return false;
    return bid_px_.back() >= ask_px_.front();
}

} // namespace book

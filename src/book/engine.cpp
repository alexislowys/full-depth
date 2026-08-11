#include "book/engine.hpp"

#include <algorithm>
#include <cstring>

namespace book {

namespace {
constexpr std::size_t kMaxLocates = 65536;
const std::string kUnknownSymbol = "?";

// Symbols arrive right-padded with spaces; store them trimmed.
std::string trimmed(const char* stock) {
    std::string s(stock);
    s.erase(s.find_last_not_of(' ') + 1);
    return s;
}
} // namespace

Engine::Engine(std::string filter_symbol)
    : filter_symbol_(std::move(filter_symbol)),
      books_(kMaxLocates),
      state_(kMaxLocates, 'H'), // absent from the H spin = treat as halted
      auction_pending_(kMaxLocates, false),
      locate_ops_(kMaxLocates),
      symbols_(kMaxLocates) {}

const Book* Engine::book_for(std::uint16_t locate) const {
    return &books_[locate];
}

const std::string& Engine::symbol_of(std::uint16_t locate) const {
    return symbols_[locate].empty() ? kUnknownSymbol : symbols_[locate];
}

void Engine::on_stock_directory(const itch::StockDirectory& m) {
    symbols_[m.h.locate] = trimmed(m.stock);
    if (!filter_symbol_.empty() && symbols_[m.h.locate] == filter_symbol_)
        target_locate_ = m.h.locate;
}

void Engine::on_stock_trading_action(const itch::StockTradingAction& m) {
    if (m.state == 'T' && state_[m.h.locate] != 'T')
        auction_pending_[m.h.locate] = true;
    state_[m.h.locate] = m.state;
}

void Engine::on_cross_trade(const itch::CrossTrade& m) {
    auction_pending_[m.h.locate] = false;
}

void Engine::check_crossed(std::uint16_t locate, std::uint64_t ts_ns) {
    if (state_[locate] == 'T' && books_[locate].crossed_or_locked()) {
        if (auction_pending_[locate]) {
            ++violations_.crossed_auction_pending;
            return;
        }
        ++violations_.crossed_while_trading;
        if (crossed_events_.size() < 10000) {
            crossed_events_.push_back(
                {locate, ts_ns, books_[locate].best_bid()->price,
                 books_[locate].best_ask()->price});
        }
    }
}

void Engine::on_add_order(const itch::AddOrder& m) {
    if (!tracked(m.h.locate)) return;
    if (!orders_.insert(m.ref,
                        Order{m.shares, m.price, m.h.locate, m.side})) {
        ++violations_.duplicate_ref;
        return;
    }
    books_[m.h.locate].add(m.side, m.price, m.shares);
    ++stats_.adds;
    ++locate_ops_[m.h.locate].adds;
    stats_.peak_live_orders =
        std::max<std::uint64_t>(stats_.peak_live_orders, orders_.size());
    check_crossed(m.h.locate, m.h.ts_ns);
}

Engine::Order* Engine::find(std::uint64_t ref,
                            std::uint64_t& unknown_counter) {
    Order* ord = orders_.find(ref);
    if (!ord) ++unknown_counter;
    return ord;
}

// Shared reduction path for executions, partial cancels, and deletes.
void Engine::reduce(std::uint64_t ref, std::uint32_t shares,
                    bool full_delete, std::uint64_t& unknown_counter) {
    Order* ord = find(ref, unknown_counter);
    if (!ord) return;
    std::uint32_t n = full_delete ? ord->shares : shares;
    if (n > ord->shares) {
        ++violations_.over_reduction;
        n = ord->shares;
    }
    const bool gone = (n == ord->shares);
    if (!books_[ord->locate].remove(ord->side, ord->price, n, gone))
        ++violations_.ladder_underflow;
    if (gone) {
        orders_.erase(ref);
    } else {
        ord->shares -= n;
    }
}

void Engine::on_order_executed(const itch::OrderExecuted& m) {
    if (!tracked(m.h.locate)) return;
    // Locate the order first so violation attribution stays exact.
    Order* ord = find(m.ref, violations_.unknown_ref);
    if (!ord) return;
    const std::uint16_t locate = ord->locate;
    std::uint32_t n = m.shares;
    if (n > ord->shares) {
        ++violations_.over_reduction;
        n = ord->shares;
    }
    const bool gone = (n == ord->shares);
    if (!books_[locate].remove(ord->side, ord->price, n, gone))
        ++violations_.ladder_underflow;
    if (gone)
        orders_.erase(m.ref);
    else
        ord->shares -= n;
    ++stats_.executes;
    stats_.executed_shares += n;
    ++locate_ops_[locate].executes;
    check_crossed(locate, m.h.ts_ns);
}

void Engine::on_order_executed_price(const itch::OrderExecutedPrice& m) {
    // Book impact is at the order's *display* price; the execution price
    // in the message is for trade reporting only.
    on_order_executed(itch::OrderExecuted{m.h, m.ref, m.shares, m.match});
}

void Engine::on_order_cancel(const itch::OrderCancel& m) {
    if (!tracked(m.h.locate)) return;
    Order* ord = find(m.ref, violations_.unknown_ref);
    if (!ord) return;
    const std::uint16_t locate = ord->locate;
    if (m.shares >= ord->shares) {
        // Spec reserves X for partial cancels; a cancel wiping the order
        // is an oddity worth counting (and an over_reduction if it
        // exceeds the remainder).
        ++violations_.cancel_to_zero;
    }
    reduce(m.ref, m.shares, false, violations_.unknown_ref);
    ++stats_.cancels;
    ++locate_ops_[locate].cancels;
    check_crossed(locate, m.h.ts_ns);
}

void Engine::on_order_delete(const itch::OrderDelete& m) {
    if (!tracked(m.h.locate)) return;
    reduce(m.ref, 0, true, violations_.unknown_ref);
    ++stats_.deletes;
    ++locate_ops_[m.h.locate].deletes;
    check_crossed(m.h.locate, m.h.ts_ns);
}

void Engine::on_order_replace(const itch::OrderReplace& m) {
    if (!tracked(m.h.locate)) return;
    Order* orig = find(m.orig_ref, violations_.replace_unknown);
    if (!orig) return; // no side/symbol to inherit — cannot synthesize
    // Copy before erase — the store invalidates pointers on mutation.
    const Order old = *orig;
    const std::uint16_t locate = old.locate;
    const char side = old.side;
    if (!books_[locate].remove(side, old.price, old.shares, true))
        ++violations_.ladder_underflow;
    orders_.erase(m.orig_ref);
    if (!orders_.insert(m.new_ref, Order{m.shares, m.price, locate, side})) {
        ++violations_.duplicate_ref;
        return;
    }
    books_[locate].add(side, m.price, m.shares);
    ++stats_.replaces;
    stats_.peak_live_orders =
        std::max<std::uint64_t>(stats_.peak_live_orders, orders_.size());
    ++locate_ops_[locate].replaces;
    check_crossed(locate, m.h.ts_ns);
}

bool Engine::audit(std::uint16_t locate) const {
    Book rebuilt;
    orders_.for_each([&](std::uint64_t, const Order& ord) {
        if (ord.locate == locate)
            rebuilt.add(ord.side, ord.price, ord.shares);
    });
    return rebuilt == books_[locate];
}

} // namespace book

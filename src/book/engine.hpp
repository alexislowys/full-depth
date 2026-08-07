#pragma once

#include "book/book.hpp"
#include "book/order_store.hpp"
#include "itch/decoder.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace book {

// Everything that can go wrong while replaying a day. All zeros after a
// full replay is the correctness claim of this project.
struct Violations {
    std::uint64_t duplicate_ref = 0;   // add with an already-live ref
    std::uint64_t unknown_ref = 0;     // E/C/X/D against a ref never seen
    std::uint64_t replace_unknown = 0; // U whose original ref is missing
    std::uint64_t over_reduction = 0;  // exec/cancel > remaining shares
    std::uint64_t ladder_underflow = 0; // level bookkeeping went negative
    // bid >= ask in state 'T' with no reopening/IPO cross pending — a
    // genuinely inexplicable crossed book.
    std::uint64_t crossed_while_trading = 0;
    std::uint64_t cancel_to_zero = 0; // X leaving 0 shares (expect D instead)
    // Crossed while a post-resumption/IPO auction had not yet printed
    // its Q cross — legitimate market state, tracked for transparency.
    std::uint64_t crossed_auction_pending = 0;

    std::uint64_t total() const {
        return duplicate_ref + unknown_ref + replace_unknown +
               over_reduction + ladder_underflow + crossed_while_trading;
        // cancel_to_zero is an oddity counter, not a correctness failure
    }
};

struct EngineStats {
    std::uint64_t adds = 0;
    std::uint64_t executes = 0;
    std::uint64_t cancels = 0;
    std::uint64_t deletes = 0;
    std::uint64_t replaces = 0;
    std::uint64_t executed_shares = 0;
    std::uint64_t peak_live_orders = 0;
};

// Whole-market order book engine. Plugs into itch::decode_stream as the
// handler; optionally restricted to a single symbol (week-3 gate mode).
class Engine : public itch::NullHandler {
public:
    // Empty filter = build books for every symbol.
    explicit Engine(std::string filter_symbol = {});

    // Lookahead hook from decode_stream: prefetch the order-store slot
    // the next message's reference number will probe.
    void prefetch_hint(const std::uint8_t* p, std::uint16_t) {
        switch (p[0]) {
            case 'A':
            case 'F':
            case 'E':
            case 'C':
            case 'X':
            case 'D':
            case 'U':
                orders_.prefetch(itch::be64(p + 11));
                break;
            default:
                break;
        }
    }

    void on_stock_directory(const itch::StockDirectory& m);
    void on_stock_trading_action(const itch::StockTradingAction& m);
    void on_cross_trade(const itch::CrossTrade& m);
    void on_add_order(const itch::AddOrder& m);
    void on_order_executed(const itch::OrderExecuted& m);
    void on_order_executed_price(const itch::OrderExecutedPrice& m);
    void on_order_cancel(const itch::OrderCancel& m);
    void on_order_delete(const itch::OrderDelete& m);
    void on_order_replace(const itch::OrderReplace& m);

    struct CrossedEvent {
        std::uint16_t locate;
        std::uint64_t ts_ns;
        std::uint32_t bid;
        std::uint32_t ask;
    };

    const Violations& violations() const { return violations_; }
    const std::vector<CrossedEvent>& crossed_events() const {
        return crossed_events_;
    }
    const EngineStats& stats() const { return stats_; }
    std::uint64_t live_orders() const { return orders_.size(); }

    using Order = OrderStore::Order;

    // Locate resolved from the directory spin for the filter symbol;
    // 0 if unfiltered or not (yet) seen.
    std::uint16_t target_locate() const { return target_locate_; }
    const Book* book_for(std::uint16_t locate) const;
    const std::string& symbol_of(std::uint16_t locate) const;

    // Read-only order lookup for wrapping handlers (e.g. the export
    // layer peeks the resting order before forwarding an execution).
    const Order* peek(std::uint64_t ref) const { return orders_.find(ref); }

    // Rebuild one symbol's ladders from the live order store and compare
    // with the incrementally maintained book. True iff identical. The
    // end-of-day audit: incremental state must equal from-scratch state.
    bool audit(std::uint16_t locate) const;

private:
    bool tracked(std::uint16_t locate) const {
        return filter_symbol_.empty() || locate == target_locate_;
    }
    Order* find(std::uint64_t ref, std::uint64_t& unknown_counter);
    void reduce(std::uint64_t ref, std::uint32_t shares, bool full_delete,
                std::uint64_t& unknown_counter);
    void check_crossed(std::uint16_t locate, std::uint64_t ts_ns);

    std::string filter_symbol_;
    std::uint16_t target_locate_ = 0;
    OrderStore orders_;
    std::vector<Book> books_;
    std::vector<char> state_;         // trading state per locate ('H' default)
    // True from a transition into 'T' until that symbol's next Q cross:
    // the reopening/IPO auction hasn't printed, so a crossed displayed
    // book is expected (observed: IPO releases ANPC/BDTX, LULD
    // resumptions RKDA/DTSS on the 2020-01-30 sample day).
    std::vector<bool> auction_pending_;
    std::vector<std::string> symbols_;
    Violations violations_;
    std::vector<CrossedEvent> crossed_events_;
    EngineStats stats_;
};

} // namespace book

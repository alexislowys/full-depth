#pragma once

#include "itch/be.hpp"
#include "itch/messages.hpp"

#include <cstddef>
#include <cstdint>

// Dispatch layer: routes a framed ITCH stream to a handler. The handler
// is a duck-typed template parameter so the compiler can inline the hot
// path; inherit NullHandler and override only the callbacks you need.

namespace itch {

struct NullHandler {
    // Called with the NEXT message's payload before the current one is
    // dispatched — a hook for handlers to issue memory prefetches that
    // hide DRAM latency on large lookup structures. No-op by default.
    void prefetch_hint(const std::uint8_t*, std::uint16_t) {}

    void on_system_event(const SystemEvent&) {}
    void on_stock_directory(const StockDirectory&) {}
    void on_stock_trading_action(const StockTradingAction&) {}
    void on_reg_sho(const RegSho&) {}
    void on_market_participant_position(const MarketParticipantPosition&) {}
    void on_mwcb_decline(const MwcbDecline&) {}
    void on_mwcb_status(const MwcbStatus&) {}
    void on_ipo_quoting(const IpoQuoting&) {}
    void on_luld_collar(const LuldCollar&) {}
    void on_operational_halt(const OperationalHalt&) {}
    void on_add_order(const AddOrder&) {}
    void on_order_executed(const OrderExecuted&) {}
    void on_order_executed_price(const OrderExecutedPrice&) {}
    void on_order_cancel(const OrderCancel&) {}
    void on_order_delete(const OrderDelete&) {}
    void on_order_replace(const OrderReplace&) {}
    void on_trade(const Trade&) {}
    void on_cross_trade(const CrossTrade&) {}
    void on_broken_trade(const BrokenTrade&) {}
    void on_noii(const Noii&) {}
    void on_rpii(const Rpii&) {}
    void on_direct_listing(const DirectListing&) {}
};

enum class DecodeStatus : std::uint8_t {
    Ok,
    UnknownType,
    BadLength,
};

// Decode one message payload (p[0] is the type byte) and invoke the
// matching handler callback.
template <class Handler>
DecodeStatus decode_message(const std::uint8_t* p, std::uint16_t len,
                            Handler&& h) {
    const std::uint8_t type = p[0];
    const int want = expected_length(type);
    if (want < 0) return DecodeStatus::UnknownType;
    if (len != want) return DecodeStatus::BadLength;
    switch (type) {
        case 'S': h.on_system_event(decode_system_event(p)); break;
        case 'R': h.on_stock_directory(decode_stock_directory(p)); break;
        case 'H':
            h.on_stock_trading_action(decode_stock_trading_action(p));
            break;
        case 'Y': h.on_reg_sho(decode_reg_sho(p)); break;
        case 'L':
            h.on_market_participant_position(
                decode_market_participant_position(p));
            break;
        case 'V': h.on_mwcb_decline(decode_mwcb_decline(p)); break;
        case 'W': h.on_mwcb_status(decode_mwcb_status(p)); break;
        case 'K': h.on_ipo_quoting(decode_ipo_quoting(p)); break;
        case 'J': h.on_luld_collar(decode_luld_collar(p)); break;
        case 'h': h.on_operational_halt(decode_operational_halt(p)); break;
        case 'A': h.on_add_order(decode_add_order(p, false)); break;
        case 'F': h.on_add_order(decode_add_order(p, true)); break;
        case 'E': h.on_order_executed(decode_order_executed(p)); break;
        case 'C':
            h.on_order_executed_price(decode_order_executed_price(p));
            break;
        case 'X': h.on_order_cancel(decode_order_cancel(p)); break;
        case 'D': h.on_order_delete(decode_order_delete(p)); break;
        case 'U': h.on_order_replace(decode_order_replace(p)); break;
        case 'P': h.on_trade(decode_trade(p)); break;
        case 'Q': h.on_cross_trade(decode_cross_trade(p)); break;
        case 'B': h.on_broken_trade(decode_broken_trade(p)); break;
        case 'I': h.on_noii(decode_noii(p)); break;
        case 'N': h.on_rpii(decode_rpii(p)); break;
        case 'O': h.on_direct_listing(decode_direct_listing(p)); break;
    }
    return DecodeStatus::Ok;
}

struct StreamResult {
    std::uint64_t messages = 0;
    std::uint64_t bytes = 0; // consumed, including 2-byte length prefixes
    DecodeStatus status = DecodeStatus::Ok;
    bool clean_eof = false;
    std::uint8_t failed_type = 0; // wire type that stopped the stream
};

// Walk a complete framed stream, dispatching every message. Stops on the
// first malformed frame, unknown type, or length mismatch.
template <class Handler>
StreamResult decode_stream(const std::uint8_t* data, std::size_t size,
                           Handler&& h) {
    StreamResult r;
    std::size_t pos = 0;
    while (pos + 2 <= size) {
        const std::uint16_t len = be16(data + pos);
        if (len == 0 || pos + 2 + len > size) return r;
        // One-frame lookahead: let the handler start pulling whatever
        // the next message will touch while we process this one.
        const std::size_t next = pos + 2u + len;
        if (next + 2 <= size) {
            const std::uint16_t next_len = be16(data + next);
            if (next_len != 0 && next + 2 + next_len <= size)
                h.prefetch_hint(data + next + 2, next_len);
        }
        const DecodeStatus s = decode_message(data + pos + 2, len, h);
        if (s != DecodeStatus::Ok) {
            r.status = s;
            r.failed_type = data[pos + 2];
            return r;
        }
        ++r.messages;
        pos += 2u + len;
        r.bytes = pos;
    }
    r.clean_eof = (pos == size);
    return r;
}

} // namespace itch

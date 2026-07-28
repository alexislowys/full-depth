#pragma once

#include <cstdint>

// Decoded TotalView-ITCH 5.0 messages. One struct per wire type; field
// names and offsets follow the official spec (docs/spec-notes.md holds
// the verified offset tables). Prices are unsigned fixed-point with 4
// decimal places except MWCB decline levels, which carry 8 (Price(8)).
// Fixed-width text fields are NUL-terminated for convenience; the wire
// pads them with trailing spaces.

namespace itch {

struct Header {
    std::uint16_t locate;
    std::uint16_t tracking;
    std::uint64_t ts_ns; // nanoseconds since midnight ET
};

struct SystemEvent { // 'S'
    Header h;
    char event_code; // O,S,Q,M,E,C daily lifecycle
};

struct StockDirectory { // 'R'
    Header h;
    char stock[9];
    char market_category;
    char financial_status;
    std::uint32_t round_lot_size;
    char round_lots_only;
    char issue_classification;
    char issue_subtype[3];
    char authenticity;
    char short_sale_threshold;
    char ipo_flag;
    char luld_tier;
    char etp_flag;
    std::uint32_t etp_leverage;
    char inverse;
};

struct StockTradingAction { // 'H'
    Header h;
    char stock[9];
    char state; // H,P,Q,T
    char reserved;
    char reason[5];
};

struct RegSho { // 'Y'
    Header h;
    char stock[9];
    char action; // 0,1,2
};

struct MarketParticipantPosition { // 'L'
    Header h;
    char mpid[5];
    char stock[9];
    char primary_mm;
    char mm_mode;
    char state;
};

struct MwcbDecline { // 'V'  (levels are Price(8))
    Header h;
    std::uint64_t level1;
    std::uint64_t level2;
    std::uint64_t level3;
};

struct MwcbStatus { // 'W'
    Header h;
    char breached_level; // 1,2,3
};

struct IpoQuoting { // 'K'
    Header h;
    char stock[9];
    std::uint32_t release_time_s; // seconds since midnight
    char qualifier;               // A,C
    std::uint32_t price;
};

struct LuldCollar { // 'J'
    Header h;
    char stock[9];
    std::uint32_t reference_price;
    std::uint32_t upper_price;
    std::uint32_t lower_price;
    std::uint32_t extensions;
};

struct OperationalHalt { // 'h'
    Header h;
    char stock[9];
    char market_code; // Q,B,X
    char action;      // H,T
};

struct AddOrder { // 'A' and 'F'; mpid blank for 'A'
    Header h;
    std::uint64_t ref;
    char side; // B,S
    std::uint32_t shares;
    char stock[9];
    std::uint32_t price;
    char mpid[5];
};

struct OrderExecuted { // 'E'
    Header h;
    std::uint64_t ref;
    std::uint32_t shares;
    std::uint64_t match;
};

struct OrderExecutedPrice { // 'C'
    Header h;
    std::uint64_t ref;
    std::uint32_t shares;
    std::uint64_t match;
    char printable; // Y,N
    std::uint32_t price;
};

struct OrderCancel { // 'X' (partial cancel; order stays on book)
    Header h;
    std::uint64_t ref;
    std::uint32_t shares;
};

struct OrderDelete { // 'D'
    Header h;
    std::uint64_t ref;
};

struct OrderReplace { // 'U' (side/stock inherited from original order)
    Header h;
    std::uint64_t orig_ref;
    std::uint64_t new_ref;
    std::uint32_t shares;
    std::uint32_t price;
};

struct Trade { // 'P' non-cross; ref is zero-filled since 2010
    Header h;
    std::uint64_t ref;
    char side;
    std::uint32_t shares;
    char stock[9];
    std::uint32_t price;
    std::uint64_t match;
};

struct CrossTrade { // 'Q' — shares is 8 bytes, unlike everywhere else
    Header h;
    std::uint64_t shares;
    char stock[9];
    std::uint32_t price;
    std::uint64_t match;
    char cross_type; // O,C,H
};

struct BrokenTrade { // 'B'
    Header h;
    std::uint64_t match;
};

struct Noii { // 'I'
    Header h;
    std::uint64_t paired_shares;
    std::uint64_t imbalance_shares;
    char direction; // B,S,N,O,P
    char stock[9];
    std::uint32_t far_price;
    std::uint32_t near_price;
    std::uint32_t reference_price;
    char cross_type;         // O,C,H,A
    char price_variation;    // L,1..9,A,B,C,space
};

struct Rpii { // 'N'
    Header h;
    char stock[9];
    char interest_flag; // B,S,A,N
};

struct DirectListing { // 'O'
    Header h;
    char stock[9];
    char eligibility; // Y,N
    std::uint32_t min_price;
    std::uint32_t max_price;
    std::uint32_t near_price;
    std::uint64_t near_time;
    std::uint32_t lower_collar;
    std::uint32_t upper_collar;
};

// Expected payload length (including the type byte) for a wire type, or
// -1 if the type is not part of ITCH 5.0.
int expected_length(std::uint8_t type);

// Per-type field decoders. `p` points at the payload (p[0] is the type
// byte); the caller has already validated the length via
// expected_length. 'A'/'F' share decode_add_order (mpid blanked for 'A').
SystemEvent decode_system_event(const std::uint8_t* p);
StockDirectory decode_stock_directory(const std::uint8_t* p);
StockTradingAction decode_stock_trading_action(const std::uint8_t* p);
RegSho decode_reg_sho(const std::uint8_t* p);
MarketParticipantPosition decode_market_participant_position(
    const std::uint8_t* p);
MwcbDecline decode_mwcb_decline(const std::uint8_t* p);
MwcbStatus decode_mwcb_status(const std::uint8_t* p);
IpoQuoting decode_ipo_quoting(const std::uint8_t* p);
LuldCollar decode_luld_collar(const std::uint8_t* p);
OperationalHalt decode_operational_halt(const std::uint8_t* p);
AddOrder decode_add_order(const std::uint8_t* p, bool with_mpid);
OrderExecuted decode_order_executed(const std::uint8_t* p);
OrderExecutedPrice decode_order_executed_price(const std::uint8_t* p);
OrderCancel decode_order_cancel(const std::uint8_t* p);
OrderDelete decode_order_delete(const std::uint8_t* p);
OrderReplace decode_order_replace(const std::uint8_t* p);
Trade decode_trade(const std::uint8_t* p);
CrossTrade decode_cross_trade(const std::uint8_t* p);
BrokenTrade decode_broken_trade(const std::uint8_t* p);
Noii decode_noii(const std::uint8_t* p);
Rpii decode_rpii(const std::uint8_t* p);
DirectListing decode_direct_listing(const std::uint8_t* p);

} // namespace itch

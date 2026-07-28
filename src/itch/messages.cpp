#include "itch/messages.hpp"

#include "itch/be.hpp"

#include <cstring>

namespace itch {

namespace {

Header header(const std::uint8_t* p) {
    return Header{be16(p + 1), be16(p + 3), be48(p + 5)};
}

// Copy a fixed-width alpha field and NUL-terminate.
template <std::size_t N>
void alpha(char (&dst)[N], const std::uint8_t* src) {
    std::memcpy(dst, src, N - 1);
    dst[N - 1] = '\0';
}

} // namespace

int expected_length(std::uint8_t type) {
    switch (type) {
        case 'S': return 12;
        case 'R': return 39;
        case 'H': return 25;
        case 'Y': return 20;
        case 'L': return 26;
        case 'V': return 35;
        case 'W': return 12;
        case 'K': return 28;
        case 'J': return 35;
        case 'h': return 21;
        case 'A': return 36;
        case 'F': return 40;
        case 'E': return 31;
        case 'C': return 36;
        case 'X': return 23;
        case 'D': return 19;
        case 'U': return 35;
        case 'P': return 44;
        case 'Q': return 40;
        case 'B': return 19;
        case 'I': return 50;
        case 'N': return 20;
        case 'O': return 48;
        default: return -1;
    }
}

SystemEvent decode_system_event(const std::uint8_t* p) {
    return {header(p), static_cast<char>(p[11])};
}

StockDirectory decode_stock_directory(const std::uint8_t* p) {
    StockDirectory m;
    m.h = header(p);
    alpha(m.stock, p + 11);
    m.market_category = static_cast<char>(p[19]);
    m.financial_status = static_cast<char>(p[20]);
    m.round_lot_size = be32(p + 21);
    m.round_lots_only = static_cast<char>(p[25]);
    m.issue_classification = static_cast<char>(p[26]);
    alpha(m.issue_subtype, p + 27);
    m.authenticity = static_cast<char>(p[29]);
    m.short_sale_threshold = static_cast<char>(p[30]);
    m.ipo_flag = static_cast<char>(p[31]);
    m.luld_tier = static_cast<char>(p[32]);
    m.etp_flag = static_cast<char>(p[33]);
    m.etp_leverage = be32(p + 34);
    m.inverse = static_cast<char>(p[38]);
    return m;
}

StockTradingAction decode_stock_trading_action(const std::uint8_t* p) {
    StockTradingAction m;
    m.h = header(p);
    alpha(m.stock, p + 11);
    m.state = static_cast<char>(p[19]);
    m.reserved = static_cast<char>(p[20]);
    alpha(m.reason, p + 21);
    return m;
}

RegSho decode_reg_sho(const std::uint8_t* p) {
    RegSho m;
    m.h = header(p);
    alpha(m.stock, p + 11);
    m.action = static_cast<char>(p[19]);
    return m;
}

MarketParticipantPosition decode_market_participant_position(
    const std::uint8_t* p) {
    MarketParticipantPosition m;
    m.h = header(p);
    alpha(m.mpid, p + 11);
    alpha(m.stock, p + 15);
    m.primary_mm = static_cast<char>(p[23]);
    m.mm_mode = static_cast<char>(p[24]);
    m.state = static_cast<char>(p[25]);
    return m;
}

MwcbDecline decode_mwcb_decline(const std::uint8_t* p) {
    return {header(p), be64(p + 11), be64(p + 19), be64(p + 27)};
}

MwcbStatus decode_mwcb_status(const std::uint8_t* p) {
    return {header(p), static_cast<char>(p[11])};
}

IpoQuoting decode_ipo_quoting(const std::uint8_t* p) {
    IpoQuoting m;
    m.h = header(p);
    alpha(m.stock, p + 11);
    m.release_time_s = be32(p + 19);
    m.qualifier = static_cast<char>(p[23]);
    m.price = be32(p + 24);
    return m;
}

LuldCollar decode_luld_collar(const std::uint8_t* p) {
    LuldCollar m;
    m.h = header(p);
    alpha(m.stock, p + 11);
    m.reference_price = be32(p + 19);
    m.upper_price = be32(p + 23);
    m.lower_price = be32(p + 27);
    m.extensions = be32(p + 31);
    return m;
}

OperationalHalt decode_operational_halt(const std::uint8_t* p) {
    OperationalHalt m;
    m.h = header(p);
    alpha(m.stock, p + 11);
    m.market_code = static_cast<char>(p[19]);
    m.action = static_cast<char>(p[20]);
    return m;
}

AddOrder decode_add_order(const std::uint8_t* p, bool with_mpid) {
    AddOrder m;
    m.h = header(p);
    m.ref = be64(p + 11);
    m.side = static_cast<char>(p[19]);
    m.shares = be32(p + 20);
    alpha(m.stock, p + 24);
    m.price = be32(p + 32);
    if (with_mpid) {
        alpha(m.mpid, p + 36);
    } else {
        m.mpid[0] = '\0';
    }
    return m;
}

OrderExecuted decode_order_executed(const std::uint8_t* p) {
    return {header(p), be64(p + 11), be32(p + 19), be64(p + 23)};
}

OrderExecutedPrice decode_order_executed_price(const std::uint8_t* p) {
    return {header(p),     be64(p + 11),
            be32(p + 19),  be64(p + 23),
            static_cast<char>(p[31]), be32(p + 32)};
}

OrderCancel decode_order_cancel(const std::uint8_t* p) {
    return {header(p), be64(p + 11), be32(p + 19)};
}

OrderDelete decode_order_delete(const std::uint8_t* p) {
    return {header(p), be64(p + 11)};
}

OrderReplace decode_order_replace(const std::uint8_t* p) {
    return {header(p), be64(p + 11), be64(p + 19), be32(p + 27),
            be32(p + 31)};
}

Trade decode_trade(const std::uint8_t* p) {
    Trade m;
    m.h = header(p);
    m.ref = be64(p + 11);
    m.side = static_cast<char>(p[19]);
    m.shares = be32(p + 20);
    alpha(m.stock, p + 24);
    m.price = be32(p + 32);
    m.match = be64(p + 36);
    return m;
}

CrossTrade decode_cross_trade(const std::uint8_t* p) {
    CrossTrade m;
    m.h = header(p);
    m.shares = be64(p + 11);
    alpha(m.stock, p + 19);
    m.price = be32(p + 27);
    m.match = be64(p + 31);
    m.cross_type = static_cast<char>(p[39]);
    return m;
}

BrokenTrade decode_broken_trade(const std::uint8_t* p) {
    return {header(p), be64(p + 11)};
}

Noii decode_noii(const std::uint8_t* p) {
    Noii m;
    m.h = header(p);
    m.paired_shares = be64(p + 11);
    m.imbalance_shares = be64(p + 19);
    m.direction = static_cast<char>(p[27]);
    alpha(m.stock, p + 28);
    m.far_price = be32(p + 36);
    m.near_price = be32(p + 40);
    m.reference_price = be32(p + 44);
    m.cross_type = static_cast<char>(p[48]);
    m.price_variation = static_cast<char>(p[49]);
    return m;
}

Rpii decode_rpii(const std::uint8_t* p) {
    Rpii m;
    m.h = header(p);
    alpha(m.stock, p + 11);
    m.interest_flag = static_cast<char>(p[19]);
    return m;
}

DirectListing decode_direct_listing(const std::uint8_t* p) {
    DirectListing m;
    m.h = header(p);
    alpha(m.stock, p + 11);
    m.eligibility = static_cast<char>(p[19]);
    m.min_price = be32(p + 20);
    m.max_price = be32(p + 24);
    m.near_price = be32(p + 28);
    m.near_time = be64(p + 32);
    m.lower_collar = be32(p + 40);
    m.upper_collar = be32(p + 44);
    return m;
}

} // namespace itch

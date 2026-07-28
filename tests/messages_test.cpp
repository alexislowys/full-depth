// Field-exact decoder fixtures for every ITCH 5.0 message type.
//
// Fixtures are constructed byte-by-byte from the offset tables in the
// official spec (docs/spec-notes.md) — the spec is the oracle, not the
// sample data file.

#include "itch/messages.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint16_t kLocate = 0x1234;
constexpr std::uint16_t kTracking = 0x5678;
// 09:30:00.000000007 ET in nanoseconds — needs all 48 timestamp bits' width.
constexpr std::uint64_t kTs = 34'200'000'000'007ULL;

class Builder {
public:
    Builder& c(char v) {
        b_.push_back(static_cast<std::uint8_t>(v));
        return *this;
    }
    Builder& u16(std::uint16_t v) {
        b_.push_back(v >> 8);
        b_.push_back(v & 0xff);
        return *this;
    }
    Builder& u32(std::uint32_t v) {
        for (int i = 3; i >= 0; --i) b_.push_back((v >> (8 * i)) & 0xff);
        return *this;
    }
    Builder& u48(std::uint64_t v) {
        for (int i = 5; i >= 0; --i) b_.push_back((v >> (8 * i)) & 0xff);
        return *this;
    }
    Builder& u64(std::uint64_t v) {
        for (int i = 7; i >= 0; --i) b_.push_back((v >> (8 * i)) & 0xff);
        return *this;
    }
    // Alpha field: right-padded with spaces to n bytes.
    Builder& str(const char* s, std::size_t n) {
        const std::size_t len = std::strlen(s);
        for (std::size_t i = 0; i < n; ++i)
            b_.push_back(i < len ? static_cast<std::uint8_t>(s[i]) : ' ');
        return *this;
    }
    Builder& header(char type) {
        return c(type).u16(kLocate).u16(kTracking).u48(kTs);
    }
    const std::uint8_t* data() const { return b_.data(); }
    std::size_t size() const { return b_.size(); }

private:
    std::vector<std::uint8_t> b_;
};

void expect_header(const itch::Header& h) {
    EXPECT_EQ(h.locate, kLocate);
    EXPECT_EQ(h.tracking, kTracking);
    EXPECT_EQ(h.ts_ns, kTs);
}

#define EXPECT_SIZED(b, type) \
    ASSERT_EQ(static_cast<int>((b).size()), itch::expected_length(type))

TEST(Messages, SystemEvent) {
    Builder b;
    b.header('S').c('Q');
    EXPECT_SIZED(b, 'S');
    const auto m = itch::decode_system_event(b.data());
    expect_header(m.h);
    EXPECT_EQ(m.event_code, 'Q');
}

TEST(Messages, StockDirectory) {
    Builder b;
    b.header('R')
        .str("AAPL", 8)
        .c('Q')          // market category
        .c('N')          // financial status
        .u32(100)        // round lot size
        .c('N')          // round lots only
        .c('C')          // issue classification
        .str("Z", 2)     // issue sub-type
        .c('P')          // authenticity
        .c('N')          // short sale threshold
        .c('N')          // IPO flag
        .c('1')          // LULD tier
        .c('N')          // ETP flag
        .u32(3)          // ETP leverage
        .c('N');         // inverse
    EXPECT_SIZED(b, 'R');
    const auto m = itch::decode_stock_directory(b.data());
    expect_header(m.h);
    EXPECT_STREQ(m.stock, "AAPL    ");
    EXPECT_EQ(m.market_category, 'Q');
    EXPECT_EQ(m.financial_status, 'N');
    EXPECT_EQ(m.round_lot_size, 100u);
    EXPECT_EQ(m.round_lots_only, 'N');
    EXPECT_EQ(m.issue_classification, 'C');
    EXPECT_STREQ(m.issue_subtype, "Z ");
    EXPECT_EQ(m.authenticity, 'P');
    EXPECT_EQ(m.short_sale_threshold, 'N');
    EXPECT_EQ(m.ipo_flag, 'N');
    EXPECT_EQ(m.luld_tier, '1');
    EXPECT_EQ(m.etp_flag, 'N');
    EXPECT_EQ(m.etp_leverage, 3u);
    EXPECT_EQ(m.inverse, 'N');
}

TEST(Messages, StockTradingAction) {
    Builder b;
    b.header('H').str("MSFT", 8).c('T').c(' ').str("LUDP", 4);
    EXPECT_SIZED(b, 'H');
    const auto m = itch::decode_stock_trading_action(b.data());
    expect_header(m.h);
    EXPECT_STREQ(m.stock, "MSFT    ");
    EXPECT_EQ(m.state, 'T');
    EXPECT_STREQ(m.reason, "LUDP");
}

TEST(Messages, RegSho) {
    Builder b;
    b.header('Y').str("TSLA", 8).c('1');
    EXPECT_SIZED(b, 'Y');
    const auto m = itch::decode_reg_sho(b.data());
    expect_header(m.h);
    EXPECT_STREQ(m.stock, "TSLA    ");
    EXPECT_EQ(m.action, '1');
}

TEST(Messages, MarketParticipantPosition) {
    Builder b;
    b.header('L').str("NSDQ", 4).str("AMD", 8).c('Y').c('N').c('A');
    EXPECT_SIZED(b, 'L');
    const auto m = itch::decode_market_participant_position(b.data());
    expect_header(m.h);
    EXPECT_STREQ(m.mpid, "NSDQ");
    EXPECT_STREQ(m.stock, "AMD     ");
    EXPECT_EQ(m.primary_mm, 'Y');
    EXPECT_EQ(m.mm_mode, 'N');
    EXPECT_EQ(m.state, 'A');
}

TEST(Messages, MwcbDecline) {
    Builder b;
    // Price(8): 8 decimal places, e.g. 3229.10 = 322910000000
    b.header('V').u64(322910000000ULL).u64(305930000000ULL).u64(271940000000ULL);
    EXPECT_SIZED(b, 'V');
    const auto m = itch::decode_mwcb_decline(b.data());
    expect_header(m.h);
    EXPECT_EQ(m.level1, 322910000000ULL);
    EXPECT_EQ(m.level2, 305930000000ULL);
    EXPECT_EQ(m.level3, 271940000000ULL);
}

TEST(Messages, MwcbStatus) {
    Builder b;
    b.header('W').c('2');
    EXPECT_SIZED(b, 'W');
    const auto m = itch::decode_mwcb_status(b.data());
    expect_header(m.h);
    EXPECT_EQ(m.breached_level, '2');
}

TEST(Messages, IpoQuoting) {
    Builder b;
    b.header('K').str("NEWCO", 8).u32(37800).c('A').u32(180000);
    EXPECT_SIZED(b, 'K');
    const auto m = itch::decode_ipo_quoting(b.data());
    expect_header(m.h);
    EXPECT_STREQ(m.stock, "NEWCO   ");
    EXPECT_EQ(m.release_time_s, 37800u); // 10:30:00
    EXPECT_EQ(m.qualifier, 'A');
    EXPECT_EQ(m.price, 180000u); // $18.0000
}

TEST(Messages, LuldCollar) {
    Builder b;
    b.header('J').str("GME", 8).u32(1500000).u32(1650000).u32(1350000).u32(2);
    EXPECT_SIZED(b, 'J');
    const auto m = itch::decode_luld_collar(b.data());
    expect_header(m.h);
    EXPECT_STREQ(m.stock, "GME     ");
    EXPECT_EQ(m.reference_price, 1500000u);
    EXPECT_EQ(m.upper_price, 1650000u);
    EXPECT_EQ(m.lower_price, 1350000u);
    EXPECT_EQ(m.extensions, 2u);
}

TEST(Messages, OperationalHalt) {
    Builder b;
    b.header('h').str("INTC", 8).c('Q').c('H');
    EXPECT_SIZED(b, 'h');
    const auto m = itch::decode_operational_halt(b.data());
    expect_header(m.h);
    EXPECT_STREQ(m.stock, "INTC    ");
    EXPECT_EQ(m.market_code, 'Q');
    EXPECT_EQ(m.action, 'H');
}

TEST(Messages, AddOrderNoMpid) {
    Builder b;
    b.header('A')
        .u64(0x1122334455667788ULL)
        .c('B')
        .u32(300)
        .str("NVDA", 8)
        .u32(2504500); // $250.4500
    EXPECT_SIZED(b, 'A');
    const auto m = itch::decode_add_order(b.data(), false);
    expect_header(m.h);
    EXPECT_EQ(m.ref, 0x1122334455667788ULL);
    EXPECT_EQ(m.side, 'B');
    EXPECT_EQ(m.shares, 300u);
    EXPECT_STREQ(m.stock, "NVDA    ");
    EXPECT_EQ(m.price, 2504500u);
    EXPECT_STREQ(m.mpid, "");
}

TEST(Messages, AddOrderWithMpid) {
    Builder b;
    b.header('F')
        .u64(42)
        .c('S')
        .u32(100)
        .str("QQQ", 8)
        .u32(3300000)
        .str("JPMS", 4);
    EXPECT_SIZED(b, 'F');
    const auto m = itch::decode_add_order(b.data(), true);
    expect_header(m.h);
    EXPECT_EQ(m.ref, 42u);
    EXPECT_EQ(m.side, 'S');
    EXPECT_EQ(m.shares, 100u);
    EXPECT_STREQ(m.stock, "QQQ     ");
    EXPECT_EQ(m.price, 3300000u);
    EXPECT_STREQ(m.mpid, "JPMS");
}

TEST(Messages, OrderExecuted) {
    Builder b;
    b.header('E').u64(42).u32(50).u64(0xAABBCCDDEEFF0011ULL);
    EXPECT_SIZED(b, 'E');
    const auto m = itch::decode_order_executed(b.data());
    expect_header(m.h);
    EXPECT_EQ(m.ref, 42u);
    EXPECT_EQ(m.shares, 50u);
    EXPECT_EQ(m.match, 0xAABBCCDDEEFF0011ULL);
}

TEST(Messages, OrderExecutedPrice) {
    Builder b;
    b.header('C').u64(42).u32(50).u64(77).c('Y').u32(1999900);
    EXPECT_SIZED(b, 'C');
    const auto m = itch::decode_order_executed_price(b.data());
    expect_header(m.h);
    EXPECT_EQ(m.ref, 42u);
    EXPECT_EQ(m.shares, 50u);
    EXPECT_EQ(m.match, 77u);
    EXPECT_EQ(m.printable, 'Y');
    EXPECT_EQ(m.price, 1999900u);
}

TEST(Messages, OrderCancel) {
    Builder b;
    b.header('X').u64(42).u32(25);
    EXPECT_SIZED(b, 'X');
    const auto m = itch::decode_order_cancel(b.data());
    expect_header(m.h);
    EXPECT_EQ(m.ref, 42u);
    EXPECT_EQ(m.shares, 25u);
}

TEST(Messages, OrderDelete) {
    Builder b;
    b.header('D').u64(0xFFFFFFFFFFFFFFFFULL);
    EXPECT_SIZED(b, 'D');
    const auto m = itch::decode_order_delete(b.data());
    expect_header(m.h);
    EXPECT_EQ(m.ref, 0xFFFFFFFFFFFFFFFFULL);
}

TEST(Messages, OrderReplace) {
    Builder b;
    b.header('U').u64(42).u64(43).u32(200).u32(1000000);
    EXPECT_SIZED(b, 'U');
    const auto m = itch::decode_order_replace(b.data());
    expect_header(m.h);
    EXPECT_EQ(m.orig_ref, 42u);
    EXPECT_EQ(m.new_ref, 43u);
    EXPECT_EQ(m.shares, 200u);
    EXPECT_EQ(m.price, 1000000u);
}

TEST(Messages, Trade) {
    Builder b;
    b.header('P').u64(0).c('B').u32(100).str("SPY", 8).u32(3250000).u64(99);
    EXPECT_SIZED(b, 'P');
    const auto m = itch::decode_trade(b.data());
    expect_header(m.h);
    EXPECT_EQ(m.ref, 0u); // zero-filled on the wire since 2010
    EXPECT_EQ(m.side, 'B');
    EXPECT_EQ(m.shares, 100u);
    EXPECT_STREQ(m.stock, "SPY     ");
    EXPECT_EQ(m.price, 3250000u);
    EXPECT_EQ(m.match, 99u);
}

TEST(Messages, CrossTrade) {
    Builder b;
    // Shares is 8 bytes here — the one place ITCH widens it.
    b.header('Q').u64(5000000000ULL).str("AAPL", 8).u32(3180000).u64(7).c('C');
    EXPECT_SIZED(b, 'Q');
    const auto m = itch::decode_cross_trade(b.data());
    expect_header(m.h);
    EXPECT_EQ(m.shares, 5000000000ULL);
    EXPECT_STREQ(m.stock, "AAPL    ");
    EXPECT_EQ(m.price, 3180000u);
    EXPECT_EQ(m.match, 7u);
    EXPECT_EQ(m.cross_type, 'C');
}

TEST(Messages, BrokenTrade) {
    Builder b;
    b.header('B').u64(99);
    EXPECT_SIZED(b, 'B');
    const auto m = itch::decode_broken_trade(b.data());
    expect_header(m.h);
    EXPECT_EQ(m.match, 99u);
}

TEST(Messages, Noii) {
    Builder b;
    b.header('I')
        .u64(1000)
        .u64(500)
        .c('B')
        .str("AAPL", 8)
        .u32(3180000)
        .u32(3181000)
        .u32(3180500)
        .c('O')
        .c('L');
    EXPECT_SIZED(b, 'I');
    const auto m = itch::decode_noii(b.data());
    expect_header(m.h);
    EXPECT_EQ(m.paired_shares, 1000u);
    EXPECT_EQ(m.imbalance_shares, 500u);
    EXPECT_EQ(m.direction, 'B');
    EXPECT_STREQ(m.stock, "AAPL    ");
    EXPECT_EQ(m.far_price, 3180000u);
    EXPECT_EQ(m.near_price, 3181000u);
    EXPECT_EQ(m.reference_price, 3180500u);
    EXPECT_EQ(m.cross_type, 'O');
    EXPECT_EQ(m.price_variation, 'L');
}

TEST(Messages, Rpii) {
    Builder b;
    b.header('N').str("IBM", 8).c('A');
    EXPECT_SIZED(b, 'N');
    const auto m = itch::decode_rpii(b.data());
    expect_header(m.h);
    EXPECT_STREQ(m.stock, "IBM     ");
    EXPECT_EQ(m.interest_flag, 'A');
}

TEST(Messages, DirectListing) {
    Builder b;
    b.header('O')
        .str("DLCR", 8)
        .c('Y')
        .u32(160000)
        .u32(1440000)
        .u32(800000)
        .u64(kTs + 1)
        .u32(720000)
        .u32(880000);
    EXPECT_SIZED(b, 'O');
    const auto m = itch::decode_direct_listing(b.data());
    expect_header(m.h);
    EXPECT_STREQ(m.stock, "DLCR    ");
    EXPECT_EQ(m.eligibility, 'Y');
    EXPECT_EQ(m.min_price, 160000u);
    EXPECT_EQ(m.max_price, 1440000u);
    EXPECT_EQ(m.near_price, 800000u);
    EXPECT_EQ(m.near_time, kTs + 1);
    EXPECT_EQ(m.lower_collar, 720000u);
    EXPECT_EQ(m.upper_collar, 880000u);
}

TEST(Messages, ExpectedLengthRejectsUnknown) {
    EXPECT_EQ(itch::expected_length('Z'), -1);
    EXPECT_EQ(itch::expected_length(0x00), -1);
    EXPECT_EQ(itch::expected_length('a'), -1);
}

} // namespace

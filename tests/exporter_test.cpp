#include "export/exporter.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

static_assert(sizeof(fdexport::TradeRecord) == 24);
static_assert(offsetof(fdexport::TradeRecord, ts_ns) == 0);
static_assert(offsetof(fdexport::TradeRecord, price) == 8);
static_assert(offsetof(fdexport::TradeRecord, shares) == 12);
static_assert(offsetof(fdexport::TradeRecord, locate) == 16);
static_assert(offsetof(fdexport::TradeRecord, side) == 18);
static_assert(offsetof(fdexport::TradeRecord, kind) == 19);
static_assert(offsetof(fdexport::TradeRecord, pad) == 20);

static_assert(sizeof(fdexport::L1Record) == 32);
static_assert(offsetof(fdexport::L1Record, ts_ns) == 0);
static_assert(offsetof(fdexport::L1Record, bid_px) == 8);
static_assert(offsetof(fdexport::L1Record, ask_px) == 12);
static_assert(offsetof(fdexport::L1Record, bid_sz) == 16);
static_assert(offsetof(fdexport::L1Record, ask_sz) == 20);
static_assert(offsetof(fdexport::L1Record, locate) == 24);
static_assert(offsetof(fdexport::L1Record, bid_ct) == 26);
static_assert(offsetof(fdexport::L1Record, ask_ct) == 28);
static_assert(offsetof(fdexport::L1Record, pad) == 30);

constexpr std::uint64_t kTs = 34'200'000'000'000ULL;

itch::Header hdr(std::uint16_t locate, std::uint64_t ts = kTs) {
    return {locate, 0, ts};
}

itch::AddOrder add(std::uint16_t locate, std::uint64_t ref, char side,
                   std::uint32_t shares, std::uint32_t price,
                   std::uint64_t ts = kTs) {
    itch::AddOrder m{};
    m.h = hdr(locate, ts);
    m.ref = ref;
    m.side = side;
    m.shares = shares;
    std::strcpy(m.stock, "TEST    ");
    m.price = price;
    m.mpid[0] = '\0';
    return m;
}

itch::StockDirectory directory(std::uint16_t locate, const char* stock) {
    itch::StockDirectory m{};
    m.h = hdr(locate);
    std::snprintf(m.stock, sizeof m.stock, "%-8s", stock);
    return m;
}

itch::StockTradingAction trading(std::uint16_t locate, char state) {
    itch::StockTradingAction m{};
    m.h = hdr(locate);
    m.state = state;
    return m;
}

itch::Trade ptrade(std::uint16_t locate, char side, std::uint32_t shares,
                   std::uint32_t price, std::uint64_t ts = kTs) {
    itch::Trade m{};
    m.h = hdr(locate, ts);
    m.ref = 0;
    m.side = side;
    m.shares = shares;
    std::strcpy(m.stock, "TEST    ");
    m.price = price;
    m.match = 900;
    return m;
}

itch::CrossTrade qcross(std::uint16_t locate, std::uint64_t shares,
                        std::uint32_t price, std::uint64_t ts = kTs) {
    itch::CrossTrade m{};
    m.h = hdr(locate, ts);
    m.shares = shares;
    std::strcpy(m.stock, "TEST    ");
    m.price = price;
    m.match = 901;
    m.cross_type = 'O';
    return m;
}

template <class R>
std::vector<R> read_records(const std::string& path) {
    std::vector<R> out;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    R r;
    while (std::fread(&r, sizeof r, 1, f) == 1) out.push_back(r);
    std::fclose(f);
    return out;
}

// Lines with trailing spaces/CR stripped, so space-padded symbols
// compare equal to their trimmed form.
std::vector<std::string> read_lines(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() &&
               (line.back() == ' ' || line.back() == '\r'))
            line.pop_back();
        out.push_back(line);
    }
    return out;
}

void expect_trade(const std::vector<fdexport::TradeRecord>& v,
                  std::size_t i, std::uint64_t ts, std::uint32_t price,
                  std::uint32_t shares, std::uint16_t locate,
                  std::uint8_t side, std::uint8_t kind) {
    SCOPED_TRACE("trade[" + std::to_string(i) + "]");
    const auto& r = v[i];
    EXPECT_EQ(r.ts_ns, ts);
    EXPECT_EQ(r.price, price);
    EXPECT_EQ(r.shares, shares);
    EXPECT_EQ(r.locate, locate);
    EXPECT_EQ(r.side, side);
    EXPECT_EQ(r.kind, kind);
    EXPECT_EQ(r.pad, 0u);
}

void expect_l1(const std::vector<fdexport::L1Record>& v, std::size_t i,
               std::uint64_t ts, std::uint32_t bid_px,
               std::uint32_t ask_px, std::uint32_t bid_sz,
               std::uint32_t ask_sz, std::uint16_t locate,
               std::uint16_t bid_ct, std::uint16_t ask_ct) {
    SCOPED_TRACE("l1[" + std::to_string(i) + "]");
    const auto& r = v[i];
    EXPECT_EQ(r.ts_ns, ts);
    EXPECT_EQ(r.bid_px, bid_px);
    EXPECT_EQ(r.ask_px, ask_px);
    EXPECT_EQ(r.bid_sz, bid_sz);
    EXPECT_EQ(r.ask_sz, ask_sz);
    EXPECT_EQ(r.locate, locate);
    EXPECT_EQ(r.bid_ct, bid_ct);
    EXPECT_EQ(r.ask_ct, ask_ct);
    EXPECT_EQ(r.pad, 0u);
}

class ExporterTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = testing::TempDir() + "exporter_" +
               ::testing::UnitTest::GetInstance()
                   ->current_test_info()
                   ->name();
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }

    static void prime(fdexport::Exporter& x, std::uint16_t locate = 7,
                      const char* stock = "TEST") {
        x.on_stock_directory(directory(locate, stock));
        x.on_stock_trading_action(trading(locate, 'T'));
    }

    std::vector<fdexport::TradeRecord> trades() const {
        return read_records<fdexport::TradeRecord>(dir_ + "/trades.bin");
    }
    std::vector<fdexport::L1Record> l1() const {
        return read_records<fdexport::L1Record>(dir_ + "/l1.bin");
    }

    std::string dir_;
};

TEST_F(ExporterTest, StaticLayout) {
    EXPECT_EQ(sizeof(fdexport::TradeRecord), 24u);
    EXPECT_EQ(offsetof(fdexport::TradeRecord, ts_ns), 0u);
    EXPECT_EQ(offsetof(fdexport::TradeRecord, price), 8u);
    EXPECT_EQ(offsetof(fdexport::TradeRecord, shares), 12u);
    EXPECT_EQ(offsetof(fdexport::TradeRecord, locate), 16u);
    EXPECT_EQ(offsetof(fdexport::TradeRecord, side), 18u);
    EXPECT_EQ(offsetof(fdexport::TradeRecord, kind), 19u);
    EXPECT_EQ(offsetof(fdexport::TradeRecord, pad), 20u);

    EXPECT_EQ(sizeof(fdexport::L1Record), 32u);
    EXPECT_EQ(offsetof(fdexport::L1Record, ts_ns), 0u);
    EXPECT_EQ(offsetof(fdexport::L1Record, bid_px), 8u);
    EXPECT_EQ(offsetof(fdexport::L1Record, ask_px), 12u);
    EXPECT_EQ(offsetof(fdexport::L1Record, bid_sz), 16u);
    EXPECT_EQ(offsetof(fdexport::L1Record, ask_sz), 20u);
    EXPECT_EQ(offsetof(fdexport::L1Record, locate), 24u);
    EXPECT_EQ(offsetof(fdexport::L1Record, bid_ct), 26u);
    EXPECT_EQ(offsetof(fdexport::L1Record, ask_ct), 28u);
    EXPECT_EQ(offsetof(fdexport::L1Record, pad), 30u);
}

TEST_F(ExporterTest, AddOrdersEmitL1) {
    {
        fdexport::Exporter x(dir_);
        prime(x);
        x.on_add_order(add(7, 1, 'B', 100, 999000, kTs));
        x.on_add_order(add(7, 2, 'S', 300, 1001000, kTs + 1));
        x.on_add_order(add(7, 3, 'B', 200, 999000, kTs + 2));
    }
    EXPECT_TRUE(trades().empty());
    const auto a = l1();
    ASSERT_EQ(a.size(), 3u);
    expect_l1(a, 0, kTs, 999000, 0, 100, 0, 7, 1, 0);
    expect_l1(a, 1, kTs + 1, 999000, 1001000, 100, 300, 7, 1, 1);
    expect_l1(a, 2, kTs + 2, 999000, 1001000, 300, 300, 7, 2, 1);
}

TEST_F(ExporterTest, DeepAddNoL1) {
    {
        fdexport::Exporter x(dir_);
        prime(x);
        x.on_add_order(add(7, 1, 'B', 100, 999000, kTs));
        x.on_add_order(add(7, 2, 'B', 50, 998000, kTs + 1));
    }
    EXPECT_TRUE(trades().empty());
    const auto a = l1();
    ASSERT_EQ(a.size(), 1u);
    expect_l1(a, 0, kTs, 999000, 0, 100, 0, 7, 1, 0);
}

TEST_F(ExporterTest, ExecutionEmitsTradeAndL1) {
    {
        fdexport::Exporter x(dir_);
        prime(x);
        x.on_add_order(add(7, 1, 'B', 100, 999000, kTs));
        x.on_order_executed({hdr(7, kTs + 5), 1, 40, 500});
    }
    const auto t = trades();
    ASSERT_EQ(t.size(), 1u);
    expect_trade(t, 0, kTs + 5, 999000, 40, 7, 'B', 0);
    const auto a = l1();
    ASSERT_EQ(a.size(), 2u);
    expect_l1(a, 0, kTs, 999000, 0, 100, 0, 7, 1, 0);
    expect_l1(a, 1, kTs + 5, 999000, 0, 60, 0, 7, 1, 0);
}

TEST_F(ExporterTest, ExecutedWithPricePrintable) {
    {
        fdexport::Exporter x(dir_);
        prime(x);
        x.on_add_order(add(7, 1, 'S', 100, 1001000, kTs));
        x.on_order_executed_price(
            {hdr(7, kTs + 5), 1, 40, 500, 'Y', 1000500});
    }
    const auto t = trades();
    ASSERT_EQ(t.size(), 1u);
    // Execution price from the message, not the 1001000 display price.
    expect_trade(t, 0, kTs + 5, 1000500, 40, 7, 'S', 1);
    const auto a = l1();
    ASSERT_EQ(a.size(), 2u);
    expect_l1(a, 0, kTs, 0, 1001000, 0, 100, 7, 0, 1);
    expect_l1(a, 1, kTs + 5, 0, 1001000, 0, 60, 7, 0, 1);
}

TEST_F(ExporterTest, ExecutedWithPriceNonPrintable) {
    {
        fdexport::Exporter x(dir_);
        prime(x);
        x.on_add_order(add(7, 1, 'S', 100, 1001000, kTs));
        x.on_order_executed_price(
            {hdr(7, kTs + 5), 1, 40, 500, 'N', 1000500});
    }
    EXPECT_TRUE(trades().empty());
    const auto a = l1();
    ASSERT_EQ(a.size(), 2u);
    expect_l1(a, 0, kTs, 0, 1001000, 0, 100, 7, 0, 1);
    expect_l1(a, 1, kTs + 5, 0, 1001000, 0, 60, 7, 0, 1);
}

TEST_F(ExporterTest, NonCrossTrade) {
    {
        fdexport::Exporter x(dir_);
        prime(x);
        x.on_add_order(add(7, 1, 'B', 100, 999000, kTs));
        x.on_trade(ptrade(7, 'B', 75, 1000200, kTs + 5));
        EXPECT_EQ(x.engine().live_orders(), 1u);
        const auto bid = x.engine().book_for(7)->best_bid();
        ASSERT_TRUE(bid.has_value());
        EXPECT_EQ(bid->level.shares, 100u);
    }
    const auto t = trades();
    ASSERT_EQ(t.size(), 1u);
    expect_trade(t, 0, kTs + 5, 1000200, 75, 7, 'B', 2);
    const auto a = l1();
    ASSERT_EQ(a.size(), 1u);
    expect_l1(a, 0, kTs, 999000, 0, 100, 0, 7, 1, 0);
}

TEST_F(ExporterTest, CrossTrade) {
    {
        fdexport::Exporter x(dir_);
        prime(x);
        x.on_add_order(add(7, 1, 'B', 100, 999000, kTs));
        x.on_cross_trade(qcross(7, 5000, 1000000, kTs + 5));
        x.on_cross_trade(qcross(7, (1ULL << 32) + 5, 1000000, kTs + 6));
    }
    const auto t = trades();
    ASSERT_EQ(t.size(), 2u);
    expect_trade(t, 0, kTs + 5, 1000000, 5000, 7, 0, 3);
    // u64 share count saturates at u32 max.
    expect_trade(t, 1, kTs + 6, 1000000, UINT32_MAX, 7, 0, 3);
    ASSERT_EQ(l1().size(), 1u);
}

TEST_F(ExporterTest, DeleteRestoresL1) {
    {
        fdexport::Exporter x(dir_);
        prime(x);
        x.on_add_order(add(7, 1, 'B', 100, 999000, kTs));
        x.on_add_order(add(7, 2, 'B', 50, 998000, kTs + 1));
        x.on_order_delete({hdr(7, kTs + 5), 1});
        x.on_order_delete({hdr(7, kTs + 6), 2});
    }
    EXPECT_TRUE(trades().empty());
    const auto a = l1();
    ASSERT_EQ(a.size(), 3u);
    expect_l1(a, 0, kTs, 999000, 0, 100, 0, 7, 1, 0);
    expect_l1(a, 1, kTs + 5, 998000, 0, 50, 0, 7, 1, 0);
    expect_l1(a, 2, kTs + 6, 0, 0, 0, 0, 7, 0, 0);
}

TEST_F(ExporterTest, ReplaceEmitsL1) {
    {
        fdexport::Exporter x(dir_);
        prime(x);
        x.on_add_order(add(7, 1, 'B', 100, 999000, kTs));
        x.on_order_replace({hdr(7, kTs + 5), 1, 2, 150, 998500});
    }
    EXPECT_TRUE(trades().empty());
    const auto a = l1();
    ASSERT_EQ(a.size(), 2u);
    expect_l1(a, 0, kTs, 999000, 0, 100, 0, 7, 1, 0);
    expect_l1(a, 1, kTs + 5, 998500, 0, 150, 0, 7, 1, 0);
}

TEST_F(ExporterTest, FilterSymbol) {
    {
        fdexport::Exporter x(dir_, "TGT");
        x.on_stock_directory(directory(3, "TGT"));
        x.on_stock_directory(directory(4, "OTHER"));
        x.on_stock_trading_action(trading(3, 'T'));
        x.on_stock_trading_action(trading(4, 'T'));
        x.on_add_order(add(4, 1, 'B', 100, 999000, kTs));
        x.on_trade(ptrade(4, 'B', 75, 1000200, kTs + 1));
        x.on_cross_trade(qcross(4, 500, 1000000, kTs + 2));
        x.on_add_order(add(3, 2, 'B', 100, 999000, kTs + 3));
        EXPECT_EQ(x.engine().target_locate(), 3u);
        EXPECT_EQ(x.trades_written(), 0u);
        EXPECT_EQ(x.l1_written(), 1u);
    }
    EXPECT_TRUE(trades().empty());
    const auto a = l1();
    ASSERT_EQ(a.size(), 1u);
    expect_l1(a, 0, kTs + 3, 999000, 0, 100, 0, 3, 1, 0);
}

TEST_F(ExporterTest, CountsAndMeta) {
    std::uint64_t tw = 0;
    std::uint64_t lw = 0;
    {
        fdexport::Exporter x(dir_);
        x.on_stock_directory(directory(7, "TEST"));
        x.on_stock_directory(directory(8, "OTHER"));
        x.on_stock_trading_action(trading(7, 'T'));
        x.on_add_order(add(7, 1, 'B', 100, 999000, kTs));
        x.on_add_order(add(7, 2, 'S', 200, 1001000, kTs + 1));
        x.on_order_executed({hdr(7, kTs + 2), 1, 40, 500});
        x.on_trade(ptrade(7, 'B', 75, 1000200, kTs + 3));
        x.on_cross_trade(qcross(7, 500, 1000000, kTs + 4));
        x.write_symbols_and_meta("/data/sample.itch", 8);
        tw = x.trades_written();
        lw = x.l1_written();
        EXPECT_TRUE(x.all_ok());
    }
    EXPECT_EQ(tw, 3u);
    EXPECT_EQ(lw, 3u);
    EXPECT_EQ(std::filesystem::file_size(dir_ + "/trades.bin"),
              tw * sizeof(fdexport::TradeRecord));
    EXPECT_EQ(std::filesystem::file_size(dir_ + "/l1.bin"),
              lw * sizeof(fdexport::L1Record));

    const auto rows = read_lines(dir_ + "/symbols.csv");
    ASSERT_GE(rows.size(), 3u);
    EXPECT_EQ(rows[0], "locate,symbol");
    EXPECT_EQ(std::count(rows.begin() + 1, rows.end(), "7,TEST"), 1);
    EXPECT_EQ(std::count(rows.begin() + 1, rows.end(), "8,OTHER"), 1);

    std::map<std::string, std::string> kv;
    for (const auto& line : read_lines(dir_ + "/meta.txt")) {
        const auto eq = line.find('=');
        if (eq != std::string::npos)
            kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    EXPECT_EQ(kv["source"], "/data/sample.itch");
    EXPECT_EQ(kv["messages"], "8");
    EXPECT_EQ(kv["trades"], "3");
    EXPECT_EQ(kv["l1"], "3");
}

TEST_F(ExporterTest, UnknownRefEmitsNothing) {
    {
        fdexport::Exporter x(dir_);
        prime(x);
        x.on_order_executed({hdr(7), 99, 10, 500});
        x.on_order_executed_price({hdr(7), 99, 10, 501, 'Y', 1000500});
        EXPECT_EQ(x.engine().violations().unknown_ref, 2u);
        x.close();
        EXPECT_TRUE(x.all_ok());
    }
    EXPECT_TRUE(trades().empty());
    EXPECT_TRUE(l1().empty()); // book never changed
}

TEST_F(ExporterTest, WriteFailureSurfaces) {
    fdexport::Exporter x(dir_ + "/no_such_subdir");
    prime(x);
    x.on_add_order(add(7, 1, 'B', 100, 999000));
    x.on_trade(ptrade(7, 'B', 100, 999000));
    EXPECT_FALSE(x.all_ok());
    EXPECT_EQ(x.trades_written(), 0u);
    EXPECT_EQ(x.l1_written(), 0u);
    EXPECT_FALSE(x.write_symbols_and_meta("src", 1));
    x.close();
    EXPECT_FALSE(x.all_ok());
}

TEST_F(ExporterTest, L1SizeAndCountClamp) {
    {
        fdexport::Exporter x(dir_);
        prime(x);
        // Two orders at one price: level total 0x1E0000000 > u32 max.
        x.on_add_order(add(7, 1, 'B', 0xF0000000u, 999000));
        x.on_add_order(add(7, 2, 'B', 0xF0000000u, 999000, kTs + 1));
        // Count clamp on the ask side: 70000 one-share orders.
        for (std::uint64_t r = 0; r < 70000; ++r)
            x.on_add_order(add(7, 1000 + r, 'S', 1, 1001000, kTs + 2 + r));
        x.close();
    }
    const auto a = l1();
    ASSERT_EQ(a.size(), 2u + 70000u);
    EXPECT_EQ(a[1].bid_sz, 0xFFFFFFFFu); // saturated, not truncated
    EXPECT_EQ(a[1].bid_ct, 2u);
    EXPECT_EQ(a.back().ask_ct, 0xFFFFu); // saturated, not wrapped
    EXPECT_EQ(a.back().ask_sz, 70000u);
}

TEST_F(ExporterTest, CancelEmitsL1) {
    {
        fdexport::Exporter x(dir_);
        prime(x);
        x.on_add_order(add(7, 1, 'B', 100, 999000));
        x.on_add_order(add(7, 2, 'B', 50, 998000, kTs + 1)); // deep
        x.on_order_cancel({hdr(7, kTs + 5), 1, 30});
        x.on_order_cancel({hdr(7, kTs + 6), 2, 10}); // deep: no L1 change
        x.close();
    }
    const auto a = l1();
    ASSERT_EQ(a.size(), 2u); // deep add and deep cancel emit nothing
    expect_l1(a, 1, kTs + 5, 999000, 0, 70, 0, 7, 1, 0);
}

TEST_F(ExporterTest, PerLocateL1State) {
    {
        fdexport::Exporter x(dir_);
        prime(x, 7, "TEST");
        prime(x, 8, "OTHER");
        // Identical top-of-book on two locates: both must emit.
        x.on_add_order(add(7, 1, 'B', 100, 999000));
        x.on_add_order(add(8, 2, 'B', 100, 999000, kTs + 1));
        x.close();
    }
    const auto a = l1();
    ASSERT_EQ(a.size(), 2u);
    EXPECT_EQ(a[0].locate, 7u);
    EXPECT_EQ(a[1].locate, 8u);
    EXPECT_EQ(a[0].bid_px, a[1].bid_px);
}

TEST_F(ExporterTest, FilterSymbolPassThrough) {
    {
        fdexport::Exporter x(dir_, "TGT");
        x.on_stock_directory(directory(3, "TGT"));
        x.on_stock_directory(directory(4, "OTHER"));
        x.on_stock_trading_action(trading(3, 'T'));
        x.on_add_order(add(3, 2, 'B', 100, 999000));
        x.on_order_executed({hdr(3, kTs + 1), 2, 40, 500});
        x.on_trade(ptrade(3, 'S', 25, 998500, kTs + 2));
        x.on_cross_trade(qcross(3, 1000, 999500, kTs + 3));
        x.close();
    }
    const auto t = trades();
    ASSERT_EQ(t.size(), 3u);
    expect_trade(t, 0, kTs + 1, 999000, 40, 3, 'B', 0);
    expect_trade(t, 1, kTs + 2, 998500, 25, 3, 'S', 2);
    expect_trade(t, 2, kTs + 3, 999500, 1000, 3, 0, 3);
}

} // namespace

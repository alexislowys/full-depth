#include "book/engine.hpp"

#include <gtest/gtest.h>

#include <cstring>

namespace {

itch::Header hdr(std::uint16_t locate, std::uint64_t ts = 34'200'000'000'000ULL) {
    return {locate, 0, ts};
}

itch::AddOrder add(std::uint16_t locate, std::uint64_t ref, char side,
                   std::uint32_t shares, std::uint32_t price) {
    itch::AddOrder m{};
    m.h = hdr(locate);
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

itch::CrossTrade cross(std::uint16_t locate) {
    itch::CrossTrade m{};
    m.h = hdr(locate);
    m.cross_type = 'O';
    return m;
}

class EngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        e.on_stock_directory(directory(7, "TEST"));
        // Mirror a real day: transition into trading, then the opening
        // cross prints — after which a crossed book is a violation.
        e.on_stock_trading_action(trading(7, 'T'));
        e.on_cross_trade(cross(7));
    }
    book::Engine e;
};

TEST_F(EngineTest, AddBuildsLevels) {
    e.on_add_order(add(7, 1, 'B', 100, 999000));
    e.on_add_order(add(7, 2, 'B', 200, 999000));
    e.on_add_order(add(7, 3, 'S', 300, 1001000));

    const auto* b = e.book_for(7);
    const auto bid = b->best_bid();
    ASSERT_TRUE(bid.has_value());
    EXPECT_EQ(bid->price, 999000u);
    EXPECT_EQ(bid->level.shares, 300u);
    EXPECT_EQ(bid->level.orders, 2u);
    const auto ask = b->best_ask();
    ASSERT_TRUE(ask.has_value());
    EXPECT_EQ(ask->price, 1001000u);
    EXPECT_EQ(ask->level.shares, 300u);
    EXPECT_EQ(e.violations().total(), 0u);
}

TEST_F(EngineTest, BestBidIsHighestBestAskIsLowest) {
    e.on_add_order(add(7, 1, 'B', 100, 998000));
    e.on_add_order(add(7, 2, 'B', 100, 999000));
    e.on_add_order(add(7, 3, 'S', 100, 1002000));
    e.on_add_order(add(7, 4, 'S', 100, 1001000));

    EXPECT_EQ(e.book_for(7)->best_bid()->price, 999000u);
    EXPECT_EQ(e.book_for(7)->best_ask()->price, 1001000u);
}

TEST_F(EngineTest, PartialExecutionReducesOrderAndLevel) {
    e.on_add_order(add(7, 1, 'B', 100, 999000));
    e.on_order_executed({hdr(7), 1, 40, 500});

    const auto bid = e.book_for(7)->best_bid();
    ASSERT_TRUE(bid.has_value());
    EXPECT_EQ(bid->level.shares, 60u);
    EXPECT_EQ(bid->level.orders, 1u); // order still live
    EXPECT_EQ(e.stats().executed_shares, 40u);
    EXPECT_EQ(e.live_orders(), 1u);
}

TEST_F(EngineTest, FullExecutionRemovesOrderAndLevel) {
    e.on_add_order(add(7, 1, 'B', 100, 999000));
    e.on_order_executed({hdr(7), 1, 100, 500});

    EXPECT_FALSE(e.book_for(7)->best_bid().has_value());
    EXPECT_EQ(e.live_orders(), 0u);
    EXPECT_EQ(e.violations().total(), 0u);
}

TEST_F(EngineTest, ExecutedWithPriceImpactsDisplayPrice) {
    e.on_add_order(add(7, 1, 'S', 100, 1001000));
    // Executes at an improved price, but the book removal happens at the
    // displayed 1001000 level.
    e.on_order_executed_price({hdr(7), 1, 100, 500, 'Y', 1000500});

    EXPECT_FALSE(e.book_for(7)->best_ask().has_value());
    EXPECT_EQ(e.live_orders(), 0u);
    EXPECT_EQ(e.violations().total(), 0u);
}

TEST_F(EngineTest, PartialCancelKeepsOrder) {
    e.on_add_order(add(7, 1, 'B', 100, 999000));
    e.on_order_cancel({hdr(7), 1, 30});

    const auto bid = e.book_for(7)->best_bid();
    ASSERT_TRUE(bid.has_value());
    EXPECT_EQ(bid->level.shares, 70u);
    EXPECT_EQ(e.violations().cancel_to_zero, 0u);
    EXPECT_EQ(e.violations().total(), 0u);
}

TEST_F(EngineTest, DeleteRemovesOrder) {
    e.on_add_order(add(7, 1, 'B', 100, 999000));
    e.on_add_order(add(7, 2, 'B', 50, 999000));
    e.on_order_delete({hdr(7), 1});

    const auto bid = e.book_for(7)->best_bid();
    ASSERT_TRUE(bid.has_value());
    EXPECT_EQ(bid->level.shares, 50u);
    EXPECT_EQ(bid->level.orders, 1u);
    EXPECT_EQ(e.live_orders(), 1u);
}

TEST_F(EngineTest, ReplaceInheritsSideAndMovesPrice) {
    e.on_add_order(add(7, 1, 'B', 100, 999000));
    e.on_order_replace({hdr(7), 1, 2, 150, 998000});

    const auto* b = e.book_for(7);
    const auto bid = b->best_bid();
    ASSERT_TRUE(bid.has_value());
    EXPECT_EQ(bid->price, 998000u);
    EXPECT_EQ(bid->level.shares, 150u);
    EXPECT_EQ(e.live_orders(), 1u);
    EXPECT_EQ(e.violations().total(), 0u);

    // The old ref is dead; the new ref is live.
    e.on_order_delete({hdr(7), 1});
    EXPECT_EQ(e.violations().unknown_ref, 1u);
    e.on_order_delete({hdr(7), 2});
    EXPECT_EQ(e.live_orders(), 0u);
}

TEST_F(EngineTest, ReplaceChainSurvives) {
    e.on_add_order(add(7, 1, 'S', 100, 1001000));
    e.on_order_replace({hdr(7), 1, 2, 100, 1002000});
    e.on_order_replace({hdr(7), 2, 3, 80, 1000000});

    const auto ask = e.book_for(7)->best_ask();
    ASSERT_TRUE(ask.has_value());
    EXPECT_EQ(ask->price, 1000000u);
    EXPECT_EQ(ask->level.shares, 80u);
    EXPECT_EQ(e.stats().replaces, 2u);
    EXPECT_EQ(e.violations().total(), 0u);
}

TEST_F(EngineTest, UnknownRefsAreCountedNotFatal) {
    e.on_order_executed({hdr(7), 99, 10, 500});
    e.on_order_cancel({hdr(7), 99, 10});
    e.on_order_delete({hdr(7), 99});
    e.on_order_replace({hdr(7), 99, 100, 10, 999000});

    EXPECT_EQ(e.violations().unknown_ref, 3u);
    EXPECT_EQ(e.violations().replace_unknown, 1u);
    EXPECT_EQ(e.live_orders(), 0u);
}

TEST_F(EngineTest, DuplicateAddCounted) {
    e.on_add_order(add(7, 1, 'B', 100, 999000));
    e.on_add_order(add(7, 1, 'B', 200, 998000));

    EXPECT_EQ(e.violations().duplicate_ref, 1u);
    // Book unchanged by the rejected duplicate.
    EXPECT_EQ(e.book_for(7)->best_bid()->price, 999000u);
}

TEST_F(EngineTest, OverExecutionClampsAndCounts) {
    e.on_add_order(add(7, 1, 'B', 100, 999000));
    e.on_order_executed({hdr(7), 1, 150, 500});

    EXPECT_EQ(e.violations().over_reduction, 1u);
    EXPECT_EQ(e.stats().executed_shares, 100u); // clamped
    EXPECT_EQ(e.live_orders(), 0u);
    EXPECT_FALSE(e.book_for(7)->best_bid().has_value());
}

TEST_F(EngineTest, CrossedWhileTradingCounted) {
    e.on_add_order(add(7, 1, 'S', 100, 1000000));
    e.on_add_order(add(7, 2, 'B', 100, 1000000)); // locks the book

    EXPECT_EQ(e.violations().crossed_while_trading, 1u);
}

TEST_F(EngineTest, CrossedWhileHaltedIsLegitimate) {
    e.on_stock_trading_action(trading(7, 'H'));
    e.on_add_order(add(7, 1, 'S', 100, 1000000));
    e.on_add_order(add(7, 2, 'B', 100, 1001000)); // crossed, but halted

    EXPECT_EQ(e.violations().crossed_while_trading, 0u);
}

TEST_F(EngineTest, CrossedBeforeReopeningCrossIsLegitimate) {
    // Halt, accumulate a crossed book, resume trading: the cross stays
    // legitimate until the reopening cross prints (IPO/LULD pattern seen
    // on ANPC/BDTX/RKDA/DTSS in the sample day).
    e.on_stock_trading_action(trading(7, 'H'));
    e.on_add_order(add(7, 1, 'S', 100, 1000000));
    e.on_add_order(add(7, 2, 'B', 100, 1001000));
    e.on_stock_trading_action(trading(7, 'T'));
    e.on_add_order(add(7, 3, 'B', 50, 1002000)); // still crossed, pending

    EXPECT_EQ(e.violations().crossed_while_trading, 0u);
    EXPECT_EQ(e.violations().crossed_auction_pending, 1u);

    // Reopening cross prints; crossing is no longer excusable.
    e.on_cross_trade(cross(7));
    e.on_add_order(add(7, 4, 'B', 50, 1003000));
    EXPECT_EQ(e.violations().crossed_while_trading, 1u);
}

TEST_F(EngineTest, SymbolFilterTracksOnlyTarget) {
    book::Engine f("TGT");
    f.on_stock_directory(directory(3, "TGT"));
    f.on_stock_directory(directory(4, "OTHER"));
    f.on_stock_trading_action(trading(3, 'T'));

    f.on_add_order(add(3, 1, 'B', 100, 999000));
    f.on_add_order(add(4, 2, 'B', 100, 999000)); // not tracked

    EXPECT_EQ(f.target_locate(), 3u);
    EXPECT_EQ(f.live_orders(), 1u);
    EXPECT_TRUE(f.book_for(3)->best_bid().has_value());
    EXPECT_FALSE(f.book_for(4)->best_bid().has_value());
    EXPECT_EQ(f.stats().adds, 1u);
}

TEST_F(EngineTest, AuditMatchesAfterMixedTraffic) {
    e.on_add_order(add(7, 1, 'B', 100, 999000));
    e.on_add_order(add(7, 2, 'B', 200, 998000));
    e.on_add_order(add(7, 3, 'S', 300, 1001000));
    e.on_order_executed({hdr(7), 1, 40, 500});
    e.on_order_cancel({hdr(7), 2, 50});
    e.on_order_replace({hdr(7), 3, 4, 250, 1002000});

    EXPECT_TRUE(e.audit(7));
    EXPECT_EQ(e.violations().total(), 0u);
}

} // namespace

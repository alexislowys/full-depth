#include "book/order_store.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace {

book::OrderStore::Order order(std::uint32_t shares, std::uint32_t price) {
    return {shares, price, 1, 'B'};
}

TEST(OrderStore, InsertFindErase) {
    book::OrderStore s(16);
    EXPECT_TRUE(s.insert(42, order(100, 999000)));
    ASSERT_NE(s.find(42), nullptr);
    EXPECT_EQ(s.find(42)->shares, 100u);
    EXPECT_EQ(s.size(), 1u);

    s.erase(42);
    EXPECT_EQ(s.find(42), nullptr);
    EXPECT_EQ(s.size(), 0u);
}

TEST(OrderStore, DuplicateInsertRejected) {
    book::OrderStore s(16);
    EXPECT_TRUE(s.insert(7, order(100, 1)));
    EXPECT_FALSE(s.insert(7, order(200, 2)));
    EXPECT_EQ(s.find(7)->shares, 100u); // original untouched
    EXPECT_EQ(s.size(), 1u);
}

TEST(OrderStore, EraseAbsentIsNoop) {
    book::OrderStore s(16);
    s.insert(1, order(10, 1));
    s.erase(999);
    EXPECT_EQ(s.size(), 1u);
    EXPECT_NE(s.find(1), nullptr);
}

TEST(OrderStore, RefZeroIsAValidKey) {
    book::OrderStore s(16);
    EXPECT_TRUE(s.insert(0, order(50, 123)));
    ASSERT_NE(s.find(0), nullptr);
    EXPECT_EQ(s.find(0)->shares, 50u);
    s.erase(0);
    EXPECT_EQ(s.find(0), nullptr);
}

TEST(OrderStore, BackshiftPreservesCollisionChains) {
    // Small table so sequential keys collide after masking; delete from
    // the middle of probe chains and verify every survivor stays
    // reachable.
    book::OrderStore s(16);
    std::vector<std::uint64_t> keys;
    for (std::uint64_t k = 1; k <= 12; ++k) {
        ASSERT_TRUE(s.insert(k, order(static_cast<std::uint32_t>(k), 1)));
        keys.push_back(k);
    }
    // Delete every third key.
    for (std::size_t i = 0; i < keys.size(); i += 3) s.erase(keys[i]);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i % 3 == 0) {
            EXPECT_EQ(s.find(keys[i]), nullptr) << "key " << keys[i];
        } else {
            ASSERT_NE(s.find(keys[i]), nullptr) << "key " << keys[i];
            EXPECT_EQ(s.find(keys[i])->shares, keys[i]);
        }
    }
}

TEST(OrderStore, GrowthRehashesEverything) {
    book::OrderStore s(16); // forces several grows
    for (std::uint64_t k = 1; k <= 1000; ++k)
        ASSERT_TRUE(s.insert(k, order(static_cast<std::uint32_t>(k), 1)));
    EXPECT_EQ(s.size(), 1000u);
    for (std::uint64_t k = 1; k <= 1000; ++k) {
        ASSERT_NE(s.find(k), nullptr) << "key " << k;
        EXPECT_EQ(s.find(k)->shares, k);
    }
}

TEST(OrderStore, ChurnMatchesReferenceModel) {
    // Randomized insert/erase churn cross-checked against
    // std::unordered_map as the oracle.
    book::OrderStore s(64);
    std::unordered_map<std::uint64_t, std::uint32_t> ref;
    std::uint64_t rng = 0x123456789ULL;
    auto next = [&rng] {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };
    for (int i = 0; i < 20000; ++i) {
        const std::uint64_t key = next() % 500;
        if (next() % 2 == 0) {
            const auto shares = static_cast<std::uint32_t>(next() % 10000);
            const bool inserted = s.insert(key, order(shares, 1));
            const bool expected = ref.emplace(key, shares).second;
            ASSERT_EQ(inserted, expected) << "key " << key;
        } else {
            s.erase(key);
            ref.erase(key);
        }
    }
    ASSERT_EQ(s.size(), ref.size());
    for (const auto& [k, shares] : ref) {
        ASSERT_NE(s.find(k), nullptr) << "key " << k;
        EXPECT_EQ(s.find(k)->shares, shares);
    }
    std::uint64_t visited = 0;
    s.for_each([&](std::uint64_t k, const book::OrderStore::Order& o) {
        ++visited;
        auto it = ref.find(k);
        ASSERT_NE(it, ref.end());
        EXPECT_EQ(o.shares, it->second);
    });
    EXPECT_EQ(visited, ref.size());
}

} // namespace

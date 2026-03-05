#include <gtest/gtest.h>

#include <limits>
#include <nlohmann/json.hpp>

#include "order_book.hpp"

namespace {

nlohmann::json makeSnapshot(long long lastUpdateId,
                            const std::vector<std::pair<std::string, std::string>>& asks,
                            const std::vector<std::pair<std::string, std::string>>& bids) {
    nlohmann::json snap;
    snap["lastUpdateId"] = lastUpdateId;
    snap["asks"] = nlohmann::json::array();
    for (const auto& [p, q] : asks) snap["asks"].push_back({p, q});
    snap["bids"] = nlohmann::json::array();
    for (const auto& [p, q] : bids) snap["bids"].push_back({p, q});
    return snap;
}

nlohmann::json makeUpdate(long long U, long long u,
                          const std::vector<std::pair<std::string, std::string>>& asks,
                          const std::vector<std::pair<std::string, std::string>>& bids) {
    nlohmann::json upd;
    upd["U"] = U;
    upd["u"] = u;
    upd["a"] = nlohmann::json::array();
    for (const auto& [p, q] : asks) upd["a"].push_back({p, q});
    upd["b"] = nlohmann::json::array();
    for (const auto& [p, q] : bids) upd["b"].push_back({p, q});
    return upd;
}

}  // namespace

// ─── Snapshot ────────────────────────────────────────────────────────────────

TEST(OrderBook, SnapshotPopulatesBookAndSetsState) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.5"}, {"50002.00", "0.5"}},
                                    {{"49999.00", "2.0"}, {"49998.00", "1.0"}}));

    EXPECT_TRUE(book.isSnapshotApplied());
    EXPECT_EQ(book.getLastUpdateId(), 100);
    EXPECT_EQ(book.getAsks().size(), 2u);
    EXPECT_EQ(book.getBids().size(), 2u);
}

TEST(OrderBook, SnapshotClearsPreviousLevels) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}}, {{"49999.00", "1.0"}}));
    book.applySnapshot(makeSnapshot(200, {{"60000.00", "2.0"}}, {}));

    auto asks = book.getAsks();
    EXPECT_EQ(asks.size(), 1u);
    EXPECT_EQ(book.getLastUpdateId(), 200);
    // Only 60000.00 should remain from second snapshot
    EXPECT_EQ(asks.begin()->first, 6000000000000LL);  // 60000.00 * 1e8
}

// ─── Update: stale / gap / valid ─────────────────────────────────────────────

TEST(OrderBook, StaleUpdateIsIgnoredAndReturnsTrue) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}}, {}));

    // u=100 <= lastUpdateId=100 → stale
    EXPECT_TRUE(book.applyUpdate(makeUpdate(99, 100, {{"50002.00", "0.5"}}, {})));
    EXPECT_EQ(book.getLastUpdateId(), 100);  // unchanged
    EXPECT_EQ(book.getAsks().size(), 1u);    // no new level added
}

TEST(OrderBook, GapInUpdateIdsReturnsFalse) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}}, {}));

    // U=102 > lastUpdateId+1=101 → gap
    EXPECT_FALSE(book.applyUpdate(makeUpdate(102, 103, {}, {})));
}

TEST(OrderBook, ValidConsecutiveUpdateAdvancesLastUpdateId) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}}, {}));

    EXPECT_TRUE(book.applyUpdate(makeUpdate(101, 105, {}, {})));
    EXPECT_EQ(book.getLastUpdateId(), 105);
}

// ─── Update: level manipulation ──────────────────────────────────────────────

TEST(OrderBook, UpdateAddsNewPriceLevel) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}}, {}));

    EXPECT_TRUE(book.applyUpdate(makeUpdate(101, 101, {{"50002.00", "0.5"}}, {})));

    EXPECT_EQ(book.getAsks().size(), 2u);
}

TEST(OrderBook, UpdateModifiesExistingLevel) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}}, {}));

    EXPECT_TRUE(book.applyUpdate(makeUpdate(101, 101, {{"50001.00", "3.0"}}, {})));

    auto asks = book.getAsks();
    // price key: 50001.00 * 1e8 = 5000100000000; qty: 3.0 * 1e8 = 300000000
    EXPECT_EQ(asks.at(5000100000000LL), 300000000LL);
}

TEST(OrderBook, UpdateWithZeroQuantityRemovesLevel) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}, {"50002.00", "0.5"}}, {}));

    EXPECT_TRUE(book.applyUpdate(makeUpdate(101, 101, {{"50001.00", "0.0"}}, {})));

    auto asks = book.getAsks();
    EXPECT_EQ(asks.size(), 1u);
    EXPECT_EQ(asks.begin()->first, 5000200000000LL);  // 50002.00 * 1e8
}

TEST(OrderBook, UpdateRemovesBidLevelWithZeroQuantity) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {}, {{"49999.00", "2.0"}, {"49998.00", "1.0"}}));

    EXPECT_TRUE(book.applyUpdate(makeUpdate(101, 101, {}, {{"49999.00", "0.0"}})));

    auto bids = book.getBids();
    EXPECT_EQ(bids.size(), 1u);
    EXPECT_EQ(bids.begin()->first, 4999800000000LL);  // 49998.00 * 1e8
}

// ─── Clear ───────────────────────────────────────────────────────────────────

TEST(OrderBook, ClearResetsAllState) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}}, {{"49999.00", "1.0"}}));
    book.clear();

    EXPECT_FALSE(book.isSnapshotApplied());
    EXPECT_EQ(book.getLastUpdateId(), 0);
    EXPECT_TRUE(book.getAsks().empty());
    EXPECT_TRUE(book.getBids().empty());
}

// ─── Stats ───────────────────────────────────────────────────────────────────

TEST(OrderBook, BestAskIsLowestAskPrice) {
    OrderBook book;
    // Insert asks out of order; best ask must be lowest
    book.applySnapshot(
        makeSnapshot(100, {{"50002.00", "1.0"}, {"50001.00", "0.5"}}, {{"49999.00", "2.0"}}));

    auto stats = book.getStats();
    EXPECT_NEAR(stats.bestAsk, 50001.0, 1e-6);
}

TEST(OrderBook, BestBidIsHighestBidPrice) {
    OrderBook book;
    book.applySnapshot(
        makeSnapshot(100, {{"50001.00", "1.0"}}, {{"49998.00", "1.0"}, {"49999.00", "2.0"}}));

    auto stats = book.getStats();
    EXPECT_NEAR(stats.bestBid, 49999.0, 1e-6);
}

TEST(OrderBook, StatsCountsReflectCurrentBookSize) {
    OrderBook book;
    book.applySnapshot(
        makeSnapshot(100, {{"50001.00", "1.0"}, {"50002.00", "0.5"}}, {{"49999.00", "2.0"}}));

    auto stats = book.getStats();
    EXPECT_EQ(stats.asksCount, 2u);
    EXPECT_EQ(stats.bidsCount, 1u);
}

TEST(OrderBook, EmptyBookStatsAreZero) {
    OrderBook book;
    auto stats = book.getStats();

    EXPECT_EQ(stats.bestAsk, 0.0);
    EXPECT_EQ(stats.bestBid, 0.0);
    EXPECT_EQ(stats.asksCount, 0u);
    EXPECT_EQ(stats.bidsCount, 0u);
}

// ─── Ordering guarantees ─────────────────────────────────────────────────────

TEST(OrderBook, AsksAreSortedAscendingByPrice) {
    OrderBook book;
    book.applySnapshot(
        makeSnapshot(100, {{"50003.00", "1.0"}, {"50001.00", "0.5"}, {"50002.00", "2.0"}}, {}));

    long long prev = 0;
    for (auto& [price, qty] : book.getAsks()) {
        EXPECT_GT(price, prev);
        prev = price;
    }
}

TEST(OrderBook, BidsAreSortedDescendingByPrice) {
    OrderBook book;
    book.applySnapshot(
        makeSnapshot(100, {}, {{"49997.00", "1.0"}, {"49999.00", "0.5"}, {"49998.00", "2.0"}}));

    long long prev = std::numeric_limits<long long>::max();
    for (auto& [price, qty] : book.getBids()) {
        EXPECT_LT(price, prev);
        prev = price;
    }
}

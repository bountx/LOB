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
    auto stats = book.getStats();
    EXPECT_EQ(stats.asksCount, 2u);
    EXPECT_EQ(stats.bidsCount, 2u);
}

TEST(OrderBook, SnapshotClearsPreviousLevels) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}}, {{"49999.00", "1.0"}}));
    book.applySnapshot(makeSnapshot(200, {{"60000.00", "2.0"}}, {}));

    auto snap = book.getSnapshot();
    EXPECT_EQ(snap.asks.size(), 1u);
    EXPECT_EQ(book.getLastUpdateId(), 200);
    // Only 60000.00 should remain from second snapshot
    EXPECT_EQ(snap.asks.front().first, 6000000000000LL);  // 60000.00 * 1e8
}

// ─── Update: stale / gap / valid ─────────────────────────────────────────────

TEST(OrderBook, StaleUpdateIsIgnoredAndReturnsTrue) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}}, {}));

    // u=100 <= lastUpdateId=100 → stale
    auto result =
        book.applyUpdate(makeUpdate(99, 100, {{"50002.00", "0.5"}}, {}), EventKind::Genuine);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(book.getLastUpdateId(), 100);  // unchanged
    auto stats = book.getStats();
    EXPECT_EQ(stats.asksCount, 1u);  // no new level added
}

TEST(OrderBook, GapInUpdateIdsReturnsFalse) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}}, {}));

    // U=102 > lastUpdateId+1=101 → gap
    auto result = book.applyUpdate(makeUpdate(102, 103, {}, {}), EventKind::Genuine);
    EXPECT_FALSE(result.success);
}

TEST(OrderBook, ValidConsecutiveUpdateAdvancesLastUpdateId) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}}, {}));

    auto result = book.applyUpdate(makeUpdate(101, 105, {}, {}), EventKind::Genuine);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(book.getLastUpdateId(), 105);
}

// ─── Update: level manipulation ──────────────────────────────────────────────

TEST(OrderBook, UpdateAddsNewPriceLevel) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}}, {}));

    auto result =
        book.applyUpdate(makeUpdate(101, 101, {{"50002.00", "0.5"}}, {}), EventKind::Genuine);
    EXPECT_TRUE(result.success);

    auto stats = book.getStats();
    EXPECT_EQ(stats.asksCount, 2u);
}

TEST(OrderBook, UpdateModifiesExistingLevel) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}}, {}));

    auto result =
        book.applyUpdate(makeUpdate(101, 101, {{"50001.00", "3.0"}}, {}), EventKind::Genuine);
    EXPECT_TRUE(result.success);

    // Verify delta reports the modified level
    EXPECT_EQ(result.deltas.size(), 1u);
    EXPECT_EQ(result.deltas[0].price, 5000100000000LL);  // 50001.00 * 1e8
    EXPECT_EQ(result.deltas[0].newQty, 300000000LL);     // 3.0 * 1e8
}

TEST(OrderBook, UpdateWithZeroQuantityRemovesLevel) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}, {"50002.00", "0.5"}}, {}));

    auto result =
        book.applyUpdate(makeUpdate(101, 101, {{"50001.00", "0.0"}}, {}), EventKind::Genuine);
    EXPECT_TRUE(result.success);

    auto snap = book.getSnapshot();
    EXPECT_EQ(snap.asks.size(), 1u);
    EXPECT_EQ(snap.asks.front().first, 5000200000000LL);  // 50002.00 * 1e8
}

TEST(OrderBook, UpdateRemovesBidLevelWithZeroQuantity) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {}, {{"49999.00", "2.0"}, {"49998.00", "1.0"}}));

    auto result =
        book.applyUpdate(makeUpdate(101, 101, {}, {{"49999.00", "0.0"}}), EventKind::Genuine);
    EXPECT_TRUE(result.success);

    auto snap = book.getSnapshot();
    EXPECT_EQ(snap.bids.size(), 1u);
    EXPECT_EQ(snap.bids.front().first, 4999800000000LL);  // 49998.00 * 1e8
}

// ─── Clear ───────────────────────────────────────────────────────────────────

TEST(OrderBook, ClearResetsAllState) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}}, {{"49999.00", "1.0"}}));
    book.clear();

    EXPECT_FALSE(book.isSnapshotApplied());
    EXPECT_EQ(book.getLastUpdateId(), 0);
    auto snap = book.getSnapshot();
    EXPECT_TRUE(snap.asks.empty());
    EXPECT_TRUE(snap.bids.empty());
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

    auto snap = book.getSnapshot();
    long long prev = 0;
    for (const auto& [price, qty] : snap.asks) {
        EXPECT_GT(price, prev);
        prev = price;
    }
}

TEST(OrderBook, BidsAreSortedDescendingByPrice) {
    OrderBook book;
    book.applySnapshot(
        makeSnapshot(100, {}, {{"49997.00", "1.0"}, {"49999.00", "0.5"}, {"49998.00", "2.0"}}));

    auto snap = book.getSnapshot();
    long long prev = std::numeric_limits<long long>::max();
    for (const auto& [price, qty] : snap.bids) {
        EXPECT_LT(price, prev);
        prev = price;
    }
}

// ─── OFI view ────────────────────────────────────────────────────────────────

TEST(OrderBook, DeltaInOfiViewFlagSetForTopLevels) {
    // Default ofiDepth=10; with 3 ask levels all should be in view
    OrderBook book;
    book.applySnapshot(
        makeSnapshot(100, {{"50001.00", "1.0"}, {"50002.00", "0.5"}, {"50003.00", "2.0"}}, {}));

    // Modify a level that is within the top-10 OFI view
    auto result =
        book.applyUpdate(makeUpdate(101, 101, {{"50001.00", "2.0"}}, {}), EventKind::Genuine);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.deltas.size(), 1u);
    EXPECT_TRUE(result.deltas[0].inOfiView);
}

TEST(OrderBook, EventKindBackfillPreservedInDelta) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}}, {}));

    auto result =
        book.applyUpdate(makeUpdate(101, 101, {{"50002.00", "0.5"}}, {}), EventKind::Backfill);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.deltas.size(), 1u);
    EXPECT_EQ(result.deltas[0].kind, EventKind::Backfill);
}

TEST(OrderBook, BackfillOfiViewCrossingProducesBackfillKindOnAllDeltas) {
    // ofiDepth=2 with 3 ask levels so position 3 (50003) sits outside the view.
    // A Backfill removal of the best ask triggers a view rebuild (replacement enters).
    // Both the direct removal delta and the secondary replacement delta must carry
    // kind == EventKind::Backfill, not EventKind::Maintenance.
    OrderBook book(2);
    book.applySnapshot(
        makeSnapshot(100, {{"50001.00", "1.0"}, {"50002.00", "1.0"}, {"50003.00", "1.0"}}, {}));

    auto result =
        book.applyUpdate(makeUpdate(101, 101, {{"50001.00", "0.0"}}, {}), EventKind::Backfill);
    EXPECT_TRUE(result.success);

    // Direct removal delta + replacement maintenance delta.
    ASSERT_EQ(result.deltas.size(), 2u);
    for (const auto& d : result.deltas) {
        EXPECT_EQ(d.kind, EventKind::Backfill);
    }

    // Verify the delta semantics are still correct.
    EXPECT_EQ(result.deltas[0].newQty, 0LL);    // removal: level gone from state
    EXPECT_LT(result.deltas[0].deltaQty, 0LL);  // removal: negative state delta
    EXPECT_GT(result.deltas[1].deltaQty, 0LL);  // replacement: positive view-contribution delta
}

TEST(OrderBook, ApplyDeltaReturnsDeltasWithGenuineKind) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(0, {{"50001.00", "1.0"}}, {{"49999.00", "1.0"}}));

    nlohmann::json bids = nlohmann::json::array();
    bids.push_back({"49999.00", "2.0"});
    nlohmann::json asks = nlohmann::json::array();
    asks.push_back({"50001.00", "0.0"});  // remove

    auto deltas = book.applyDelta(bids, asks, EventKind::Genuine);
    EXPECT_EQ(deltas.size(), 2u);
    for (const auto& d : deltas) {
        EXPECT_EQ(d.kind, EventKind::Genuine);
    }
}

/**
 * @brief Verifies that an ask level inserted deeper than the configured OFI depth is not considered
 * in the OFI view.
 *
 * Sets OFI depth to 2, applies a snapshot with two top ask levels, then inserts a third ask deeper
 * than position 2. Expects the update to succeed, exactly one delta to be produced, and that
 * delta's `inOfiView` flag to be `false`.
 */

TEST(OrderBook, LevelBeyondOfiDepthNotInView) {
    // ofiDepth=2: only top-2 ask levels are tracked in the OFI view.
    // A new ask level inserted deeper than position 2 must have inOfiView=false.
    OrderBook book(2);
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}, {"50002.00", "1.0"}}, {}));

    // 50003 is beyond the top-2 asks — should not be in view.
    auto result =
        book.applyUpdate(makeUpdate(101, 101, {{"50003.00", "0.5"}}, {}), EventKind::Genuine);
    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.deltas.size(), 1u);
    EXPECT_FALSE(result.deltas[0].inOfiView);
}

TEST(OrderBook, LevelWithinOfiDepthIsInView) {
    // ofiDepth=2: a new ask that beats the current worst ask in the view enters the view.
    // Because the view is full, the worst level (50003) is evicted and emits a Maintenance delta.
    OrderBook book(2);
    book.applySnapshot(makeSnapshot(100, {{"50002.00", "1.0"}, {"50003.00", "1.0"}}, {}));

    // 50001 beats 50002 (current best ask) — must enter the top-2 view, evicting 50003.
    auto result =
        book.applyUpdate(makeUpdate(101, 101, {{"50001.00", "0.5"}}, {}), EventKind::Genuine);
    EXPECT_TRUE(result.success);

    // Direct delta: 50001 enters the view.
    ASSERT_GE(result.deltas.size(), 1u);
    EXPECT_EQ(result.deltas[0].kind, EventKind::Genuine);
    EXPECT_TRUE(result.deltas[0].inOfiView);

    // Maintenance delta: 50003 is evicted from the now-full view.
    ASSERT_EQ(result.deltas.size(), 2u);
    EXPECT_EQ(result.deltas[1].kind, EventKind::Maintenance);
    EXPECT_TRUE(result.deltas[1].wasInView);
    EXPECT_FALSE(result.deltas[1].inOfiView);
    EXPECT_LT(result.deltas[1].deltaQty, 0LL);  // negative: view qty dropped
}

// ─── deltaQty semantics ────────────────────────────────────────────────────────

TEST(OrderBook, DeltaQtyIsPositiveOnQtyIncrease) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}}, {}));

    // 1.0 → 3.0: delta = +2.0 * 1e8
    auto result =
        book.applyUpdate(makeUpdate(101, 101, {{"50001.00", "3.0"}}, {}), EventKind::Genuine);
    ASSERT_EQ(result.deltas.size(), 1u);
    EXPECT_EQ(result.deltas[0].deltaQty, 200000000LL);
    EXPECT_EQ(result.deltas[0].newQty, 300000000LL);
}

TEST(OrderBook, DeltaQtyIsNegativeOnQtyDecrease) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "2.0"}}, {}));

    // 2.0 → 0.5: delta = -1.5 * 1e8
    auto result =
        book.applyUpdate(makeUpdate(101, 101, {{"50001.00", "0.5"}}, {}), EventKind::Genuine);
    ASSERT_EQ(result.deltas.size(), 1u);
    EXPECT_EQ(result.deltas[0].deltaQty, -150000000LL);
}

TEST(OrderBook, RemovedLevelHasNewQtyZeroAndNegativeDelta) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.5"}}, {}));

    auto result =
        book.applyUpdate(makeUpdate(101, 101, {{"50001.00", "0.0"}}, {}), EventKind::Genuine);
    ASSERT_EQ(result.deltas.size(), 1u);
    EXPECT_EQ(result.deltas[0].newQty, 0LL);
    EXPECT_EQ(result.deltas[0].deltaQty, -150000000LL);  // 0 - 1.5*1e8
}

// ─── OFI view refill after removal ───────────────────────────────────────────

TEST(OrderBook, OfiViewRefillsWhenLevelRemovedFromView) {
    // ofiDepth=2 with 3 ask levels. Remove the best ask; position 3 should slide into view.
    OrderBook book(2);
    book.applySnapshot(
        makeSnapshot(100, {{"50001.00", "1.0"}, {"50002.00", "1.0"}, {"50003.00", "1.0"}}, {}));

    // Remove best ask (50001). State still has 50002 and 50003, both should now be in view.
    auto result =
        book.applyUpdate(makeUpdate(101, 101, {{"50001.00", "0.0"}}, {}), EventKind::Genuine);
    EXPECT_TRUE(result.success);

    // getStats() reads from the OFI view — best ask must now be 50002.
    auto stats = book.getStats();
    EXPECT_NEAR(stats.bestAsk, 50002.0, 1e-6);

    // The snapshot must contain both remaining levels.
    auto snap = book.getSnapshot();
    EXPECT_EQ(snap.asks.size(), 2u);

    // Direct delta: removal of 50001 (Genuine, was in view, now gone).
    ASSERT_GE(result.deltas.size(), 1u);
    EXPECT_EQ(result.deltas[0].kind, EventKind::Genuine);
    EXPECT_TRUE(result.deltas[0].wasInView);
    EXPECT_FALSE(result.deltas[0].inOfiView);
    EXPECT_EQ(result.deltas[0].newQty, 0LL);

    // Maintenance delta: 50003 slides into the view as a replacement.
    ASSERT_EQ(result.deltas.size(), 2u);
    EXPECT_EQ(result.deltas[1].kind, EventKind::Maintenance);
    EXPECT_FALSE(result.deltas[1].wasInView);
    EXPECT_TRUE(result.deltas[1].inOfiView);
    EXPECT_GT(result.deltas[1].deltaQty, 0LL);  // positive: view qty grew
}

// ─── OFI formula ──────────────────────────────────────────────────────────────

TEST(OrderBook, OfiComputationBidMinusAsk) {
    // Verify that a consumer can compute OFI = sum(bid deltas) - sum(ask deltas)
    // for Genuine deltas that touched the OFI view (wasInView || inOfiView).
    // In this case both levels remain in the view so inOfiView alone is sufficient.
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}}, {{"49999.00", "1.0"}}));

    // Bid increases by 0.5, ask decreases by 0.3
    nlohmann::json bids = nlohmann::json::array();
    bids.push_back({"49999.00", "1.5"});  // +0.5
    nlohmann::json asks = nlohmann::json::array();
    asks.push_back({"50001.00", "0.7"});  // -0.3

    auto deltas = book.applyDelta(bids, asks, EventKind::Genuine);

    long long ofi = 0;
    for (const auto& d : deltas) {
        // Exclude Backfill; include both Genuine and Maintenance (view evictions/replacements).
        if (d.kind != EventKind::Backfill && (d.wasInView || d.inOfiView)) {
            ofi += d.isBid ? d.deltaQty : -d.deltaQty;
        }
    }
    // bid delta = +50000000 (0.5 * 1e8); ask delta applied negated = +30000000 (0.3 * 1e8)
    // OFI = 50000000 + 30000000 = 80000000
    EXPECT_EQ(ofi, 80000000LL);
}

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

// ─── OFI view bid-side eviction and refill ────────────────────────────────────

TEST(OrderBook, OfiViewBidEvictionWhenViewFull) {
    // ofiDepth=2: a new bid that beats the current worst bid in the view enters the view.
    // ofiBids sorted descending: [49999, 49998]; worst bid in view = 49998.
    // A new bid at 50000 beats 49998, enters the view, evicts 49998.
    OrderBook book(2);
    book.applySnapshot(makeSnapshot(100, {}, {{"49999.00", "1.0"}, {"49998.00", "0.8"}}));

    auto result =
        book.applyUpdate(makeUpdate(101, 101, {}, {{"50000.00", "0.5"}}), EventKind::Genuine);
    EXPECT_TRUE(result.success);

    // Direct delta: 50000 enters the view.
    ASSERT_GE(result.deltas.size(), 1u);
    EXPECT_EQ(result.deltas[0].kind, EventKind::Genuine);
    EXPECT_TRUE(result.deltas[0].inOfiView);
    EXPECT_EQ(result.deltas[0].price, 5000000000000LL);  // 50000.00 * 1e8

    // Maintenance delta: 49998 is evicted from the now-full view.
    ASSERT_EQ(result.deltas.size(), 2u);
    EXPECT_EQ(result.deltas[1].kind, EventKind::Maintenance);
    EXPECT_EQ(result.deltas[1].price, 4999800000000LL);  // 49998.00 * 1e8
    EXPECT_TRUE(result.deltas[1].wasInView);
    EXPECT_FALSE(result.deltas[1].inOfiView);
    EXPECT_LT(result.deltas[1].deltaQty, 0LL);  // negative: view qty dropped to 0
}

TEST(OrderBook, OfiViewBidRefillsWhenLevelRemovedFromView) {
    // ofiDepth=2 with 3 bid levels. Remove the best bid; position 3 should slide into view.
    // ofiBids view: [50000, 49999]; 49998 is in state but outside the view.
    OrderBook book(2);
    book.applySnapshot(
        makeSnapshot(100, {}, {{"50000.00", "1.0"}, {"49999.00", "1.0"}, {"49998.00", "1.0"}}));

    // Remove best bid (50000). State still has 49999 and 49998, both should now be in view.
    auto result =
        book.applyUpdate(makeUpdate(101, 101, {}, {{"50000.00", "0.0"}}), EventKind::Genuine);
    EXPECT_TRUE(result.success);

    // getStats() best bid must now be 49999.
    auto stats = book.getStats();
    EXPECT_NEAR(stats.bestBid, 49999.0, 1e-6);

    // The snapshot must contain both remaining levels.
    auto snap = book.getSnapshot();
    EXPECT_EQ(snap.bids.size(), 2u);

    // Direct delta: removal of 50000 (Genuine, was in view, now gone).
    ASSERT_GE(result.deltas.size(), 1u);
    EXPECT_EQ(result.deltas[0].kind, EventKind::Genuine);
    EXPECT_TRUE(result.deltas[0].wasInView);
    EXPECT_FALSE(result.deltas[0].inOfiView);
    EXPECT_EQ(result.deltas[0].newQty, 0LL);

    // Maintenance delta: 49998 slides into the view as a replacement.
    ASSERT_EQ(result.deltas.size(), 2u);
    EXPECT_EQ(result.deltas[1].kind, EventKind::Maintenance);
    EXPECT_EQ(result.deltas[1].price, 4999800000000LL);  // 49998.00 * 1e8
    EXPECT_FALSE(result.deltas[1].wasInView);
    EXPECT_TRUE(result.deltas[1].inOfiView);
    EXPECT_GT(result.deltas[1].deltaQty, 0LL);  // positive: view qty grew from 0
}

// ─── OFI replacement scan: delete best / middle / worst / empty-view ─────────
//
// These tests exercise the targeted O(N) replacement scan introduced in
// updateOfiView to replace the old rebuildOfiSide() call. Each test removes an
// in-view level and asserts that (a) the view remains correctly sorted, (b) the
// next-outside level is picked as the replacement, and (c) the Maintenance delta
// describes the replacement entry accurately.

// --- Ask-side ---

TEST(OrderBook, OfiReplacementScanAskDeleteBest) {
    // ofiDepth=3, 4 ask levels. View=[50001, 50002, 50003], 50004 outside.
    // Delete best ask (50001); 50004 should slide in at the back.
    OrderBook book(3);
    book.applySnapshot(makeSnapshot(
        100, {{"50001.00", "1.0"}, {"50002.00", "1.0"}, {"50003.00", "1.0"}, {"50004.00", "1.0"}},
        {}));

    auto result =
        book.applyUpdate(makeUpdate(101, 101, {{"50001.00", "0.0"}}, {}), EventKind::Genuine);
    EXPECT_TRUE(result.success);

    // View must be ordered ascending: 50002 < 50003 < 50004.
    auto stats = book.getStats();
    EXPECT_NEAR(stats.bestAsk, 50002.0, 1e-6);

    // Maintenance delta: 50004 enters the view.
    ASSERT_EQ(result.deltas.size(), 2u);
    EXPECT_EQ(result.deltas[1].kind, EventKind::Maintenance);
    EXPECT_EQ(result.deltas[1].price, 5000400000000LL);  // 50004 * 1e8
    EXPECT_FALSE(result.deltas[1].wasInView);
    EXPECT_TRUE(result.deltas[1].inOfiView);
    EXPECT_GT(result.deltas[1].deltaQty, 0LL);
}

TEST(OrderBook, OfiReplacementScanAskDeleteMiddle) {
    // ofiDepth=3, 4 ask levels. View=[50001, 50002, 50003], 50004 outside.
    // Delete middle ask (50002); 50004 should slide in at the back.
    OrderBook book(3);
    book.applySnapshot(makeSnapshot(
        100, {{"50001.00", "1.0"}, {"50002.00", "1.0"}, {"50003.00", "1.0"}, {"50004.00", "1.0"}},
        {}));

    auto result =
        book.applyUpdate(makeUpdate(101, 101, {{"50002.00", "0.0"}}, {}), EventKind::Genuine);
    EXPECT_TRUE(result.success);

    // View must be ordered ascending: 50001 < 50003 < 50004.
    auto stats = book.getStats();
    EXPECT_NEAR(stats.bestAsk, 50001.0, 1e-6);

    ASSERT_EQ(result.deltas.size(), 2u);
    EXPECT_EQ(result.deltas[1].price, 5000400000000LL);  // 50004 * 1e8
    EXPECT_EQ(result.deltas[1].kind, EventKind::Maintenance);
    EXPECT_TRUE(result.deltas[1].inOfiView);
    EXPECT_GT(result.deltas[1].deltaQty, 0LL);
}

TEST(OrderBook, OfiReplacementScanAskDeleteWorst) {
    // ofiDepth=3, 4 ask levels. View=[50001, 50002, 50003], 50004 outside.
    // Delete worst-in-view ask (50003); 50004 should slide in at the back.
    OrderBook book(3);
    book.applySnapshot(makeSnapshot(
        100, {{"50001.00", "1.0"}, {"50002.00", "1.0"}, {"50003.00", "1.0"}, {"50004.00", "1.0"}},
        {}));

    auto result =
        book.applyUpdate(makeUpdate(101, 101, {{"50003.00", "0.0"}}, {}), EventKind::Genuine);
    EXPECT_TRUE(result.success);

    // View must be ordered ascending: 50001 < 50002 < 50004.
    auto stats = book.getStats();
    EXPECT_NEAR(stats.bestAsk, 50001.0, 1e-6);

    ASSERT_EQ(result.deltas.size(), 2u);
    EXPECT_EQ(result.deltas[1].price, 5000400000000LL);  // 50004 * 1e8
    EXPECT_EQ(result.deltas[1].kind, EventKind::Maintenance);
    EXPECT_TRUE(result.deltas[1].inOfiView);
    EXPECT_GT(result.deltas[1].deltaQty, 0LL);
}

TEST(OrderBook, OfiReplacementScanAskNoReplacementWhenNothingOutside) {
    // ofiDepth=2, exactly 2 ask levels — both in view, nothing outside.
    // Deleting one shrinks the view to 1 with no Maintenance delta.
    OrderBook book(2);
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}, {"50002.00", "1.0"}}, {}));

    auto result =
        book.applyUpdate(makeUpdate(101, 101, {{"50001.00", "0.0"}}, {}), EventKind::Genuine);
    EXPECT_TRUE(result.success);

    // Only the Genuine removal delta — no Maintenance delta.
    ASSERT_EQ(result.deltas.size(), 1u);
    EXPECT_EQ(result.deltas[0].kind, EventKind::Genuine);

    auto stats = book.getStats();
    EXPECT_NEAR(stats.bestAsk, 50002.0, 1e-6);
}

TEST(OrderBook, OfiReplacementScanAskEmptyViewGetsRefilled) {
    // ofiDepth=1: deleting the only in-view ask triggers the threshold=0 path
    // (any remaining ask is eligible). The next ask should fill the view.
    OrderBook book(1);
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.0"}, {"50002.00", "1.0"}}, {}));

    auto result =
        book.applyUpdate(makeUpdate(101, 101, {{"50001.00", "0.0"}}, {}), EventKind::Genuine);
    EXPECT_TRUE(result.success);

    auto stats = book.getStats();
    EXPECT_NEAR(stats.bestAsk, 50002.0, 1e-6);

    // Maintenance delta for 50002 entering the view.
    ASSERT_EQ(result.deltas.size(), 2u);
    EXPECT_EQ(result.deltas[1].kind, EventKind::Maintenance);
    EXPECT_EQ(result.deltas[1].price, 5000200000000LL);  // 50002 * 1e8
    EXPECT_TRUE(result.deltas[1].inOfiView);
}

// --- Bid-side ---

TEST(OrderBook, OfiReplacementScanBidDeleteBest) {
    // ofiDepth=3, 4 bid levels. View=[50000, 49999, 49998], 49997 outside.
    // Delete best bid (50000); 49997 should slide in at the back.
    OrderBook book(3);
    book.applySnapshot(makeSnapshot(
        100, {},
        {{"50000.00", "1.0"}, {"49999.00", "1.0"}, {"49998.00", "1.0"}, {"49997.00", "1.0"}}));

    auto result =
        book.applyUpdate(makeUpdate(101, 101, {}, {{"50000.00", "0.0"}}), EventKind::Genuine);
    EXPECT_TRUE(result.success);

    // View must be ordered descending: 49999 > 49998 > 49997.
    auto stats = book.getStats();
    EXPECT_NEAR(stats.bestBid, 49999.0, 1e-6);

    ASSERT_EQ(result.deltas.size(), 2u);
    EXPECT_EQ(result.deltas[1].kind, EventKind::Maintenance);
    EXPECT_EQ(result.deltas[1].price, 4999700000000LL);  // 49997 * 1e8
    EXPECT_FALSE(result.deltas[1].wasInView);
    EXPECT_TRUE(result.deltas[1].inOfiView);
    EXPECT_GT(result.deltas[1].deltaQty, 0LL);
}

TEST(OrderBook, OfiReplacementScanBidDeleteMiddle) {
    // ofiDepth=3, 4 bid levels. View=[50000, 49999, 49998], 49997 outside.
    // Delete middle bid (49999); 49997 should slide in at the back.
    OrderBook book(3);
    book.applySnapshot(makeSnapshot(
        100, {},
        {{"50000.00", "1.0"}, {"49999.00", "1.0"}, {"49998.00", "1.0"}, {"49997.00", "1.0"}}));

    auto result =
        book.applyUpdate(makeUpdate(101, 101, {}, {{"49999.00", "0.0"}}), EventKind::Genuine);
    EXPECT_TRUE(result.success);

    // View must be ordered descending: 50000 > 49998 > 49997.
    auto stats = book.getStats();
    EXPECT_NEAR(stats.bestBid, 50000.0, 1e-6);

    ASSERT_EQ(result.deltas.size(), 2u);
    EXPECT_EQ(result.deltas[1].price, 4999700000000LL);  // 49997 * 1e8
    EXPECT_EQ(result.deltas[1].kind, EventKind::Maintenance);
    EXPECT_TRUE(result.deltas[1].inOfiView);
    EXPECT_GT(result.deltas[1].deltaQty, 0LL);
}

TEST(OrderBook, OfiReplacementScanBidDeleteWorst) {
    // ofiDepth=3, 4 bid levels. View=[50000, 49999, 49998], 49997 outside.
    // Delete worst-in-view bid (49998); 49997 should slide in at the back.
    OrderBook book(3);
    book.applySnapshot(makeSnapshot(
        100, {},
        {{"50000.00", "1.0"}, {"49999.00", "1.0"}, {"49998.00", "1.0"}, {"49997.00", "1.0"}}));

    auto result =
        book.applyUpdate(makeUpdate(101, 101, {}, {{"49998.00", "0.0"}}), EventKind::Genuine);
    EXPECT_TRUE(result.success);

    // View must be ordered descending: 50000 > 49999 > 49997.
    auto stats = book.getStats();
    EXPECT_NEAR(stats.bestBid, 50000.0, 1e-6);

    ASSERT_EQ(result.deltas.size(), 2u);
    EXPECT_EQ(result.deltas[1].price, 4999700000000LL);  // 49997 * 1e8
    EXPECT_EQ(result.deltas[1].kind, EventKind::Maintenance);
    EXPECT_TRUE(result.deltas[1].inOfiView);
    EXPECT_GT(result.deltas[1].deltaQty, 0LL);
}

TEST(OrderBook, OfiReplacementScanBidNoReplacementWhenNothingOutside) {
    // ofiDepth=2, exactly 2 bid levels — both in view, nothing outside.
    // Deleting one shrinks the view to 1 with no Maintenance delta.
    OrderBook book(2);
    book.applySnapshot(makeSnapshot(100, {}, {{"50000.00", "1.0"}, {"49999.00", "1.0"}}));

    auto result =
        book.applyUpdate(makeUpdate(101, 101, {}, {{"50000.00", "0.0"}}), EventKind::Genuine);
    EXPECT_TRUE(result.success);

    ASSERT_EQ(result.deltas.size(), 1u);
    EXPECT_EQ(result.deltas[0].kind, EventKind::Genuine);

    auto stats = book.getStats();
    EXPECT_NEAR(stats.bestBid, 49999.0, 1e-6);
}

TEST(OrderBook, OfiReplacementScanBidEmptyViewGetsRefilled) {
    // ofiDepth=1: deleting the only in-view bid triggers the !hasWorst path
    // (any remaining bid is eligible). The next bid should fill the view.
    OrderBook book(1);
    book.applySnapshot(makeSnapshot(100, {}, {{"50000.00", "1.0"}, {"49999.00", "1.0"}}));

    auto result =
        book.applyUpdate(makeUpdate(101, 101, {}, {{"50000.00", "0.0"}}), EventKind::Genuine);
    EXPECT_TRUE(result.success);

    auto stats = book.getStats();
    EXPECT_NEAR(stats.bestBid, 49999.0, 1e-6);

    // Maintenance delta for 49999 entering the view.
    ASSERT_EQ(result.deltas.size(), 2u);
    EXPECT_EQ(result.deltas[1].kind, EventKind::Maintenance);
    EXPECT_EQ(result.deltas[1].price, 4999900000000LL);  // 49999 * 1e8
    EXPECT_TRUE(result.deltas[1].inOfiView);
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

// ─── Recentering on out-of-range updates ─────────────────────────────────────

TEST(OrderBook, IncrementalUpdateDirectionalRecenter) {
    // Default tick = $0.01 (1 000 000), halfRange = 5 000 → window ±$50.
    OrderBook book;
    book.applySnapshot(makeSnapshot(100, {{"50001.00", "1.5"}}, {{"49999.00", "2.0"}}));

    auto stats = book.getStats();
    EXPECT_DOUBLE_EQ(stats.bestAsk, 50001.0);
    EXPECT_DOUBLE_EQ(stats.bestBid, 49999.0);

    // Far-away ask ABOVE the window → deep-book level for asks → dropped.
    // Far-away bid ABOVE the window → market moved up → recentered.
    auto res = book.applyUpdate(makeUpdate(101, 101,
                                           {{"50200.00", "0.1"}},  // deep ask → dropped
                                           {{"50100.00", "3.0"}}), // bid above window → recenter
                                EventKind::Genuine);
    EXPECT_TRUE(res.success);

    stats = book.getStats();
    // The far-away ask was dropped (it's a deep-book ask above the window).
    // The original ask at 50001.00 may or may not survive the bid recenter.
    // The bid at 50100.00 triggered a recenter and was applied.
    EXPECT_GE(stats.bidsCount, 1u);
    EXPECT_NEAR(stats.bestBid, 50100.0, 1e-6);
}

TEST(OrderBook, DeltaDirectionalRecentering) {
    OrderBook book;
    book.applySnapshot(makeSnapshot(0, {{"50001.00", "1.5"}}, {{"49999.00", "2.0"}}));

    // Kraken-style applyDelta:
    //   - bid at $49960 is in range → applied normally
    //   - bid at $40000 is BELOW the window → deep-book bid → dropped
    //   - ask at $49800 is BELOW the window → market moved down → recenter
    nlohmann::json bids = nlohmann::json::array();
    bids.push_back({"49960.00", "5.0"});   // in range
    bids.push_back({"40000.00", "1.0"});   // below window → dropped (deep bid)
    nlohmann::json asks = nlohmann::json::array();
    asks.push_back({"49800.00", "2.0"});   // below window → recentered (market moved down)

    auto deltas = book.applyDelta(bids, asks, EventKind::Genuine);

    auto stats = book.getStats();
    // $49960 applied (in range), $40000 dropped (deep bid below window).
    EXPECT_EQ(stats.bidsCount, 2u);  // original $49999 + $49960
    EXPECT_NEAR(stats.bestBid, 49999.0, 1e-6);
    // $49800 ask triggered recenter and was applied.
    EXPECT_GE(stats.asksCount, 1u);
}
// Regression test for int overflow in PriceLadder::inRange/toIdx.
// A price that overflows static_cast<int>(offset/tickSize) must be correctly
// rejected — not silently wrapped to a negative index.
TEST(OrderBook, SnapshotWithExtremeOutOfRangePriceDoesNotCrash) {
    OrderBook book(10);
    // Normal bid first (sets center around 405.00), then an absurd price
    // ($161,332,696.00 — the exact value seen in the Kraken crash).
    auto snap = makeSnapshot(
        0,
        {{"406.00", "1.00000000"}},                    // ask
        {{"405.00", "1.00000000"},                      // bid — sets center
         {"161332696.00000000", "0.05190000"}}          // extreme price
    );
    // Must not crash (segfault).  The extreme price should be silently
    // dropped by the inRange guard.
    EXPECT_NO_FATAL_FAILURE(book.applySnapshot(snap));
    auto stats = book.getStats();
    EXPECT_EQ(stats.bidsCount, 1u);  // only 405.00 survives
    EXPECT_EQ(stats.asksCount, 1u);
}
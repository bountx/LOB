#pragma once
#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>
#include <span>
#include <utility>
#include <vector>

#include "ofi_types.hpp"
#include "price_ladder.hpp"

class OrderBook {
public:
    struct Stats {
        std::size_t asksCount = 0;
        std::size_t bidsCount = 0;
        double bestAsk = 0.0;
        double bestBid = 0.0;
    };

    // Used by SubscriberServer to send a full-book snapshot to new clients.
    // bids sorted descending (best bid first), asks sorted ascending (best ask first).
    struct Snapshot {
        bool applied = false;
        std::vector<std::pair<long long, long long>> asks;
        std::vector<std::pair<long long, long long>> bids;
    };

    // ofiDepth: number of top price levels tracked in the OFI view per side (default 10).
    // tickSize: minimum price increment in internal 1e8 units (default 1 000 000 = $0.01).
    explicit OrderBook(std::size_t ofiDepth = 10,
                       long long tickSize = PriceLadder::kDefaultTick);

    // Clears all state and rebuilds from the Binance-format snapshot JSON.
    // Rebuilds the OFI view from the top-ofiDepth levels of the new state.
    void applySnapshot(const nlohmann::json& snapshot);

    // Applies a Binance-format incremental update (sequence-checked).
    // Returns success=false and empty deltas if a sequence gap is detected (caller must resync).
    // kind should be Backfill during buffer replay, Genuine for live stream updates.
    UpdateResult applyUpdate(const nlohmann::json& update, EventKind kind);

    // Pre-parsed hot-path overload — skips JSON parsing entirely.
    // asks/bids contain {price, qty} pairs already scaled by 1e8.
    UpdateResult applyUpdate(long long firstId, long long lastId,
                             std::span<const std::pair<long long, long long>> asks,
                             std::span<const std::pair<long long, long long>> bids, EventKind kind);

    // Applies bid/ask level arrays directly (no sequence checking) — used by Kraken.
    // bids/asks: JSON arrays of ["price", "qty"] string pairs; qty "0" removes the level.
    // kind: Genuine for live Kraken incremental updates.
    std::vector<LevelDelta> applyDelta(const nlohmann::json& bids, const nlohmann::json& asks,
                                       EventKind kind);

    void clear();
    long long getLastUpdateId() const;
    bool isSnapshotApplied() const;
    Stats getStats() const;

    // Returns the full-book snapshot sorted for subscriber broadcasting (cold path).
    Snapshot getSnapshot() const;

    void printOrderBook() const;
    void printOrderBookStats() const;

private:
    using Level = std::pair<long long, long long>;  // {price, qty}

    mutable std::mutex orderBookMutex;
    long long lastUpdateId = 0;
    std::atomic<bool> snapshotApplied{false};
    std::size_t ofiDepth;

    // Full order book state via price ladders — O(1) insert/update/delete,
    // O(1) best-price, O(ticks_gap) next/prev level (≈ O(1) for dense books).
    PriceLadder bidLadder;
    PriceLadder askLadder;

    // OFI view: top-ofiDepth levels per side, kept sorted.
    // ofiBids: sorted descending by price (best bid first).
    // ofiAsks: sorted ascending by price (best ask first).
    std::vector<Level> ofiBids;
    std::vector<Level> ofiAsks;

    // Returned by updateOfiView to describe secondary OFI-view structural changes.
    struct ViewChangeResult {
        long long evictedPrice = 0;
        long long evictedQty = 0;
        long long replacementPrice = 0;
        long long replacementQty = 0;
    };

    // Shared sequencing + mutation core for both applyUpdate overloads.
    // Must be called with orderBookMutex held.
    UpdateResult applyUpdateCore(long long firstId, long long lastId,
                                 std::span<const std::pair<long long, long long>> asks,
                                 std::span<const std::pair<long long, long long>> bids,
                                 EventKind kind);

    // Apply one price-level change to state + OFI view. Emits a LevelDelta into `out`.
    // Must be called with orderBookMutex held.
    void applyLevelChange(long long price, long long newQty, bool isBid, EventKind kind,
                          std::vector<LevelDelta>& out);

    // Update OFI view for one side after a level change.
    ViewChangeResult updateOfiView(long long price, long long newQty, bool isBid);

    // Full rebuild of one OFI view side from the ladder (O(ofiDepth * avg_gap)).
    // Must be called with orderBookMutex held.
    void rebuildOfiSide(bool isBid);
};

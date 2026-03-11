#include "order_book.hpp"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <iostream>
#include <stdexcept>

#include "decimal.hpp"

namespace {
constexpr double kPriceScale = 100'000'000.0;
}

/**
 * @brief Construct an OrderBook with a specified Out-of-Index (OFI) view depth.
 *
 * @param ofiDepthArg Maximum number of price levels to keep in each OFI side (bids and asks).
 */
OrderBook::OrderBook(std::size_t ofiDepthArg) : ofiDepth(ofiDepthArg) {}

/**
 * @brief Replace the entire live order book state from a snapshot JSON.
 *
 * Parses the provided snapshot into temporary state and, on successful parsing,
 * atomically swaps it into the live book: updates lastUpdateId, replaces bid
 * and ask maps, rebuilds OFI views for both sides, and marks the snapshot as
 * applied. Parsing failures leave the existing live state unchanged.
 *
 * @param snapshot JSON object expected to contain:
 *  - "lastUpdateId": numeric update identifier,
 *  - "asks": array of [priceString, quantityString] pairs,
 *  - "bids": array of [priceString, quantityString] pairs.
 *  Price and quantity strings are parsed via parseDecimal; levels with
 *  quantity equal to zero are omitted.
 */
void OrderBook::applySnapshot(const nlohmann::json& snapshot) {
    // Parse into temporaries first so a throw leaves the live book unchanged.
    const long long newLastUpdateId = snapshot["lastUpdateId"].get<long long>();
    std::unordered_map<long long, long long> newBids;
    std::unordered_map<long long, long long> newAsks;
    for (const auto& ask : snapshot["asks"]) {
        long long price = parseDecimal(ask[0].get<std::string>());
        long long qty = parseDecimal(ask[1].get<std::string>());
        if (qty > 0) {
            newAsks[price] = qty;
        }
    }
    for (const auto& bid : snapshot["bids"]) {
        long long price = parseDecimal(bid[0].get<std::string>());
        long long qty = parseDecimal(bid[1].get<std::string>());
        if (qty > 0) {
            newBids[price] = qty;
        }
    }
    // All parsing succeeded — take the lock and swap atomically.
    std::lock_guard<std::mutex> lock(orderBookMutex);
    lastUpdateId = newLastUpdateId;
    bidState = std::move(newBids);
    askState = std::move(newAsks);
    ofiBids.clear();
    ofiAsks.clear();
    rebuildOfiSide(true);
    rebuildOfiSide(false);
    snapshotApplied.store(true);
    printf("snapshot applied\n");
}

/**
 * @brief Apply a parsed incremental update to the order book and produce level/OFI deltas.
 *
 * Requires the caller to hold orderBookMutex. Validates sequence continuity using
 * `firstId`/`lastId`, applies the provided ask and bid level changes to internal
 * state, advances `lastUpdateId` on success, and accumulates any emitted
 * LevelDelta entries describing direct level changes and OFI-view side effects.
 *
 * If `lastId` is less than or equal to the current `lastUpdateId`, the update
 * is treated as already applied and no deltas are produced. If `firstId` is
 * greater than `lastUpdateId + 1`, a gap is detected and the update is not applied.
 *
 * @param firstId First sequence id of the incoming update range.
 * @param lastId Last sequence id of the incoming update range.
 * @param asks Span of (price, quantity) pairs representing ask-level updates; values are stored as
 * internal scaled integers.
 * @param bids Span of (price, quantity) pairs representing bid-level updates; values are stored as
 * internal scaled integers.
 * @param kind Event kind that categorizes emitted deltas (e.g., Maintenance or Backfill).
 * @return UpdateResult `success` is `true` when the update was applied (or already applied),
 * `false` when a gap was detected. `deltas` contains emitted LevelDelta entries.
 */
UpdateResult OrderBook::applyUpdateCore(long long firstId, long long lastId,
                                        std::span<const std::pair<long long, long long>> asks,
                                        std::span<const std::pair<long long, long long>> bids,
                                        EventKind kind) {
    // Must be called with orderBookMutex held.
    if (lastId <= lastUpdateId) {
        return {true, {}};
    }
    if (firstId > lastUpdateId + 1) {
        printf("gap in update IDs, triggering resync\n");
        return {false, {}};
    }
    UpdateResult result;
    result.success = true;
    for (const auto& [price, qty] : asks) {
        applyLevelChange(price, qty, false, kind, result.deltas);
    }
    for (const auto& [price, qty] : bids) {
        applyLevelChange(price, qty, true, kind, result.deltas);
    }
    lastUpdateId = lastId;
    return result;
}

/**
 * @brief Parse a JSON incremental update and apply it to the live order book.
 *
 * Parses the JSON fields for first and last update IDs and the ask/bid level
 * arrays, then applies the parsed update to the book under the internal mutex.
 * Parsing is performed before acquiring the mutex so parse failures do not
 * partially mutate the live state.
 *
 * @param update JSON object containing incremental update fields:
 *               - "U": first update ID
 *               - "u": last update ID
 *               - "a": array of ask levels [price, quantity]
 *               - "b": array of bid levels [price, quantity]
 * @param kind   Event kind to attribute to resulting level deltas.
 * @return UpdateResult Result describing whether the update was applied and any
 *                      generated level deltas; indicates failure when the
 *                      update is out-of-order or a gap is detected.
 */
UpdateResult OrderBook::applyUpdate(const nlohmann::json& update, EventKind kind) {
    // Parse all data before acquiring the lock so a JSON exception cannot leave
    // the live book half-mutated.
    const long long firstId = update["U"].get<long long>();
    const long long lastId = update["u"].get<long long>();
    std::vector<std::pair<long long, long long>> asks, bids;
    for (const auto& ask : update["a"]) {
        asks.push_back(
            {parseDecimal(ask[0].get<std::string>()), parseDecimal(ask[1].get<std::string>())});
    }
    for (const auto& bid : update["b"]) {
        bids.push_back(
            {parseDecimal(bid[0].get<std::string>()), parseDecimal(bid[1].get<std::string>())});
    }
    std::lock_guard<std::mutex> lock(orderBookMutex);
    return applyUpdateCore(firstId, lastId, asks, bids, kind);
}

/**
 * @brief Applies a parsed incremental update to the order book using an ID range and level changes.
 *
 * Applies the provided asks and bids (price/quantity pairs) for update IDs in the closed interval
 * [firstId, lastId] and returns the resulting update outcome and any level deltas.
 *
 * @param firstId First update identifier in the incoming update sequence.
 * @param lastId Last update identifier in the incoming update sequence.
 * @param asks Span of ask levels as (price, quantity) pairs; both values are stored as integers
 * using kPriceScale.
 * @param bids Span of bid levels as (price, quantity) pairs; both values are stored as integers
 * using kPriceScale.
 * @param kind EventKind that classifies the origin of the update (affects how deltas are labeled).
 * @return UpdateResult Result describing whether the update was applied and the list of produced
 * LevelDelta entries. On detection of a gap (missing updates) the result indicates failure and
 * contains no deltas.
 */
UpdateResult OrderBook::applyUpdate(long long firstId, long long lastId,
                                    std::span<const std::pair<long long, long long>> asks,
                                    std::span<const std::pair<long long, long long>> bids,
                                    EventKind kind) {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    return applyUpdateCore(firstId, lastId, asks, bids, kind);
}

/**
 * @brief Apply a batch of bid and ask level updates and produce the resulting level deltas.
 *
 * Parses the provided JSON level arrays and applies each price-level change to the live order
 * book, updating OFI views and emitting LevelDelta records for direct changes and any
 * secondary changes caused by OFI membership updates.
 *
 * @param bidsArr JSON array of bid levels; each element must be an array ["price", "quantity"] with
 * string values.
 * @param asksArr JSON array of ask levels; each element must be an array ["price", "quantity"] with
 * string values.
 * @param kind EventKind indicating the source/type of the update (e.g., Maintenance or Backfill).
 * @return std::vector<LevelDelta> Vector of LevelDelta entries produced by applying the provided
 * updates.
 */
std::vector<LevelDelta> OrderBook::applyDelta(const nlohmann::json& bidsArr,
                                              const nlohmann::json& asksArr, EventKind kind) {
    // Parse all level data before acquiring the lock so that a JSON/parse exception
    // cannot leave the live book half-mutated.
    struct ParsedLevel {
        long long price;
        long long qty;
        bool isBid;
    };
    std::vector<ParsedLevel> parsed;
    for (const auto& bid : bidsArr) {
        parsed.push_back({parseDecimal(bid[0].get<std::string>()),
                          parseDecimal(bid[1].get<std::string>()), true});
    }
    for (const auto& ask : asksArr) {
        parsed.push_back({parseDecimal(ask[0].get<std::string>()),
                          parseDecimal(ask[1].get<std::string>()), false});
    }

    std::lock_guard<std::mutex> lock(orderBookMutex);
    std::vector<LevelDelta> deltas;
    for (const auto& lvl : parsed) {
        applyLevelChange(lvl.price, lvl.qty, lvl.isBid, kind, deltas);
    }
    return deltas;
}

/**
 * @brief Apply a single price-level change to the internal state and update OFI views, emitting
 * resulting deltas.
 *
 * Applies the new quantity for `price` on the bid side if `isBid` is true, otherwise on the ask
 * side. Updates the OFI (top-of-book) view for that side and appends one or more LevelDelta entries
 * to `out` describing the direct change to `price` and any secondary view-membership changes caused
 * by the update.
 *
 * If the net quantity change for `price` is zero, the function performs no mutation and appends no
 * deltas.
 *
 * @param price Integer-encoded price key (scaled).
 * @param newQty New integer-encoded quantity for the price; zero means remove the level.
 * @param isBid True to operate on bids, false to operate on asks.
 * @param kind Event kind that triggered the change; propagated to emitted deltas (secondaries are
 *             tagged as Backfill if the triggering kind is Backfill, otherwise as Maintenance).
 * @param[out] out Vector to which generated LevelDelta entries (direct and secondary) are appended.
 */
void OrderBook::applyLevelChange(long long price, long long newQty, bool isBid, EventKind kind,
                                 std::vector<LevelDelta>& out) {
    auto& state = isBid ? bidState : askState;
    long long prevQty = 0;
    auto it = state.find(price);
    if (it != state.end()) {
        prevQty = it->second;
    }

    if (newQty == 0) {
        state.erase(price);
    } else {
        state[price] = newQty;
    }

    const long long delta = newQty - prevQty;
    if (delta == 0) {
        return;
    }

    // Check view membership directly (no copy) before mutating the view.
    const std::vector<Level>& viewRef = isBid ? ofiBids : ofiAsks;
    const bool wasInView = std::any_of(viewRef.begin(), viewRef.end(),
                                       [price](const Level& l) { return l.first == price; });

    // updateOfiView reports any structural change it makes (eviction / replacement)
    // so we never need a before-snapshot or post-diff.
    const ViewChangeResult viewChange = updateOfiView(price, newQty, isBid);

    const bool nowInView = std::any_of(viewRef.begin(), viewRef.end(),
                                       [price](const Level& l) { return l.first == price; });

    out.push_back(LevelDelta{price, newQty, delta, isBid, wasInView, nowInView, kind});

    // Secondary deltas for OFI-view structural changes caused by this update.
    // deltaQty is the view-contribution change (not a state change):
    //   eviction:    -evictedQty  (view qty dropped from evictedQty to 0)
    //   replacement: +replacementQty  (view qty grew from 0 to replacementQty)
    // Propagate the phase kind: Backfill → Backfill; Genuine → Maintenance.
    const EventKind secondaryKind =
        (kind == EventKind::Backfill) ? EventKind::Backfill : EventKind::Maintenance;

    if (viewChange.evictedPrice != 0) {
        const long long q = viewChange.evictedQty;
        out.push_back(
            LevelDelta{viewChange.evictedPrice, q, -q, isBid, true, false, secondaryKind});
    }
    if (viewChange.replacementPrice != 0) {
        const long long q = viewChange.replacementQty;
        out.push_back(
            LevelDelta{viewChange.replacementPrice, q, q, isBid, false, true, secondaryKind});
    }
}

OrderBook::ViewChangeResult OrderBook::updateOfiView(long long price, long long newQty,
                                                     bool isBid) {
    ViewChangeResult result;
    if (isBid) {
        // ofiBids sorted descending: front = best bid, back = worst bid in view.
        auto it = std::lower_bound(ofiBids.begin(), ofiBids.end(), price,
                                   [](const Level& a, long long p) { return a.first > p; });
        const bool inView = (it != ofiBids.end() && it->first == price);

        if (inView) {
            if (newQty == 0) {
                const bool wasFull = (ofiBids.size() == ofiDepth);
                ofiBids.erase(it);
                // Find the single replacement: max bid price strictly below the new worst-in-view.
                // O(N) scan, no allocation. The replacement always belongs at push_back()
                // because it's worse than every price currently in the view.
                // When ofiBids is empty any bid qualifies (no upper bound).
                const bool hasWorst = !ofiBids.empty();
                const long long worstInView = hasWorst ? ofiBids.back().first : 0;
                long long repPrice = 0, repQty = 0;
                for (const auto& [p, q] : bidState) {
                    if ((!hasWorst || p < worstInView) && p > repPrice) {
                        repPrice = p;
                        repQty = q;
                    }
                }
                if (repPrice != 0) {
                    ofiBids.push_back({repPrice, repQty});
                }
                if (wasFull && ofiBids.size() == ofiDepth) {
                    result.replacementPrice = ofiBids.back().first;
                    result.replacementQty = ofiBids.back().second;
                }
            } else {
                it->second = newQty;
            }
        } else if (newQty > 0) {
            const bool viewNotFull = ofiBids.size() < ofiDepth;
            const bool beatWorst = !ofiBids.empty() && price > ofiBids.back().first;
            if (viewNotFull || beatWorst) {
                ofiBids.insert(it, {price, newQty});
                if (ofiBids.size() > ofiDepth) {
                    // Capture the evicted worst level before removing it.
                    result.evictedPrice = ofiBids.back().first;
                    result.evictedQty = ofiBids.back().second;
                    ofiBids.pop_back();
                }
            }
        }
    } else {
        // ofiAsks sorted ascending: front = best ask, back = worst ask in view.
        auto it = std::lower_bound(ofiAsks.begin(), ofiAsks.end(), price,
                                   [](const Level& a, long long p) { return a.first < p; });
        const bool inView = (it != ofiAsks.end() && it->first == price);

        if (inView) {
            if (newQty == 0) {
                const bool wasFull = (ofiAsks.size() == ofiDepth);
                ofiAsks.erase(it);
                // Find the single replacement: min ask price strictly above the new worst-in-view.
                // O(N) scan, no allocation. The replacement always belongs at push_back()
                // because it's worse than every price currently in the view.
                const long long threshold = ofiAsks.empty() ? 0 : ofiAsks.back().first;
                long long repPrice = 0, repQty = 0;
                for (const auto& [p, q] : askState) {
                    if (p > threshold && (repPrice == 0 || p < repPrice)) {
                        repPrice = p;
                        repQty = q;
                    }
                }
                if (repPrice != 0) {
                    ofiAsks.push_back({repPrice, repQty});
                }
                if (wasFull && ofiAsks.size() == ofiDepth) {
                    result.replacementPrice = ofiAsks.back().first;
                    result.replacementQty = ofiAsks.back().second;
                }
            } else {
                it->second = newQty;
            }
        } else if (newQty > 0) {
            const bool viewNotFull = ofiAsks.size() < ofiDepth;
            const bool beatWorst = !ofiAsks.empty() && price < ofiAsks.back().first;
            if (viewNotFull || beatWorst) {
                ofiAsks.insert(it, {price, newQty});
                if (ofiAsks.size() > ofiDepth) {
                    result.evictedPrice = ofiAsks.back().first;
                    result.evictedQty = ofiAsks.back().second;
                    ofiAsks.pop_back();
                }
            }
        }
    }
    return result;
}

/**
 * @brief Rebuilds the OFI (top-of-book) view for the specified side from the full order state.
 *
 * Recomputes the side's OFI list to contain up to `ofiDepth` price levels selected from the full
 * side state: the highest prices for bids and the lowest prices for asks.
 *
 * @param isBid If `true`, rebuild the bid OFI view (best bids first); if `false`, rebuild the ask
 *              OFI view (best asks first).
 */
void OrderBook::rebuildOfiSide(bool isBid) {
    if (isBid) {
        std::vector<Level> all(bidState.begin(), bidState.end());
        const std::size_t n = std::min(ofiDepth, all.size());
        std::partial_sort(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(n), all.end(),
                          [](const Level& a, const Level& b) { return a.first > b.first; });
        ofiBids.assign(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(n));
    } else {
        std::vector<Level> all(askState.begin(), askState.end());
        const std::size_t n = std::min(ofiDepth, all.size());
        std::partial_sort(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(n), all.end(),
                          [](const Level& a, const Level& b) { return a.first < b.first; });
        ofiAsks.assign(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(n));
    }
}

/**
 * @brief Reset the order book to an empty, not-applied state.
 *
 * Clears all bid/ask state and OFI views, sets last update id to 0,
 * and marks that no snapshot has been applied.
 *
 * This operation is performed under the internal mutex to synchronize
 * concurrent access to the live order book state.
 */
void OrderBook::clear() {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    lastUpdateId = 0;
    snapshotApplied.store(false);
    bidState.clear();
    askState.clear();
    ofiBids.clear();
    ofiAsks.clear();
}

/**
 * @brief Retrieve the last applied update identifier.
 *
 * @return long long The current last update identifier stored in the order book.
 */
long long OrderBook::getLastUpdateId() const {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    return lastUpdateId;
}

/**
 * @brief Indicates whether a snapshot has been applied to the order book.
 *
 * @return `true` if a snapshot has been applied, `false` otherwise.
 */
bool OrderBook::isSnapshotApplied() const { return snapshotApplied.load(); }

/**
 * @brief Produce a consistent summary of order-book counts and top-of-book prices.
 *
 * Returns a snapshot of aggregated statistics taken under the internal mutex so the
 * values reflect a consistent view at the time of the call. The counts are the total
 * number of price levels in the full bid and ask state; bestAsk and bestBid are taken
 * from the OFI (top-of-book) views and converted to floating-point by dividing by
 * kPriceScale.
 *
 * @return Stats Struct containing:
 *   - asksCount: number of ask levels,
 *   - bidsCount: number of bid levels,
 *   - bestAsk: best ask price (0.0 if OFI ask view is empty),
 *   - bestBid: best bid price (0.0 if OFI bid view is empty).
 */
OrderBook::Stats OrderBook::getStats() const {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    Stats s;
    s.asksCount = askState.size();
    s.bidsCount = bidState.size();
    if (!ofiAsks.empty()) {
        s.bestAsk = static_cast<double>(ofiAsks.front().first) / kPriceScale;
    }
    if (!ofiBids.empty()) {
        s.bestBid = static_cast<double>(ofiBids.front().first) / kPriceScale;
    }
    return s;
}

/**
 * @brief Retrieve a point-in-time snapshot of the current order book state.
 *
 * The returned Snapshot contains whether a full snapshot has been applied and, when
 * applied, the complete sets of ask and bid levels. Ask levels are ordered
 * ascending by price (best ask first), and bid levels are ordered descending by
 * price (best bid first).
 *
 * @return Snapshot
 *   - `applied`: `true` if a full snapshot has been applied, `false` otherwise.
 *   - `asks`: vector of ask price/quantity pairs ordered ascending by price.
 *   - `bids`: vector of bid price/quantity pairs ordered descending by price.
 */
OrderBook::Snapshot OrderBook::getSnapshot() const {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    Snapshot s;
    s.applied = snapshotApplied.load();
    if (!s.applied) {
        return s;
    }

    s.asks.assign(askState.begin(), askState.end());
    std::sort(s.asks.begin(), s.asks.end(),
              [](const Level& a, const Level& b) { return a.first < b.first; });

    s.bids.assign(bidState.begin(), bidState.end());
    std::sort(s.bids.begin(), s.bids.end(),
              [](const Level& a, const Level& b) { return a.first > b.first; });

    return s;
}

/**
 * @brief Print the current order book (last update id, asks, and bids) to standard output.
 *
 * Acquires the internal mutex and writes the last update id followed by all ask
 * levels sorted ascending by price and all bid levels sorted descending by price.
 * Prices and quantities are converted from the internal integer representation
 * to floating-point values using kPriceScale.
 */
void OrderBook::printOrderBook() const {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    std::cout << "Last Update ID: " << lastUpdateId << '\n';

    std::vector<Level> sortedAsks(askState.begin(), askState.end());
    std::sort(sortedAsks.begin(), sortedAsks.end(),
              [](const Level& a, const Level& b) { return a.first < b.first; });
    std::cout << "Asks:\n";
    for (const auto& [price, qty] : sortedAsks) {
        std::cout << "Price: " << static_cast<double>(price) / kPriceScale
                  << ", Quantity: " << static_cast<double>(qty) / kPriceScale << '\n';
    }

    std::vector<Level> sortedBids(bidState.begin(), bidState.end());
    std::sort(sortedBids.begin(), sortedBids.end(),
              [](const Level& a, const Level& b) { return a.first > b.first; });
    std::cout << "Bids:\n";
    for (const auto& [price, qty] : sortedBids) {
        std::cout << "Price: " << static_cast<double>(price) / kPriceScale
                  << ", Quantity: " << static_cast<double>(qty) / kPriceScale << '\n';
    }
}

/**
 * @brief Print a brief summary of order-book counts and best-of-book prices to stdout.
 *
 * Acquires the internal mutex and writes the total number of ask and bid price
 * levels and the best ask/bid (top of OFI view) to standard output. Best prices
 * are scaled by kPriceScale and formatted with two decimal places; if an OFI
 * side is empty its best price is reported as 0.00.
 */
void OrderBook::printOrderBookStats() const {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    std::cout << "Total Asks: " << askState.size() << ", Total Bids: " << bidState.size() << '\n';
    std::cout << std::fixed << std::setprecision(2) << "Best Ask: "
              << (ofiAsks.empty() ? 0.0 : static_cast<double>(ofiAsks.front().first) / kPriceScale)
              << ", Best Bid: "
              << (ofiBids.empty() ? 0.0 : static_cast<double>(ofiBids.front().first) / kPriceScale)
              << '\n';
}

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

OrderBook::OrderBook(std::size_t ofiDepthArg, long long tickSize)
    : ofiDepth(ofiDepthArg),
      bidLadder(tickSize),
      askLadder(tickSize) {}

void OrderBook::applySnapshot(const nlohmann::json& snapshot) {
    const long long newLastUpdateId = snapshot["lastUpdateId"].get<long long>();
    std::vector<std::pair<long long, long long>> newAsks, newBids;
    for (const auto& ask : snapshot["asks"]) {
        long long price = parseDecimal(ask[0].get<std::string>());
        long long qty   = parseDecimal(ask[1].get<std::string>());
        if (qty > 0) newAsks.push_back({price, qty});
    }
    for (const auto& bid : snapshot["bids"]) {
        long long price = parseDecimal(bid[0].get<std::string>());
        long long qty   = parseDecimal(bid[1].get<std::string>());
        if (qty > 0) newBids.push_back({price, qty});
    }

    std::lock_guard<std::mutex> lock(orderBookMutex);
    lastUpdateId = newLastUpdateId;
    bidLadder.clear();
    askLadder.clear();
    for (const auto& [p, q] : newBids) bidLadder.set(p, q);
    for (const auto& [p, q] : newAsks) askLadder.set(p, q);
    ofiBids.clear();
    ofiAsks.clear();
    rebuildOfiSide(true);
    rebuildOfiSide(false);
    snapshotApplied.store(true);
    printf("snapshot applied\n");
}

UpdateResult OrderBook::applyUpdateCore(long long firstId, long long lastId,
                                        std::span<const std::pair<long long, long long>> asks,
                                        std::span<const std::pair<long long, long long>> bids,
                                        EventKind kind) {
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

UpdateResult OrderBook::applyUpdate(const nlohmann::json& update, EventKind kind) {
    const long long firstId = update["U"].get<long long>();
    const long long lastId  = update["u"].get<long long>();
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

UpdateResult OrderBook::applyUpdate(long long firstId, long long lastId,
                                    std::span<const std::pair<long long, long long>> asks,
                                    std::span<const std::pair<long long, long long>> bids,
                                    EventKind kind) {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    return applyUpdateCore(firstId, lastId, asks, bids, kind);
}

std::vector<LevelDelta> OrderBook::applyDelta(const nlohmann::json& bidsArr,
                                              const nlohmann::json& asksArr, EventKind kind) {
    struct ParsedLevel { long long price; long long qty; bool isBid; };
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

void OrderBook::applyLevelChange(long long price, long long newQty, bool isBid, EventKind kind,
                                 std::vector<LevelDelta>& out) {
    auto& ladder = isBid ? bidLadder : askLadder;
    const long long prevQty = ladder.get(price);

    ladder.set(price, newQty);

    const long long delta = newQty - prevQty;
    if (delta == 0) { return; }

    const std::vector<Level>& viewRef = isBid ? ofiBids : ofiAsks;
    const bool wasInView = std::any_of(viewRef.begin(), viewRef.end(),
                                       [price](const Level& l) { return l.first == price; });

    const ViewChangeResult viewChange = updateOfiView(price, newQty, isBid);

    const bool nowInView = std::any_of(viewRef.begin(), viewRef.end(),
                                       [price](const Level& l) { return l.first == price; });

    out.push_back(LevelDelta{price, newQty, delta, isBid, wasInView, nowInView, kind});

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
        auto it = std::lower_bound(ofiBids.begin(), ofiBids.end(), price,
                                   [](const Level& a, long long p) { return a.first > p; });
        const bool inView = (it != ofiBids.end() && it->first == price);

        if (inView) {
            if (newQty == 0) {
                const bool wasFull = (ofiBids.size() == ofiDepth);
                ofiBids.erase(it);
                // O(1) amortised: walk inward from the OFI boundary tick-by-tick.
                const bool hasWorst    = !ofiBids.empty();
                const long long worst  = hasWorst ? ofiBids.back().first : 0;
                const long long repPrice = hasWorst ? bidLadder.prevBelow(worst)
                                                    : bidLadder.bestHigh();
                const long long repQty   = repPrice > 0 ? bidLadder.get(repPrice) : 0;
                if (repPrice != 0) {
                    ofiBids.push_back({repPrice, repQty});
                }
                if (wasFull && ofiBids.size() == ofiDepth) {
                    result.replacementPrice = ofiBids.back().first;
                    result.replacementQty   = ofiBids.back().second;
                }
            } else {
                it->second = newQty;
            }
        } else if (newQty > 0) {
            const bool viewNotFull = ofiBids.size() < ofiDepth;
            const bool beatWorst   = !ofiBids.empty() && price > ofiBids.back().first;
            if (viewNotFull || beatWorst) {
                ofiBids.insert(it, {price, newQty});
                if (ofiBids.size() > ofiDepth) {
                    result.evictedPrice = ofiBids.back().first;
                    result.evictedQty   = ofiBids.back().second;
                    ofiBids.pop_back();
                }
            }
        }
    } else {
        auto it = std::lower_bound(ofiAsks.begin(), ofiAsks.end(), price,
                                   [](const Level& a, long long p) { return a.first < p; });
        const bool inView = (it != ofiAsks.end() && it->first == price);

        if (inView) {
            if (newQty == 0) {
                const bool wasFull = (ofiAsks.size() == ofiDepth);
                ofiAsks.erase(it);
                // O(1) amortised: walk outward from the OFI boundary tick-by-tick.
                const bool hasWorst      = !ofiAsks.empty();
                const long long worst    = hasWorst ? ofiAsks.back().first : 0;
                const long long repPrice = hasWorst ? askLadder.nextAbove(worst)
                                                    : askLadder.bestLow();
                const long long repQty   = repPrice > 0 ? askLadder.get(repPrice) : 0;
                if (repPrice != 0) {
                    ofiAsks.push_back({repPrice, repQty});
                }
                if (wasFull && ofiAsks.size() == ofiDepth) {
                    result.replacementPrice = ofiAsks.back().first;
                    result.replacementQty   = ofiAsks.back().second;
                }
            } else {
                it->second = newQty;
            }
        } else if (newQty > 0) {
            const bool viewNotFull = ofiAsks.size() < ofiDepth;
            const bool beatWorst   = !ofiAsks.empty() && price < ofiAsks.back().first;
            if (viewNotFull || beatWorst) {
                ofiAsks.insert(it, {price, newQty});
                if (ofiAsks.size() > ofiDepth) {
                    result.evictedPrice = ofiAsks.back().first;
                    result.evictedQty   = ofiAsks.back().second;
                    ofiAsks.pop_back();
                }
            }
        }
    }
    return result;
}

void OrderBook::rebuildOfiSide(bool isBid) {
    if (isBid) {
        ofiBids.clear();
        long long p = bidLadder.bestHigh();
        while (p > 0 && ofiBids.size() < ofiDepth) {
            ofiBids.push_back({p, bidLadder.get(p)});
            p = bidLadder.prevBelow(p);
        }
        // ofiBids is built best-first (descending) — already correct order.
    } else {
        ofiAsks.clear();
        long long p = askLadder.bestLow();
        while (p > 0 && ofiAsks.size() < ofiDepth) {
            ofiAsks.push_back({p, askLadder.get(p)});
            p = askLadder.nextAbove(p);
        }
        // ofiAsks is built best-first (ascending) — already correct order.
    }
}

void OrderBook::clear() {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    lastUpdateId = 0;
    snapshotApplied.store(false);
    bidLadder.clear();
    askLadder.clear();
    ofiBids.clear();
    ofiAsks.clear();
}

long long OrderBook::getLastUpdateId() const {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    return lastUpdateId;
}

bool OrderBook::isSnapshotApplied() const { return snapshotApplied.load(); }

OrderBook::Stats OrderBook::getStats() const {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    Stats s;
    s.asksCount = askLadder.activeCount();
    s.bidsCount = bidLadder.activeCount();
    if (!ofiAsks.empty()) {
        s.bestAsk = static_cast<double>(ofiAsks.front().first) / kPriceScale;
    }
    if (!ofiBids.empty()) {
        s.bestBid = static_cast<double>(ofiBids.front().first) / kPriceScale;
    }
    return s;
}

OrderBook::Snapshot OrderBook::getSnapshot() const {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    Snapshot s;
    s.applied = snapshotApplied.load();
    if (!s.applied) { return s; }

    askLadder.forEach([&s](long long p, long long q) { s.asks.push_back({p, q}); });
    // forEach visits in ascending order — asks are already ascending.

    bidLadder.forEach([&s](long long p, long long q) { s.bids.push_back({p, q}); });
    // forEach visits in ascending order — bids need descending.
    std::reverse(s.bids.begin(), s.bids.end());

    return s;
}

void OrderBook::printOrderBook() const {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    std::cout << "Last Update ID: " << lastUpdateId << '\n';

    std::vector<Level> sortedAsks, sortedBids;
    askLadder.forEach([&sortedAsks](long long p, long long q) { sortedAsks.push_back({p, q}); });
    bidLadder.forEach([&sortedBids](long long p, long long q) { sortedBids.push_back({p, q}); });
    std::reverse(sortedBids.begin(), sortedBids.end());  // descending

    std::cout << "Asks:\n";
    for (const auto& [price, qty] : sortedAsks) {
        std::cout << "Price: " << static_cast<double>(price) / kPriceScale
                  << ", Quantity: " << static_cast<double>(qty) / kPriceScale << '\n';
    }
    std::cout << "Bids:\n";
    for (const auto& [price, qty] : sortedBids) {
        std::cout << "Price: " << static_cast<double>(price) / kPriceScale
                  << ", Quantity: " << static_cast<double>(qty) / kPriceScale << '\n';
    }
}

void OrderBook::printOrderBookStats() const {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    std::cout << "Total Asks: " << askLadder.activeCount()
              << ", Total Bids: " << bidLadder.activeCount() << '\n';
    std::cout << std::fixed << std::setprecision(2) << "Best Ask: "
              << (ofiAsks.empty() ? 0.0 : static_cast<double>(ofiAsks.front().first) / kPriceScale)
              << ", Best Bid: "
              << (ofiBids.empty() ? 0.0 : static_cast<double>(ofiBids.front().first) / kPriceScale)
              << '\n';
}

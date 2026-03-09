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

OrderBook::OrderBook(std::size_t ofiDepthArg) : ofiDepth(ofiDepthArg) {}

void OrderBook::applySnapshot(const nlohmann::json& snapshot) {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    bidState.clear();
    askState.clear();
    ofiBids.clear();
    ofiAsks.clear();
    lastUpdateId = snapshot["lastUpdateId"].get<long long>();
    for (const auto& ask : snapshot["asks"]) {
        long long price = parseDecimal(ask[0].get<std::string>());
        long long qty = parseDecimal(ask[1].get<std::string>());
        if (qty > 0) {
            askState[price] = qty;
        }
    }
    for (const auto& bid : snapshot["bids"]) {
        long long price = parseDecimal(bid[0].get<std::string>());
        long long qty = parseDecimal(bid[1].get<std::string>());
        if (qty > 0) {
            bidState[price] = qty;
        }
    }
    rebuildOfiSide(true);
    rebuildOfiSide(false);
    snapshotApplied.store(true);
    printf("snapshot applied\n");
}

UpdateResult OrderBook::applyUpdate(const nlohmann::json& update, EventKind kind) {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    const long long firstId = update["U"].get<long long>();
    const long long lastId = update["u"].get<long long>();
    if (lastId <= lastUpdateId) {
        return {true, {}};
    }
    if (firstId > lastUpdateId + 1) {
        printf("gap in update IDs, triggering resync\n");
        return {false, {}};
    }
    UpdateResult result;
    result.success = true;
    for (const auto& ask : update["a"]) {
        long long price = parseDecimal(ask[0].get<std::string>());
        long long qty = parseDecimal(ask[1].get<std::string>());
        applyLevelChange(price, qty, false, kind, result.deltas);
    }
    for (const auto& bid : update["b"]) {
        long long price = parseDecimal(bid[0].get<std::string>());
        long long qty = parseDecimal(bid[1].get<std::string>());
        applyLevelChange(price, qty, true, kind, result.deltas);
    }
    lastUpdateId = lastId;
    return result;
}

std::vector<LevelDelta> OrderBook::applyDelta(const nlohmann::json& bidsArr,
                                              const nlohmann::json& asksArr, EventKind kind) {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    std::vector<LevelDelta> deltas;
    for (const auto& bid : bidsArr) {
        long long price = parseDecimal(bid[0].get<std::string>());
        long long qty = parseDecimal(bid[1].get<std::string>());
        applyLevelChange(price, qty, true, kind, deltas);
    }
    for (const auto& ask : asksArr) {
        long long price = parseDecimal(ask[0].get<std::string>());
        long long qty = parseDecimal(ask[1].get<std::string>());
        applyLevelChange(price, qty, false, kind, deltas);
    }
    return deltas;
}

void OrderBook::applyLevelChange(long long price, long long newQty, bool isBid, EventKind kind,
                                 std::vector<LevelDelta>& out) {
    auto& state = isBid ? bidState : askState;
    long long prevQty = 0;
    auto it = state.find(price);
    if (it != state.end()) prevQty = it->second;

    if (newQty == 0) {
        state.erase(price);
    } else {
        state[price] = newQty;
    }

    const long long delta = newQty - prevQty;
    if (delta == 0) return;

    updateOfiView(price, newQty, isBid);

    // Check if this price is currently in the OFI view (after the update).
    const auto& view = isBid ? ofiBids : ofiAsks;
    bool inView = false;
    if (!view.empty()) {
        // Range check first (cheap), then linear confirm (only if in range).
        const bool inRange = isBid ? (price >= view.back().first && price <= view.front().first)
                                   : (price <= view.back().first && price >= view.front().first);
        if (inRange) {
            inView = std::any_of(view.begin(), view.end(),
                                 [price](const Level& l) { return l.first == price; });
        }
    }

    out.push_back({price, newQty, delta, isBid, inView, kind});
}

void OrderBook::updateOfiView(long long price, long long newQty, bool isBid) {
    if (isBid) {
        // ofiBids sorted descending: front = best bid, back = worst bid in view.
        auto it = std::lower_bound(ofiBids.begin(), ofiBids.end(), price,
                                   [](const Level& a, long long p) { return a.first > p; });
        const bool inView = (it != ofiBids.end() && it->first == price);

        if (inView) {
            if (newQty == 0) {
                ofiBids.erase(it);
                rebuildOfiSide(true);
            } else {
                it->second = newQty;
            }
        } else if (newQty > 0) {
            const bool viewNotFull = ofiBids.size() < ofiDepth;
            const bool beatWorst = !ofiBids.empty() && price > ofiBids.back().first;
            if (viewNotFull || beatWorst) {
                ofiBids.insert(it, {price, newQty});
                if (ofiBids.size() > ofiDepth) {
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
                ofiAsks.erase(it);
                rebuildOfiSide(false);
            } else {
                it->second = newQty;
            }
        } else if (newQty > 0) {
            const bool viewNotFull = ofiAsks.size() < ofiDepth;
            const bool beatWorst = !ofiAsks.empty() && price < ofiAsks.back().first;
            if (viewNotFull || beatWorst) {
                ofiAsks.insert(it, {price, newQty});
                if (ofiAsks.size() > ofiDepth) {
                    ofiAsks.pop_back();
                }
            }
        }
    }
}

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

void OrderBook::clear() {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    lastUpdateId = 0;
    snapshotApplied.store(false);
    bidState.clear();
    askState.clear();
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

void OrderBook::printOrderBookStats() const {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    std::cout << "Total Asks: " << askState.size() << ", Total Bids: " << bidState.size() << '\n';
    std::cout << std::fixed << std::setprecision(2) << "Best Ask: "
              << (ofiAsks.empty() ? 0.0 : static_cast<double>(ofiAsks.front().first) / kPriceScale)
              << ", Best Bid: "
              << (ofiBids.empty() ? 0.0 : static_cast<double>(ofiBids.front().first) / kPriceScale)
              << '\n';
}

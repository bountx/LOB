#include "order_book.hpp"

#include <atomic>
#include <charconv>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>

#include "decimal.hpp"

namespace {
constexpr double kPriceScale = 100000000.0;
}

void OrderBook::applySnapshot(const nlohmann::json& snapshot) {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    asks.clear();
    bids.clear();
    lastUpdateId = snapshot["lastUpdateId"].get<long long>();
    for (const auto& ask : snapshot["asks"]) {
        long long price = parseDecimal(ask[0].get<std::string>());
        long long quantity = parseDecimal(ask[1].get<std::string>());
        asks[price] = quantity;
    }
    for (const auto& bid : snapshot["bids"]) {
        long long price = parseDecimal(bid[0].get<std::string>());
        long long quantity = parseDecimal(bid[1].get<std::string>());
        bids[price] = quantity;
    }
    snapshotApplied.store(true);
    printf("Snapshot applied successfully.\n");
}

bool OrderBook::applyUpdate(const nlohmann::json& update) {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    long long U = update["U"].get<long long>();  // first update ID in the stream
    long long u = update["u"].get<long long>();  // last update ID in the stream
    if (u <= lastUpdateId) {
        // Stale update, ignore
        printf("Stale update received, ignoring.\n");
        return true;
    }
    if (U > lastUpdateId + 1) {
        // Missed updates, restart from scratch
        printf("Missed updates, restarting from scratch.\n");
        return false;
    }
    for (const auto& ask : update["a"]) {
        long long price = parseDecimal(ask[0].get<std::string>());
        long long quantity = parseDecimal(ask[1].get<std::string>());
        if (quantity == 0) {
            asks.erase(price);
        } else {
            asks[price] = quantity;
        }
    }
    for (const auto& bid : update["b"]) {
        long long price = parseDecimal(bid[0].get<std::string>());
        long long quantity = parseDecimal(bid[1].get<std::string>());
        if (quantity == 0) {
            bids.erase(price);
        } else {
            bids[price] = quantity;
        }
    }
    lastUpdateId = u;
    return true;
}

void OrderBook::clear() {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    lastUpdateId = 0;
    snapshotApplied.store(false);
    asks.clear();
    bids.clear();
}

long long OrderBook::getLastUpdateId() const {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    return lastUpdateId;
}

std::map<long long, long long, std::less<>> OrderBook::getAsks() const {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    return asks;
}

std::map<long long, long long, std::greater<>> OrderBook::getBids() const {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    return bids;
}

bool OrderBook::isSnapshotApplied() const { return snapshotApplied.load(); }

OrderBook::Stats OrderBook::getStats() const {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    Stats s;
    s.asksCount = asks.size();
    s.bidsCount = bids.size();
    if (!asks.empty()) {
        s.bestAsk = static_cast<double>(asks.begin()->first) / kPriceScale;
    }
    if (!bids.empty()) {
        s.bestBid = static_cast<double>(bids.begin()->first) / kPriceScale;
    }
    return s;
}

void OrderBook::printOrderBook() const {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    std::cout << "Last Update ID: " << lastUpdateId << '\n';
    std::cout << "Asks:" << '\n';
    for (const auto& ask : asks) {
        std::cout << "Price: " << static_cast<double>(ask.first) / kPriceScale
                  << ", Quantity: " << static_cast<double>(ask.second) / kPriceScale << '\n';
    }
    std::cout << "Bids:" << '\n';
    for (const auto& bid : bids) {
        std::cout << "Price: " << static_cast<double>(bid.first) / kPriceScale
                  << ", Quantity: " << static_cast<double>(bid.second) / kPriceScale << '\n';
    }
}

void OrderBook::printOrderBookStats() const {
    std::lock_guard<std::mutex> lock(orderBookMutex);
    std::cout << "Total Asks: " << asks.size() << ", Total Bids: " << bids.size() << '\n';
    std::cout << std::fixed << std::setprecision(2) << "Best Ask: "
              << (asks.empty() ? 0.0 : static_cast<double>(asks.begin()->first) / kPriceScale)
              << ", Best Bid: "
              << (bids.empty() ? 0.0 : static_cast<double>(bids.begin()->first) / kPriceScale)
              << '\n';
}

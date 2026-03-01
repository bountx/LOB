#pragma once
#include <atomic>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>

class OrderBook {
public:
    struct Stats {
        std::size_t asksCount = 0;
        std::size_t bidsCount = 0;
        double bestAsk = 0.0;
        double bestBid = 0.0;
    };

    void applySnapshot(const nlohmann::json& snapshot);
    bool applyUpdate(const nlohmann::json& update);
    void clear();
    long long getLastUpdateId() const;
    bool isSnapshotApplied() const;
    Stats getStats() const;
    void printOrderBook() const;
    void printOrderBookStats() const;
    std::map<long long, long long, std::less<>> getAsks() const;
    std::map<long long, long long, std::greater<>> getBids() const;

private:
    mutable std::mutex orderBookMutex;
    long long lastUpdateId = 0;
    std::atomic<bool> snapshotApplied{false};
    std::map<long long, long long, std::less<>> asks;
    std::map<long long, long long, std::greater<>> bids;
};
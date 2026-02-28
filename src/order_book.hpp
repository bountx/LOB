#pragma once
#include <atomic>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>

class OrderBook {
public:
    void applySnapshot(const nlohmann::json& snapshot);
    bool applyUpdate(const nlohmann::json& update);
    void clear();
    long long getLastUpdateId() const;
    bool isSnapshotApplied() const;
    void printOrderBook() const;
    void printOrderBookStats() const;
    std::map<long long, long long, std::less<long long>> getAsks() const;
    std::map<long long, long long, std::greater<long long>> getBids() const;

private:
    mutable std::mutex orderBookMutex;
    long long lastUpdateId = 0;
    std::atomic<bool> snapshotApplied{false};
    std::map<long long, long long, std::less<long long>> asks;
    std::map<long long, long long, std::greater<long long>> bids;
};
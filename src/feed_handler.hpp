#pragma once
#include <ixwebsocket/IXWebSocket.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <queue>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "metrics.hpp"
#include "order_book.hpp"

class FeedHandler {
public:
    bool initialize(const std::vector<std::string>& symbols,
                    std::unordered_map<std::string, std::unique_ptr<OrderBook>>& books,
                    ix::WebSocket& webSocket,
                    std::unordered_map<std::string, std::unique_ptr<Metrics>>& metricsMap,
                    int snapshotDepth = 1000, int maxSnapshotRetries = 5);

private:
    std::unordered_map<std::string, std::unique_ptr<OrderBook>>* books = nullptr;
    std::unordered_map<std::string, std::unique_ptr<Metrics>>* metricsMap = nullptr;

    std::unordered_map<std::string, std::vector<nlohmann::json>> symbolBuffers;
    std::mutex bufferMutex;

    std::queue<std::string> resyncQueue;
    std::mutex resyncMutex;
    std::condition_variable resyncCv;

    std::mutex wsReadyMutex;
    std::condition_variable wsReady;
    bool wsConnected = false;

    std::jthread resyncThread;

    // "btcusdt@depth" → "BTCUSDT"
    static std::string streamToSymbol(const std::string& stream);

    int snapshotDepth = 1000;

    bool fetchAndApplySnapshot(const std::string& symbol, OrderBook& orderBook);
    void handleWsMessage(const ix::WebSocketMessagePtr& msg);
    void runResyncWorker(int maxSnapshotRetries, std::stop_token stoken);
};

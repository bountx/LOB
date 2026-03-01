#pragma once
#include <ixwebsocket/IXWebSocket.h>

#include <condition_variable>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stop_token>
#include <thread>
#include <vector>

#include "metrics.hpp"
#include "order_book.hpp"

class FeedHandler {
public:
    bool initialize(OrderBook& orderBook, ix::WebSocket& webSocket, Metrics& metrics,
                    int maxSnapshotRetries = 5);

private:
    std::mutex bufferMutex;
    std::vector<nlohmann::json> bufferedMessages;
    std::mutex resyncMutex;
    std::condition_variable resyncCv;
    bool needsResync = false;
    std::mutex wsReadyMutex;
    std::condition_variable wsReady;
    bool wsConnected = false;
    std::jthread resyncThread;

    bool fetchAndApplySnapshot(OrderBook& orderBook);
    void handleWsMessage(const ix::WebSocketMessagePtr& msg, OrderBook& orderBook,
                         Metrics& metrics);
    void runResyncWorker(OrderBook& orderBook, int maxSnapshotRetries, std::stop_token stoken);
};

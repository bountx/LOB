#pragma once
#include <ixwebsocket/IXWebSocket.h>

#include <condition_variable>
#include <mutex>
#include <nlohmann/json.hpp>
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
    std::mutex wsReadyMutex;
    std::condition_variable wsReady;
    bool wsConnected = false;

    bool fetchAndApplySnapshot(OrderBook& orderBook);
};
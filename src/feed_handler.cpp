#include "feed_handler.hpp"

#include <ixwebsocket/IXHttpClient.h>

#include <chrono>
#include <cstdio>
#include <thread>

bool FeedHandler::fetchAndApplySnapshot(OrderBook& orderBook) {
    ix::HttpClient httpClient;
    auto args = std::make_shared<ix::HttpRequestArgs>();
    args->connectTimeout = 5;    // seconds
    args->transferTimeout = 30;  // seconds
    auto response =
        httpClient.get("https://api.binance.com/api/v3/depth?symbol=BTCUSDT&limit=5000", args);
    if (response->statusCode != 200) {
        fprintf(stderr, "Failed to fetch snapshot: HTTP %d\n", response->statusCode);
        return false;
    }

    try {
        auto snapshot = nlohmann::json::parse(response->body);
        orderBook.applySnapshot(snapshot);

        // Apply buffered messages that arrived while we were fetching
        std::lock_guard<std::mutex> lock(bufferMutex);
        for (const auto& msg : bufferedMessages) {
            if (!orderBook.applyUpdate(msg)) {
                fprintf(stderr, "Buffered message caused re-sync condition\n");
                bufferedMessages.clear();
                return false;  // Trigger retry
            }
        }
        bufferedMessages.clear();
        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "Snapshot parse/apply error: %s\n", e.what());
        return false;
    }
}

bool FeedHandler::initialize(OrderBook& orderBook, ix::WebSocket& webSocket, Metrics& metrics,
                             int maxSnapshotRetries) {
    // Reset state for a fresh initialization
    orderBook.clear();
    {
        std::lock_guard<std::mutex> lock(bufferMutex);
        bufferedMessages.clear();
    }
    {
        std::lock_guard<std::mutex> lock(wsReadyMutex);
        wsConnected = false;
    }

    webSocket.setOnMessageCallback([&orderBook, &metrics, this,
                                    &webSocket](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
            printf("WebSocket connection opened.\n");
            std::lock_guard<std::mutex> lock(wsReadyMutex);
            wsConnected = true;
            wsReady.notify_one();
            return;
        }
        if (msg->type == ix::WebSocketMessageType::Close) {
            printf("WebSocket connection closed.\n");
            return;
        }
        if (msg->type == ix::WebSocketMessageType::Error) {
            printf("WebSocket error: %s\n", msg->errorInfo.reason.c_str());
            return;
        }
        if (msg->type != ix::WebSocketMessageType::Message) {
            return;
        }

        try {
            auto jsonMsg = nlohmann::json::parse(msg->str);

            // Buffer until snapshot is applied
            {
                std::lock_guard<std::mutex> lock(bufferMutex);
                if (!orderBook.isSnapshotApplied()) {
                    printf(
                        "Buffering message until snapshot is applied. Message event time: %lld\n",
                        jsonMsg["E"].get<long long>());
                    bufferedMessages.push_back(std::move(jsonMsg));
                    return;
                }
            }

            // Event lag metric
            long long eventTime = jsonMsg["E"].get<long long>();
            long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
            metrics.lastEventLagMs.store(now - eventTime);

            // Processing time metric
            auto start = std::chrono::high_resolution_clock::now();
            if (!orderBook.applyUpdate(jsonMsg)) {
                printf("Restarting from scratch due to missed updates.\n");
                orderBook.clear();  // sets snapshotApplied = false
                {
                    std::lock_guard<std::mutex> lock(bufferMutex);
                    bufferedMessages.clear();
                }
                {
                    std::lock_guard<std::mutex> lock(resyncMutex);
                    needsResync = true;
                }
                resyncCv.notify_one();
                return;
            }
            auto end = std::chrono::high_resolution_clock::now();
            long long processingUs =
                std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            metrics.lastProcessingUs.store(processingUs);

            // Update max processing time if needed
            long long prevMax = metrics.maxProcessingUs.load();
            while (processingUs > prevMax &&
                   !metrics.maxProcessingUs.compare_exchange_weak(prevMax, processingUs)) {
            }

            metrics.msgCount.fetch_add(1);
        } catch (const std::exception& ex) {
            printf("Error processing message: %s\n", ex.what());
        }
    });

    if (webSocket.getReadyState() == ix::ReadyState::Closed) {
        webSocket.start();

        // Wait until WebSocket is connected before fetching the snapshot
        std::unique_lock<std::mutex> lock(wsReadyMutex);
        wsReady.wait(lock, [this] { return wsConnected; });
    }

    // Start dedicated resync worker: waits for the callback signal and
    // performs fetchAndApplySnapshot off the IXWebSocket event loop.
    std::thread([this, &orderBook, maxSnapshotRetries]() {
        while (true) {
            {
                std::unique_lock<std::mutex> lock(resyncMutex);
                resyncCv.wait(lock, [this] { return needsResync; });
                needsResync = false;
            }
            for (int attempt = 1; attempt <= maxSnapshotRetries; ++attempt) {
                printf("Re-sync snapshot (attempt %d/%d)...\n", attempt, maxSnapshotRetries);
                if (fetchAndApplySnapshot(orderBook)) break;
                if (attempt < maxSnapshotRetries) {
                    int delayMs = 1000 * attempt;
                    printf("Re-sync failed, retrying in %d ms...\n", delayMs);
                    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                }
            }
        }
    }).detach();

    // Retry logic for snapshot fetching
    for (int attempt = 1; attempt <= maxSnapshotRetries; ++attempt) {
        printf("Fetching snapshot (attempt %d/%d)...\n", attempt, maxSnapshotRetries);
        if (fetchAndApplySnapshot(orderBook)) {
            return true;
        }
        if (attempt < maxSnapshotRetries) {
            int delayMs = 1000 * attempt;  // simple linear back-off
            printf("Snapshot fetch failed, retrying in %d ms...\n", delayMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
    }

    fprintf(stderr, "Failed to fetch snapshot after %d attempts.\n", maxSnapshotRetries);
    return false;
}
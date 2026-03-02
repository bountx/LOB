#include "feed_handler.hpp"

#include <ixwebsocket/IXHttpClient.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stop_token>
#include <thread>

std::string FeedHandler::streamToSymbol(const std::string& stream) {
    std::string sym = stream.substr(0, stream.find('@'));
    std::transform(sym.begin(), sym.end(), sym.begin(), ::toupper);
    return sym;
}

bool FeedHandler::fetchAndApplySnapshot(const std::string& symbol, OrderBook& orderBook) {
    ix::HttpClient httpClient;
    auto args = std::make_shared<ix::HttpRequestArgs>();
    args->connectTimeout = 5;
    args->transferTimeout = 30;
    const std::string url = "https://api.binance.com/api/v3/depth?symbol=" + symbol + "&limit=5000";
    auto response = httpClient.get(url, args);
    if (response->statusCode != 200) {
        fprintf(stderr, "[%s] Failed to fetch snapshot: HTTP %d\n", symbol.c_str(),
                response->statusCode);
        return false;
    }

    try {
        auto snapshot = nlohmann::json::parse(response->body);
        orderBook.applySnapshot(snapshot);

        std::lock_guard<std::mutex> lock(bufferMutex);
        for (const auto& msg : symbolBuffers[symbol]) {
            if (!orderBook.applyUpdate(msg)) {
                fprintf(stderr, "[%s] Buffered message caused re-sync condition\n", symbol.c_str());
                symbolBuffers[symbol].clear();
                return false;
            }
        }
        symbolBuffers[symbol].clear();
        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "[%s] Snapshot parse/apply error: %s\n", symbol.c_str(), e.what());
        return false;
    }
}

void FeedHandler::handleWsMessage(const ix::WebSocketMessagePtr& msg) {
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
        // Start timing before parse so we capture the full per-message cost.
        const auto start = std::chrono::high_resolution_clock::now();

        // Combo stream wraps each message: {"stream":"btcusdt@depth","data":{...}}
        auto outer = nlohmann::json::parse(msg->str);
        const std::string symbol = streamToSymbol(outer["stream"].get<std::string>());
        const auto& jsonMsg = outer["data"];

        auto booksIt = books->find(symbol);
        auto metricsIt = metricsMap->find(symbol);
        if (booksIt == books->end() || metricsIt == metricsMap->end()) {
            return;  // unknown symbol
        }
        OrderBook& book = *booksIt->second;
        Metrics& metrics = *metricsIt->second;

        // Buffer until snapshot is applied for this symbol
        {
            std::lock_guard<std::mutex> lock(bufferMutex);
            if (!book.isSnapshotApplied()) {
                symbolBuffers[symbol].push_back(jsonMsg);
                return;
            }
        }

        // Event lag metric
        long long eventTime = jsonMsg["E"].get<long long>();
        long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
        metrics.lastEventLagMs.store(now - eventTime);

        if (!book.applyUpdate(jsonMsg)) {
            printf("[%s] Restarting from scratch due to missed updates.\n", symbol.c_str());
            book.clear();
            {
                std::lock_guard<std::mutex> lock(bufferMutex);
                symbolBuffers[symbol].clear();
            }
            {
                std::lock_guard<std::mutex> lock(resyncMutex);
                resyncQueue.push(symbol);
            }
            resyncCv.notify_one();
            return;
        }
        auto end = std::chrono::high_resolution_clock::now();
        long long processingUs =
            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        metrics.lastProcessingUs.store(processingUs);

        long long prevMax = metrics.maxProcessingUs.load();
        while (processingUs > prevMax &&
               !metrics.maxProcessingUs.compare_exchange_weak(prevMax, processingUs)) {
        }

        metrics.msgCount.fetch_add(1);
    } catch (const std::exception& ex) {
        printf("Error processing message: %s\n", ex.what());
    }
}

void FeedHandler::runResyncWorker(int maxSnapshotRetries, std::stop_token stoken) {
    std::stop_callback wake(stoken, [this] { resyncCv.notify_one(); });
    while (!stoken.stop_requested()) {
        std::string symbol;
        {
            std::unique_lock<std::mutex> lock(resyncMutex);
            resyncCv.wait(lock, [&] { return !resyncQueue.empty() || stoken.stop_requested(); });
            if (stoken.stop_requested()) {
                break;
            }
            symbol = resyncQueue.front();
            resyncQueue.pop();
        }
        for (int attempt = 1; attempt <= maxSnapshotRetries; ++attempt) {
            printf("[%s] Re-sync snapshot (attempt %d/%d)...\n", symbol.c_str(), attempt,
                   maxSnapshotRetries);
            if (fetchAndApplySnapshot(symbol, *books->at(symbol))) {
                break;
            }
            if (attempt < maxSnapshotRetries && !stoken.stop_requested()) {
                int delayMs = 1000 * attempt;
                printf("[%s] Re-sync failed, retrying in %d ms...\n", symbol.c_str(), delayMs);
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            }
        }
    }
}

bool FeedHandler::initialize(
    const std::vector<std::string>& symbols,
    std::unordered_map<std::string, std::unique_ptr<OrderBook>>& booksRef, ix::WebSocket& webSocket,
    std::unordered_map<std::string, std::unique_ptr<Metrics>>& metricsMapRef,
    int maxSnapshotRetries) {
    books = &booksRef;
    metricsMap = &metricsMapRef;

    for (auto& [sym, book] : *books) {
        book->clear();
    }
    {
        std::lock_guard<std::mutex> lock(bufferMutex);
        symbolBuffers.clear();
    }
    {
        std::lock_guard<std::mutex> lock(wsReadyMutex);
        wsConnected = false;
    }

    webSocket.setOnMessageCallback(
        [this](const ix::WebSocketMessagePtr& msg) { handleWsMessage(msg); });

    if (webSocket.getReadyState() == ix::ReadyState::Closed) {
        webSocket.start();
        std::unique_lock<std::mutex> lock(wsReadyMutex);
        wsReady.wait(lock, [this] { return wsConnected; });
    }

    resyncThread = std::jthread([this, maxSnapshotRetries](std::stop_token stoken) {
        runResyncWorker(maxSnapshotRetries, stoken);
    });

    for (const auto& symbol : symbols) {
        for (int attempt = 1; attempt <= maxSnapshotRetries; ++attempt) {
            printf("[%s] Fetching snapshot (attempt %d/%d)...\n", symbol.c_str(), attempt,
                   maxSnapshotRetries);
            if (fetchAndApplySnapshot(symbol, *books->at(symbol))) {
                break;
            }
            if (attempt == maxSnapshotRetries) {
                fprintf(stderr, "[%s] Failed to fetch snapshot after %d attempts.\n",
                        symbol.c_str(), maxSnapshotRetries);
                return false;
            }
            int delayMs = 1000 * attempt;
            printf("[%s] Snapshot fetch failed, retrying in %d ms...\n", symbol.c_str(), delayMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
    }
    return true;
}

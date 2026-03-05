#include "feed_handler.hpp"

#include <ixwebsocket/IXHttpClient.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <future>
#include <stop_token>
#include <thread>

namespace {
constexpr std::size_t kMaxBufferedMsgsPerSymbol = 5000;
}

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
    const std::string url = "https://api.binance.com/api/v3/depth?symbol=" + symbol +
                            "&limit=" + std::to_string(snapshotDepth);
    auto response = httpClient.get(url, args);
    if (response->statusCode == 429) {
        int retryAfterSec = 60;
        auto it = response->headers.find("Retry-After");
        if (it != response->headers.end()) {
            try {
                retryAfterSec = std::stoi(it->second);
            } catch (...) {
            }
        }
        fprintf(stderr, "[%s] got 429, waiting %ds before retry\n", symbol.c_str(), retryAfterSec);
        std::this_thread::sleep_for(std::chrono::seconds(retryAfterSec));
        return false;
    }
    if (response->statusCode == 418) {
        fprintf(stderr, "[%s] IP banned (418), waiting 120s\n", symbol.c_str());
        std::this_thread::sleep_for(std::chrono::seconds(120));
        return false;
    }
    if (response->statusCode != 200) {
        fprintf(stderr, "[%s] snapshot fetch failed: HTTP %d\n", symbol.c_str(),
                response->statusCode);
        return false;
    }

    try {
        auto snapshot = nlohmann::json::parse(response->body);
        orderBook.applySnapshot(snapshot);

        std::lock_guard<std::mutex> lock(bufferMutex);
        for (const auto& msg : symbolBuffers[symbol]) {
            if (!orderBook.applyUpdate(msg)) {
                fprintf(stderr, "[%s] buffered message out of order, resyncing\n", symbol.c_str());
                symbolBuffers[symbol].clear();
                return false;
            }
        }
        symbolBuffers[symbol].clear();
        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "[%s] snapshot error: %s\n", symbol.c_str(), e.what());
        return false;
    }
}

void FeedHandler::handleWsMessage(const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) {
        printf("connected\n");
        std::lock_guard<std::mutex> lock(wsReadyMutex);
        wsConnected = true;
        wsReady.notify_one();
        return;
    }
    if (msg->type == ix::WebSocketMessageType::Close) {
        printf("disconnected\n");
        return;
    }
    if (msg->type == ix::WebSocketMessageType::Error) {
        printf("ws error: %s\n", msg->errorInfo.reason.c_str());
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
                if (symbolBuffers[symbol].size() < kMaxBufferedMsgsPerSymbol) {
                    symbolBuffers[symbol].push_back(jsonMsg);
                }
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
            printf("[%s] missed updates, resyncing\n", symbol.c_str());
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
        printf("message error: %s\n", ex.what());
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
            printf("[%s] resyncing (attempt %d/%d)\n", symbol.c_str(), attempt, maxSnapshotRetries);
            if (fetchAndApplySnapshot(symbol, *books->at(symbol))) {
                break;
            }
            if (attempt < maxSnapshotRetries && !stoken.stop_requested()) {
                int delayMs = 1000 * attempt;
                printf("[%s] resync failed, retrying in %dms\n", symbol.c_str(), delayMs);
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            }
        }
        if (!books->at(symbol)->isSnapshotApplied()) {
            fprintf(stderr,
                    "[%s] gave up resyncing after %d attempts, symbol is dead until restart\n",
                    symbol.c_str(), maxSnapshotRetries);
        }
        // Small gap before picking up the next symbol. When several symbols fall out of sync
        // at once (common after a network hiccup), they all land in the queue together, and
        // processing them without any delay would fire REST requests back-to-back.
        if (!stoken.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
}

bool FeedHandler::initialize(
    const std::vector<std::string>& symbols,
    std::unordered_map<std::string, std::unique_ptr<OrderBook>>& booksRef, ix::WebSocket& webSocket,
    std::unordered_map<std::string, std::unique_ptr<Metrics>>& metricsMapRef, int snapshotDepthArg,
    int maxSnapshotRetries) {
    books = &booksRef;
    metricsMap = &metricsMapRef;
    snapshotDepth = snapshotDepthArg;

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
        const bool connected =
            wsReady.wait_for(lock, std::chrono::seconds(30), [this] { return wsConnected; });
        if (!connected) {
            fprintf(stderr, "WebSocket didn't connect within 30s\n");
            return false;
        }
    }

    resyncThread = std::jthread([this, maxSnapshotRetries](std::stop_token stoken) {
        runResyncWorker(maxSnapshotRetries, stoken);
    });

    // Don't fetch all snapshots at once. 30 parallel requests = 300 weight in one burst,
    // which is fine in isolation, but Docker restarts compound this fast enough to earn a 429
    // or 418 ban. A 300ms gap between launches keeps only a handful of requests in-flight at a
    // time.
    std::vector<std::future<bool>> futures;
    futures.reserve(symbols.size());
    for (size_t i = 0; i < symbols.size(); ++i) {
        if (i > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        const auto& symbol = symbols[i];
        futures.push_back(std::async(std::launch::async, [this, symbol, maxSnapshotRetries]() {
            for (int attempt = 1; attempt <= maxSnapshotRetries; ++attempt) {
                printf("[%s] fetching snapshot (attempt %d/%d)\n", symbol.c_str(), attempt,
                       maxSnapshotRetries);
                if (fetchAndApplySnapshot(symbol, *books->at(symbol))) {
                    return true;
                }
                if (attempt == maxSnapshotRetries) {
                    break;
                }
                int delayMs = 1000 * attempt;
                printf("[%s] snapshot fetch failed, retrying in %dms\n", symbol.c_str(), delayMs);
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            }
            fprintf(stderr, "[%s] snapshot fetch gave up after %d attempts\n", symbol.c_str(),
                    maxSnapshotRetries);
            return false;
        }));
    }

    bool allOk = true;
    for (auto& f : futures) {
        if (!f.get()) {
            allOk = false;
        }
    }
    return allOk;
}

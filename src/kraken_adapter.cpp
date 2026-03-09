#include "kraken_adapter.hpp"

#include <chrono>
#include <cstdio>

#include "kraken_utils.hpp"

KrakenAdapter::KrakenAdapter() : normalizer_(defaultNormalizer()) {}

KrakenAdapter::~KrakenAdapter() { stop(); }

/**
 * @brief Stops the WebSocket connection.
 */
void KrakenAdapter::stop() { webSocket_.stop(); }

/**
 * @brief Apply a Kraken book snapshot message for one symbol.
 *
 * Converts Kraken's float price/qty levels to string pairs, builds a
 * Binance-compatible snapshot JSON, and calls OrderBook::applySnapshot.
 * Removes the symbol from the pending-snapshots set when done.
 *
 * @param data One element of the "data" array from a Kraken book snapshot message.
 */
void KrakenAdapter::handleBookSnapshot(const nlohmann::json& data) {
    const std::string krakenSym = data["symbol"].get<std::string>();
    auto canonical = normalizer_.toCanonical("kraken", krakenSym);
    if (!canonical) {
        fprintf(stderr, "[kraken] unknown symbol in snapshot: %s\n", krakenSym.c_str());
        return;
    }

    auto booksIt = books_->find(*canonical);
    if (booksIt == books_->end()) { return; }

    // Build a Binance-compatible snapshot so we can reuse OrderBook::applySnapshot.
    nlohmann::json snap;
    snap["lastUpdateId"] = 0;
    snap["bids"] = kraken::levelsToStringPairs(data["bids"]);
    snap["asks"] = kraken::levelsToStringPairs(data["asks"]);
    booksIt->second->applySnapshot(snap);

    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        pendingSnapshots_.erase(*canonical);
    }
    snapshotCv_.notify_all();
    printf("[kraken] snapshot applied for %s\n", canonical->c_str());
}

/**
 * @brief Apply a Kraken incremental book update for one symbol.
 *
 * Converts the Kraken bid/ask level arrays to string pairs and calls
 * OrderBook::applyDelta. Records processing time, event lag, and fires
 * the update callback.
 *
 * @param data One element of the "data" array from a Kraken book update message.
 */
void KrakenAdapter::handleBookUpdate(const nlohmann::json& data) {
    const std::string krakenSym = data["symbol"].get<std::string>();
    auto canonical = normalizer_.toCanonical("kraken", krakenSym);
    if (!canonical) { return; }

    auto booksIt = books_->find(*canonical);
    auto metricsIt = metricsMap_->find(*canonical);
    if (booksIt == books_->end() || metricsIt == metricsMap_->end()) { return; }

    OrderBook& book = *booksIt->second;
    Metrics& metrics = *metricsIt->second;
    if (!book.isSnapshotApplied()) { return; }

    const auto start = std::chrono::high_resolution_clock::now();

    auto bids = kraken::levelsToStringPairs(data["bids"]);
    auto asks = kraken::levelsToStringPairs(data["asks"]);
    book.applyDelta(bids, asks);

    // Event lag: Kraken update timestamp is ISO 8601 in "timestamp" field.
    long long eventMs = 0;
    if (data.contains("timestamp") && data["timestamp"].is_string()) {
        eventMs = kraken::isoTimestampToMs(data["timestamp"].get<std::string>());
    }
    if (eventMs > 0) {
        long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
        metrics.lastEventLagMs.store(now - eventMs);
    }

    const auto end = std::chrono::high_resolution_clock::now();
    const long long processingUs =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    metrics.recordProcessingUs(processingUs);
    metrics.msgCount.fetch_add(1);

    if (updateCallback_) {
        updateCallback_(exchangeName(), *canonical, bids, asks, eventMs);
    }
}

/**
 * @brief Route an incoming WebSocket message to the appropriate handler.
 *
 * Handles connection lifecycle events and dispatches Kraken book channel
 * "snapshot" and "update" messages to their respective handlers.
 *
 * @param msg Pointer to the WebSocket message.
 */
void KrakenAdapter::handleWsMessage(const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) {
        printf("[kraken] connected\n");
        {
            std::lock_guard<std::mutex> lock(wsReadyMutex_);
            wsConnected_ = true;
        }
        wsReady_.notify_one();
        return;
    }
    if (msg->type == ix::WebSocketMessageType::Close) {
        printf("[kraken] disconnected\n");
        return;
    }
    if (msg->type == ix::WebSocketMessageType::Error) {
        fprintf(stderr, "[kraken] ws error: %s\n", msg->errorInfo.reason.c_str());
        return;
    }
    if (msg->type != ix::WebSocketMessageType::Message) { return; }

    try {
        auto j = nlohmann::json::parse(msg->str);
        if (!j.contains("channel") || !j["channel"].is_string()) { return; }
        if (j["channel"] != "book") { return; }
        if (!j.contains("type") || !j["type"].is_string()) { return; }
        const std::string type = j["type"].get<std::string>();
        if (!j.contains("data") || !j["data"].is_array()) { return; }

        for (const auto& item : j["data"]) {
            if (type == "snapshot") {
                handleBookSnapshot(item);
            } else if (type == "update") {
                handleBookUpdate(item);
            }
        }
    } catch (const std::exception& ex) {
        fprintf(stderr, "[kraken] message error: %s\n", ex.what());
    }
}

/**
 * @brief Connect to Kraken, subscribe to book channel, and wait for initial snapshots.
 *
 * Validates that all canonical symbols have Kraken equivalents, opens the WebSocket,
 * sends a subscribe message, and blocks until all initial snapshots have been received
 * (or a 30-second timeout expires). Returns false on any failure.
 *
 * @param symbols       Canonical symbol list (e.g. "BTC-USDT").
 * @param booksRef      Map of canonical symbol → OrderBook.
 * @param metricsMapRef Map of canonical symbol → Metrics.
 * @param snapshotDepthArg Requested book depth (clamped to nearest supported value).
 * @param maxSnapshotRetries Unused for Kraken (snapshot arrives via WebSocket).
 * @return true if all snapshots received; false on connection or timeout failure.
 */
bool KrakenAdapter::start(
    const std::vector<std::string>& symbols,
    std::unordered_map<std::string, std::unique_ptr<OrderBook>>& booksRef,
    std::unordered_map<std::string, std::unique_ptr<Metrics>>& metricsMapRef,
    int snapshotDepthArg, int /*maxSnapshotRetries*/) {
    // Validate all canonical symbols and build Kraken-specific symbol list.
    std::vector<std::string> krakenSymbols;
    krakenSymbols.reserve(symbols.size());
    for (const auto& canonical : symbols) {
        auto krSym = normalizer_.fromCanonical("kraken", canonical);
        if (!krSym) {
            fprintf(stderr, "[kraken] no Kraken mapping for canonical symbol '%s'\n",
                    canonical.c_str());
            return false;
        }
        if (booksRef.find(canonical) == booksRef.end() ||
            metricsMapRef.find(canonical) == metricsMapRef.end()) {
            fprintf(stderr, "[kraken] symbol '%s' missing from books or metricsMap\n",
                    canonical.c_str());
            return false;
        }
        krakenSymbols.push_back(*krSym);
    }

    books_ = &booksRef;
    metricsMap_ = &metricsMapRef;
    snapshotDepth_ = kraken::clampBookDepth(snapshotDepthArg);

    for (auto& [sym, book] : *books_) {
        book->clear();
    }

    {
        std::lock_guard<std::mutex> lock(wsReadyMutex_);
        wsConnected_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        pendingSnapshots_.clear();
        for (const auto& canonical : symbols) {
            pendingSnapshots_.insert(canonical);
        }
    }

    webSocket_.setUrl("wss://ws.kraken.com/v2");
    webSocket_.setPingInterval(30);
    webSocket_.setOnMessageCallback(
        [this](const ix::WebSocketMessagePtr& msg) { handleWsMessage(msg); });
    webSocket_.start();

    // Wait for WebSocket connection.
    {
        std::unique_lock<std::mutex> lock(wsReadyMutex_);
        const bool connected =
            wsReady_.wait_for(lock, std::chrono::seconds(30), [this] { return wsConnected_; });
        if (!connected) {
            fprintf(stderr, "[kraken] WebSocket didn't connect within 30s\n");
            webSocket_.stop();
            return false;
        }
    }

    // Subscribe to the book channel.
    nlohmann::json subMsg;
    subMsg["method"] = "subscribe";
    subMsg["params"]["channel"] = "book";
    subMsg["params"]["symbol"] = krakenSymbols;
    subMsg["params"]["depth"] = snapshotDepth_;
    webSocket_.send(subMsg.dump());
    printf("[kraken] subscribed to %zu symbol(s) at depth %d\n", symbols.size(), snapshotDepth_);

    // Wait until all per-symbol snapshots have been applied.
    {
        std::unique_lock<std::mutex> lock(snapshotMutex_);
        const bool ok = snapshotCv_.wait_for(lock, std::chrono::seconds(30),
                                              [this] { return pendingSnapshots_.empty(); });
        if (!ok) {
            fprintf(stderr, "[kraken] timed out waiting for book snapshots\n");
            webSocket_.stop();
            return false;
        }
    }
    return true;
}

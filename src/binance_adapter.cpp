#include "binance_adapter.hpp"

#include <ixwebsocket/IXHttpClient.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <stop_token>
#include <thread>

#include "decimal.hpp"

namespace {
constexpr std::size_t kMaxBufferedMsgsPerSymbol = 5000;
}

/**
 * @brief Construct a BinanceAdapter configured for the specified depth update interval.
 *
 * Initializes the adapter's update interval used when subscribing to Binance depth streams.
 *
 * @param updateIntervalMs Milliseconds between depth update snapshots; must be either 100 or 1000.
 * @throws std::invalid_argument if `updateIntervalMs` is not 100 or 1000.
 */
BinanceAdapter::BinanceAdapter(int updateIntervalMs)
    : updateIntervalMs(updateIntervalMs), normalizer_(defaultNormalizer()) {
    if (updateIntervalMs != 100 && updateIntervalMs != 1000) {
        throw std::invalid_argument("BinanceAdapter: updateIntervalMs must be 100 or 1000, got " +
                                    std::to_string(updateIntervalMs));
    }
}

/**
 * @brief Cleans up the adapter, ensuring background workers and connections are stopped.
 *
 * Ensures the resync worker is requested to stop, the resync condition variable is
 * notified, the resync thread is joined/reset, and the WebSocket client is stopped.
 */
BinanceAdapter::~BinanceAdapter() { stop(); }

/**
 * @brief Stops background processing and shuts down the WebSocket connection.
 *
 * Requests cancellation of the resync worker, wakes the resync condition variable,
 * joins and destroys the resync thread, and stops the WebSocket client.
 */
void BinanceAdapter::stop() {
    watchdogThread_ = {};  // request_stop() + join before closing the socket
    resyncThread.request_stop();
    resyncCv.notify_one();
    resyncThread = {};  // join + destroy
    webSocket.stop();
}

/**
 * @brief Fetches a REST order-book snapshot from Binance for a symbol and applies it to an
 * OrderBook.
 *
 * Attempts to download the snapshot for binanceSym, apply it to orderBook, and then replay any
 * pre-snapshot buffered websocket updates for the same canonical symbol atomically. If the HTTP
 * response indicates rate limiting or an IP ban the function waits (respecting stoken) for the
 * server-recommended or a default interval and returns `false`.
 *
 * @param binanceSym Binance-format symbol used in the REST request (e.g., "BTCUSDT").
 * @param canonical Canonical/internal symbol corresponding to binanceSym (used for buffering/logs).
 * @param orderBook OrderBook instance that will receive the snapshot and replayed updates.
 * @param stoken Stop token that interrupts waits caused by rate limiting or IP ban handling.
 * @return `true` if the snapshot was applied and all buffered updates were successfully replayed;
 * `false` otherwise.
 */
bool BinanceAdapter::fetchAndApplySnapshot(const std::string& binanceSym,
                                           const std::string& canonical, OrderBook& orderBook,
                                           std::stop_token stoken) {
    ix::HttpClient httpClient;
    auto args = std::make_shared<ix::HttpRequestArgs>();
    args->connectTimeout = 5;
    args->transferTimeout = 30;
    const std::string url = "https://api.binance.com/api/v3/depth?symbol=" + binanceSym +
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
        fprintf(stderr, "[%s] got 429, waiting %ds before retry\n", canonical.c_str(),
                retryAfterSec);
        std::unique_lock<std::mutex> lock(resyncMutex);
        resyncCv.wait_for(lock, stoken, std::chrono::seconds(retryAfterSec), [] { return false; });
        return false;
    }
    if (response->statusCode == 418) {
        fprintf(stderr, "[%s] IP banned (418), waiting 120s\n", canonical.c_str());
        std::unique_lock<std::mutex> lock(resyncMutex);
        resyncCv.wait_for(lock, stoken, std::chrono::seconds(120), [] { return false; });
        return false;
    }
    if (response->statusCode != 200) {
        fprintf(stderr, "[%s] snapshot fetch failed: HTTP %d\n", canonical.c_str(),
                response->statusCode);
        return false;
    }

    try {
        auto snapshot = nlohmann::json::parse(response->body);

        // Hold bufferMutex across applySnapshot + buffer replay so that
        // handleWsMessage cannot observe isSnapshotApplied()==true and apply
        // a live update while we are still draining the pre-snapshot buffer.
        std::lock_guard<std::mutex> lock(bufferMutex);
        orderBook.applySnapshot(snapshot);
        for (const auto& msg : symbolBuffers[canonical]) {
            if (!orderBook.applyUpdate(msg, EventKind::Backfill).success) {
                fprintf(stderr, "[%s] buffered message out of order, resyncing\n",
                        canonical.c_str());
                symbolBuffers[canonical].clear();
                return false;
            }
        }
        symbolBuffers[canonical].clear();
        auto mit = metricsMap->find(canonical);
        if (mit != metricsMap->end()) {
            mit->second->liveReady.store(true, std::memory_order_release);
        }
        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "[%s] snapshot error: %s\n", canonical.c_str(), e.what());
        return false;
    }
}

/**
 * @brief Process an incoming WebSocket event from Binance and update state or metrics accordingly.
 *
 * Handles connection lifecycle events (Open/Close/Error) and parses combo-stream messages for
 * order-book updates. While a symbol's initial snapshot is missing, messages are buffered. After
 * snapshot application, updates are applied to the corresponding OrderBook; on update failures the
 * symbol is scheduled for resynchronization and its buffer is cleared. Processing time and event
 * lag are recorded in the associated Metrics.
 *
 * @param msg Pointer to the WebSocket message; for data messages it is expected to contain a
 * Binance combo stream JSON object with fields `"stream"` and `"data"`.
 */
void BinanceAdapter::handleWsMessage(const ix::WebSocketMessagePtr& msg) {
    // Refresh the connection-level activity timestamp on any live traffic so the
    // watchdog can distinguish a dead socket from a legitimately quiet symbol.
    if (msg->type == ix::WebSocketMessageType::Open ||
        msg->type == ix::WebSocketMessageType::Message) {
        lastConnectionActivityMs_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count(),
                                        std::memory_order_relaxed);
    }
    if (msg->type == ix::WebSocketMessageType::Open) {
        bool isReconnect = false;
        {
            std::lock_guard<std::mutex> lock(wsReadyMutex);
            isReconnect = wsConnected;
            wsConnected = true;
        }
        if (!isReconnect) {
            printf("[binance] connected\n");
            wsReady.notify_one();
            return;
        }

        // Reconnect path: discard stale buffers + books, reset sentinels, trigger resync.
        printf("[binance] reconnected — resyncing %zu symbol(s)\n", subscribedSymbols_.size());
        {
            std::lock_guard<std::mutex> lock(bufferMutex);
            for (const auto& sym : subscribedSymbols_) {
                symbolBuffers[sym].clear();
                auto bit = books->find(sym);
                if (bit != books->end()) bit->second->clear();
            }
        }
        for (const auto& sym : subscribedSymbols_) {
            auto mit = metricsMap->find(sym);
            if (mit != metricsMap->end()) {
                mit->second->lastUpdateTimeMs.store(0, std::memory_order_relaxed);
                mit->second->ofiAccumulator.store(0, std::memory_order_relaxed);
                mit->second->liveReady.store(false, std::memory_order_relaxed);
            }
        }
        {
            std::lock_guard<std::mutex> lock(resyncMutex);
            for (const auto& sym : subscribedSymbols_) resyncQueue.push(sym);
        }
        resyncCv.notify_one();
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
        // --- symbol identification (needed on every path) ---
        // Use simdjson to extract the stream name without building a DOM.
        // padded_string copies msg->str with SIMDJSON_PADDING suffix so the
        // SIMD parser can read past the payload safely.
        const auto start = std::chrono::high_resolution_clock::now();
        simdjson::padded_string padded(msg->str);
        auto doc = simdParser_.iterate(padded);

        std::string_view stream_sv = doc.find_field_unordered("stream").get_string().value();
        // Inline streamToSymbol: prefix before '@', uppercased.
        const auto at_pos = stream_sv.find('@');
        std::string binanceSym(stream_sv.substr(0, at_pos));
        std::transform(binanceSym.begin(), binanceSym.end(), binanceSym.begin(),
                       [](unsigned char c) { return ::toupper(c); });

        auto canonIt = binanceToCanonical_.find(binanceSym);
        if (canonIt == binanceToCanonical_.end()) {
            return;  // unknown symbol
        }
        const std::string& sym = canonIt->second;

        auto booksIt = books->find(sym);
        auto metricsIt = metricsMap->find(sym);
        if (booksIt == books->end() || metricsIt == metricsMap->end()) {
            return;
        }
        OrderBook& book = *booksIt->second;
        Metrics& metrics = *metricsIt->second;

        // --- B: fast path — snapshot fully live, skip bufferMutex entirely ---
        if (!metrics.liveReady.load(std::memory_order_acquire)) {
            // Slow path: startup / resync window.
            // Parse full message with nlohmann for buffering (cold path).
            auto outer = nlohmann::json::parse(msg->str);
            const auto& jsonMsg = outer["data"];
            {
                std::lock_guard<std::mutex> lock(bufferMutex);
                if (!book.isSnapshotApplied()) {
                    if (symbolBuffers[sym].size() < kMaxBufferedMsgsPerSymbol) {
                        symbolBuffers[sym].push_back(jsonMsg);
                    }
                    return;
                }
                // Snapshot applied and buffer drained (we hold bufferMutex; fetchAndApplySnapshot
                // already released it).  Mark this symbol live so future messages skip this path.
                metrics.liveReady.store(true, std::memory_order_release);
            }
            // Process this first live update via the nlohmann path.
            const long long eventTime = jsonMsg["E"].get<long long>();
            const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count();
            metrics.lastEventLagMs.store(now - eventTime);
            auto result = book.applyUpdate(jsonMsg, EventKind::Genuine);
            if (!result.success) {
                printf("[%s] missed updates, resyncing\n", sym.c_str());
                book.clear();
                metrics.lastUpdateTimeMs.store(0, std::memory_order_relaxed);
                metrics.ofiAccumulator.store(0, std::memory_order_relaxed);
                metrics.liveReady.store(false, std::memory_order_release);
                {
                    std::lock_guard<std::mutex> lock(bufferMutex);
                    symbolBuffers[sym].clear();
                }
                {
                    std::lock_guard<std::mutex> lock(resyncMutex);
                    resyncQueue.push(sym);
                }
                resyncCv.notify_one();
                return;
            }
            const auto end = std::chrono::high_resolution_clock::now();
            metrics.recordProcessingUs(
                std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
            metrics.msgCount.fetch_add(1);
            metrics.lastUpdateTimeMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count(),
                                           std::memory_order_relaxed);
            if (updateCallback_) {
                updateCallback_(exchangeName(), sym, result.deltas, eventTime);
            }
            return;
        }

        // --- A: live fast path — simdjson, no heap allocation, no lock ---
        auto data = doc.find_field_unordered("data").get_object().value();

        const long long eventTime =
            static_cast<long long>(data.find_field_unordered("E").get_int64().value());
        const long long firstId = static_cast<long long>(data.find_field_unordered("U").get_int64().value());
        const long long lastId = static_cast<long long>(data.find_field_unordered("u").get_int64().value());

        // Extract bid and ask levels into thread-local pre-allocated vectors.
        // string_view elements point into `padded` which lives for the whole scope.
        thread_local std::vector<std::pair<long long, long long>> tlBids, tlAsks;
        tlBids.clear();
        tlAsks.clear();

        for (auto bid_level : data.find_field_unordered("b").get_array()) {
            auto arr = bid_level.get_array().value();
            auto it = arr.begin();
            if (it == arr.end()) continue;  // skip malformed
            const std::string_view price_sv = (*it).get_string().value();
            ++it;
            if (it == arr.end()) continue;  // skip malformed
            const std::string_view qty_sv = (*it).get_string().value();
            tlBids.push_back({parseDecimal(price_sv), parseDecimal(qty_sv)});
        }
        for (auto ask_level : data.find_field_unordered("a").get_array()) {
            auto arr = ask_level.get_array().value();
            auto it = arr.begin();
            if (it == arr.end()) continue;  // skip malformed
            const std::string_view price_sv = (*it).get_string().value();
            ++it;
            if (it == arr.end()) continue;  // skip malformed
            const std::string_view qty_sv = (*it).get_string().value();
            tlAsks.push_back({parseDecimal(price_sv), parseDecimal(qty_sv)});
        }

        const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
        metrics.lastEventLagMs.store(now - eventTime);

        auto result = book.applyUpdate(firstId, lastId, tlAsks, tlBids, EventKind::Genuine);

        if (!result.success) {
            printf("[%s] missed updates, resyncing\n", sym.c_str());
            book.clear();
            metrics.lastUpdateTimeMs.store(0, std::memory_order_relaxed);
            metrics.ofiAccumulator.store(0, std::memory_order_relaxed);
            metrics.liveReady.store(false, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(bufferMutex);
                symbolBuffers[sym].clear();
            }
            {
                std::lock_guard<std::mutex> lock(resyncMutex);
                resyncQueue.push(sym);
            }
            resyncCv.notify_one();
            return;
        }

        const auto end = std::chrono::high_resolution_clock::now();
        metrics.recordProcessingUs(
            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
        metrics.msgCount.fetch_add(1);
        metrics.lastUpdateTimeMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now().time_since_epoch())
                                           .count(),
                                       std::memory_order_relaxed);

        if (updateCallback_) {
            updateCallback_(exchangeName(), sym, result.deltas, eventTime);
        }
    } catch (const std::exception& ex) {
        printf("message error: %s\n", ex.what());
    }
}

/**
 * @brief Background worker that processes symbols needing resynchronization.
 *
 * Waits for symbols enqueued for resync, then attempts up to `maxSnapshotRetries`
 * to fetch and apply a fresh snapshot for each symbol. Retries use an increasing
 * delay (1000ms * attempt) between attempts, and a 500ms pause is added after
 * each symbol to avoid bursting REST requests. The worker wakes and exits when
 * `stoken` requests stop.
 *
 * @param maxSnapshotRetries Maximum number of snapshot fetch attempts per symbol.
 * @param stoken Stop token used to interrupt waiting, wake the worker, and
 *               request graceful shutdown.
 */
void BinanceAdapter::runResyncWorker(int maxSnapshotRetries, std::stop_token stoken) {
    while (true) {
        std::string symbol;
        {
            std::unique_lock<std::mutex> lock(resyncMutex);
            // wait() with stop_token: returns false immediately if stop is requested.
            if (!resyncCv.wait(lock, stoken, [&] { return !resyncQueue.empty(); })) {
                break;
            }
            symbol = resyncQueue.front();
            resyncQueue.pop();
        }
        // symbol here is canonical (e.g. "BTC-USDT"); convert to Binance for REST URL.
        auto binanceSym = normalizer_.fromCanonical("binance", symbol).value_or(symbol);
        for (int attempt = 1; attempt <= maxSnapshotRetries; ++attempt) {
            printf("[%s] resyncing (attempt %d/%d)\n", symbol.c_str(), attempt, maxSnapshotRetries);
            if (fetchAndApplySnapshot(binanceSym, symbol, *books->at(symbol), stoken)) {
                break;
            }
            if (stoken.stop_requested()) {
                return;
            }
            if (attempt < maxSnapshotRetries) {
                int delayMs = 1000 * attempt;
                printf("[%s] resync failed, retrying in %dms\n", symbol.c_str(), delayMs);
                std::unique_lock<std::mutex> lock(resyncMutex);
                resyncCv.wait_for(lock, stoken, std::chrono::milliseconds(delayMs),
                                  [] { return false; });
                if (stoken.stop_requested()) {
                    return;
                }
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
        {
            std::unique_lock<std::mutex> lock(resyncMutex);
            resyncCv.wait_for(lock, stoken, std::chrono::milliseconds(500), [] { return false; });
        }
        if (stoken.stop_requested()) {
            return;
        }
    }
}

/**
 * @brief Initialize the adapter for the given symbols, open the Binance combined WebSocket,
 * start background workers, and obtain initial REST snapshots for each symbol.
 *
 * Configures internal references to the provided OrderBook and Metrics maps, subscribes to the
 * specified canonical symbols, starts the resynchronization and watchdog workers, and concurrently
 * fetches and applies each symbol's initial REST order book snapshot with retry and backoff.
 *
 * @param symbols List of canonical symbol names to subscribe (e.g., "BTC-USDT").
 * @param booksRef Map from canonical symbol to owned OrderBook instances; must contain every entry in `symbols`.
 * @param metricsMapRef Map from canonical symbol to owned Metrics instances; must contain every entry in `symbols`.
 * @param snapshotDepthArg Depth to request for each REST order book snapshot.
 * @param maxSnapshotRetries Maximum attempts to fetch and apply each initial snapshot.
 * @return true if the WebSocket connected and all initial snapshots were successfully applied; `false` if validation failed, the WebSocket failed to connect, or any snapshot failed.
 */
bool BinanceAdapter::start(const std::vector<std::string>& symbols,
                           std::unordered_map<std::string, std::unique_ptr<OrderBook>>& booksRef,
                           std::unordered_map<std::string, std::unique_ptr<Metrics>>& metricsMapRef,
                           int snapshotDepthArg, int maxSnapshotRetries) {
    // symbols are in canonical BASE-QUOTE format (e.g. "BTC-USDT").
    // Build binanceToCanonical_ map and validate all symbols have book/metrics entries.
    binanceToCanonical_.clear();
    for (const auto& canonical : symbols) {
        if (booksRef.find(canonical) == booksRef.end() ||
            metricsMapRef.find(canonical) == metricsMapRef.end()) {
            fprintf(stderr, "BinanceAdapter::start: symbol '%s' missing from books or metricsMap\n",
                    canonical.c_str());
            return false;
        }
        // fromCanonical("binance", "BTC-USDT") → "BTCUSDT" via auto-rule (always succeeds).
        std::string binanceSym =
            normalizer_.fromCanonical("binance", canonical).value_or(canonical);
        binanceToCanonical_[binanceSym] = canonical;
    }

    books = &booksRef;
    metricsMap = &metricsMapRef;
    snapshotDepth = snapshotDepthArg;
    subscribedSymbols_ = symbols;

    for (auto& [sym, book] : *books) {
        book->clear();
    }
    // Reset data-age sentinels so the watchdog doesn't fire before first snapshot.
    for (const auto& sym : symbols) {
        metricsMapRef.at(sym)->lastUpdateTimeMs.store(0, std::memory_order_relaxed);
        metricsMapRef.at(sym)->liveReady.store(false, std::memory_order_relaxed);
    }
    lastConnectionActivityMs_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(bufferMutex);
        symbolBuffers.clear();
    }
    {
        std::lock_guard<std::mutex> lock(wsReadyMutex);
        wsConnected = false;
    }

    // Build the combo stream URL using Binance-specific symbols (lowercase).
    std::string urlStreams;
    for (const auto& canonical : symbols) {
        std::string binanceSym =
            normalizer_.fromCanonical("binance", canonical).value_or(canonical);
        std::string lower = binanceSym;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (!urlStreams.empty()) urlStreams += "/";
        urlStreams += lower + "@depth@" + std::to_string(updateIntervalMs) + "ms";
    }
    webSocket.setUrl("wss://stream.binance.com:9443/stream?streams=" + urlStreams);
    // Send a ping every 30s so half-open TCP connections (e.g. Binance's 24h
    // forced disconnect that sends no close frame) are detected and trigger
    // IXWebSocket's built-in reconnect before we silently starve.
    webSocket.setPingInterval(30);

    webSocket.setOnMessageCallback(
        [this](const ix::WebSocketMessagePtr& msg) { handleWsMessage(msg); });

    if (webSocket.getReadyState() == ix::ReadyState::Closed) {
        webSocket.start();
        std::unique_lock<std::mutex> lock(wsReadyMutex);
        const bool connected =
            wsReady.wait_for(lock, std::chrono::seconds(30), [this] { return wsConnected; });
        if (!connected) {
            fprintf(stderr, "WebSocket didn't connect within 30s\n");
            lock.unlock();
            webSocket.stop();
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
    std::stop_token startStoken = resyncThread.get_stop_token();
    std::vector<std::future<bool>> futures;
    futures.reserve(symbols.size());
    for (size_t i = 0; i < symbols.size(); ++i) {
        if (i > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        const std::string& canonical = symbols[i];
        // Convert canonical → Binance-specific for the REST snapshot URL.
        std::string binanceSym =
            normalizer_.fromCanonical("binance", canonical).value_or(canonical);
        futures.push_back(std::async(
            std::launch::async, [this, canonical, binanceSym, maxSnapshotRetries, startStoken]() {
                for (int attempt = 1; attempt <= maxSnapshotRetries; ++attempt) {
                    printf("[%s] fetching snapshot (attempt %d/%d)\n", canonical.c_str(), attempt,
                           maxSnapshotRetries);
                    if (fetchAndApplySnapshot(binanceSym, canonical, *books->at(canonical),
                                              startStoken)) {
                        return true;
                    }
                    if (startStoken.stop_requested() || attempt == maxSnapshotRetries) {
                        break;
                    }
                    int delayMs = 1000 * attempt;
                    printf("[%s] snapshot fetch failed, retrying in %dms\n", canonical.c_str(),
                           delayMs);
                    std::unique_lock<std::mutex> lock(resyncMutex);
                    resyncCv.wait_for(lock, startStoken, std::chrono::milliseconds(delayMs),
                                      [] { return false; });
                }
                fprintf(stderr, "[%s] snapshot fetch gave up after %d attempts\n",
                        canonical.c_str(), maxSnapshotRetries);
                return false;
            }));
    }

    bool allOk = true;
    for (auto& f : futures) {
        if (!f.get()) {
            allOk = false;
        }
    }
    if (!allOk) {
        stop();
        return false;
    }
    // Start the watchdog only after all initial snapshots have succeeded so it
    // doesn't run during the startup window or leak a thread on failure paths.
    watchdogThread_ = std::jthread([this](std::stop_token st) { runWatchdog(st); });
    return true;
}

/**
 * @brief Watchdog thread: detects a silently stalled Binance feed and forces a reconnect.
 *
 * Checks every 500 ms whether any WebSocket message has arrived in the last 2 s.
 * Uses a connection-level timestamp (lastConnectionActivityMs_) updated on every Open
 * or data frame, so legitimately quiet symbols do not trigger false reconnects.
 * lastConnectionActivityMs_ == 0 means no connection has been established yet; the
 * watchdog stays silent until traffic has been seen at least once.
 */
void BinanceAdapter::runWatchdog(std::stop_token stoken) {
    constexpr long long kStaleThresholdMs = 2000;

    while (!stoken.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (stoken.stop_requested()) break;

        const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();

        const long long last = lastConnectionActivityMs_.load(std::memory_order_relaxed);
        if (last == 0) continue;  // not yet connected — skip
        if ((now - last) > kStaleThresholdMs) {
            fprintf(stderr, "[binance] stale connection: no data for %lld ms, forcing reconnect\n",
                    now - last);
            // Zero the sentinel before restarting so the next watchdog tick skips
            // until the Open handler refreshes lastConnectionActivityMs_.
            lastConnectionActivityMs_.store(0, std::memory_order_relaxed);
            webSocket.stop();
            webSocket.start();
        }
    }
}

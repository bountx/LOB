#pragma once
#include <ixwebsocket/IXWebSocket.h>
#include <simdjson.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <queue>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "binance_utils.hpp"
#include "i_exchange_adapter.hpp"
#include "metrics.hpp"
#include "order_book.hpp"
#include "symbol_normalizer.hpp"

class BinanceAdapter : public IExchangeAdapter {
public:
    // updateIntervalMs must be 100 or 1000, matching the Binance stream suffix (@depth@100ms).
    explicit BinanceAdapter(int updateIntervalMs = 100);
    ~BinanceAdapter() override;

    /**
     * @brief Identifies the exchange implementation.
     *
     * @return std::string_view Exchange identifier "binance".
     */
    std::string_view exchangeName() const override { return "binance"; }

    // Builds the WebSocket URL from symbols, connects, fetches snapshots, and starts streaming.
    // Returns false if the WebSocket doesn't connect or any snapshot can't be fetched.
    bool start(const std::vector<std::string>& symbols,
               std::unordered_map<std::string, std::unique_ptr<OrderBook>>& books,
               std::unordered_map<std::string, std::unique_ptr<Metrics>>& metricsMap,
               int snapshotDepth = 1000, int maxSnapshotRetries = 5) override;

    void stop() override;

private:
    ix::WebSocket webSocket;
    int updateIntervalMs;
    SymbolNormalizer normalizer_;
    // Maps Binance-specific stream symbol (e.g. "BTCUSDT") → canonical (e.g. "BTC-USDT").
    // Populated in start() from the canonical symbols list.
    std::unordered_map<std::string, std::string> binanceToCanonical_;

    std::unordered_map<std::string, std::unique_ptr<OrderBook>>* books = nullptr;
    std::unordered_map<std::string, std::unique_ptr<Metrics>>* metricsMap = nullptr;

    std::unordered_map<std::string, std::vector<nlohmann::json>> symbolBuffers;
    std::mutex bufferMutex;

    std::queue<std::string> resyncQueue;
    std::mutex resyncMutex;
    std::condition_variable_any resyncCv;

    std::mutex wsReadyMutex;
    std::condition_variable wsReady;
    bool wsConnected = false;

    std::jthread resyncThread;
    std::jthread watchdogThread_;

    // Steady-clock ms of the last received WebSocket message (Open or data frame).
    // 0 = not yet connected. Used by the watchdog for connection-level stall detection.
    std::atomic<long long> lastConnectionActivityMs_{0};

    int snapshotDepth = 1000;
    std::vector<std::string> subscribedSymbols_;  // canonical symbols; for reconnect + watchdog

    // Fetches a depth snapshot from the Binance REST API and applies it to the order book.
    // binanceSym is the exchange-specific symbol used in the REST URL (e.g. "BTCUSDT").
    // canonical is the canonical key used for buffer lookup (e.g. "BTC-USDT").
    // On 429 (rate limited) or 418 (IP banned), sleeps before returning false.
    bool fetchAndApplySnapshot(const std::string& binanceSym, const std::string& canonical,
                               OrderBook& orderBook, std::stop_token stoken);

    void handleWsMessage(const ix::WebSocketMessagePtr& msg);

    // Pulls symbols off the resync queue and re-fetches their snapshots, one at a time.
    // Runs on its own thread until stoken fires.
    void runResyncWorker(int maxSnapshotRetries, std::stop_token stoken);

    // Detects a silently stalled feed and forces a reconnect via webSocket.stop().
    void runWatchdog(std::stop_token stoken);

    simdjson::ondemand::parser simdParser_;
};

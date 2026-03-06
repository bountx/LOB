#pragma once
#include <ixwebsocket/IXWebSocket.h>

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

    int snapshotDepth = 1000;

    // Fetches a depth snapshot from the Binance REST API and applies it to the order book.
    // On 429 (rate limited) or 418 (IP banned), sleeps before returning false so the caller
    // doesn't immediately retry and dig the hole deeper. The sleep is interruptible via stoken.
    bool fetchAndApplySnapshot(const std::string& symbol, OrderBook& orderBook,
                               std::stop_token stoken);

    void handleWsMessage(const ix::WebSocketMessagePtr& msg);

    // Pulls symbols off the resync queue and re-fetches their snapshots, one at a time.
    // Runs on its own thread until stoken fires.
    void runResyncWorker(int maxSnapshotRetries, std::stop_token stoken);
};

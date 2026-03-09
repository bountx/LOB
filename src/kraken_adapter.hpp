#pragma once
#include <ixwebsocket/IXWebSocket.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "i_exchange_adapter.hpp"
#include "metrics.hpp"
#include "order_book.hpp"
#include "symbol_normalizer.hpp"

class KrakenAdapter : public IExchangeAdapter {
public:
    KrakenAdapter();
    ~KrakenAdapter() override;

    /**
     * @brief Identifies the exchange implementation.
     *
     * @return std::string_view Exchange identifier "kraken".
     */
    std::string_view exchangeName() const override { return "kraken"; }

    // Connects to wss://ws.kraken.com/v2, subscribes to the book channel for each symbol,
    // and blocks until the initial snapshot has been received for every symbol (or 30s timeout).
    // symbols must be in canonical BASE-QUOTE format (e.g. "BTC-USDT").
    // Returns false if the WebSocket fails to connect or any snapshot times out.
    bool start(const std::vector<std::string>& symbols,
               std::unordered_map<std::string, std::unique_ptr<OrderBook>>& books,
               std::unordered_map<std::string, std::unique_ptr<Metrics>>& metricsMap,
               int snapshotDepth = 1000, int maxSnapshotRetries = 5) override;

    void stop() override;

private:
    ix::WebSocket webSocket_;
    SymbolNormalizer normalizer_;

    std::unordered_map<std::string, std::unique_ptr<OrderBook>>* books_ = nullptr;
    std::unordered_map<std::string, std::unique_ptr<Metrics>>* metricsMap_ = nullptr;

    std::mutex wsReadyMutex_;
    std::condition_variable wsReady_;
    bool wsConnected_ = false;

    // Canonical symbols whose initial snapshots have not yet arrived.
    std::mutex snapshotMutex_;
    std::unordered_set<std::string> pendingSnapshots_;
    std::condition_variable snapshotCv_;

    int snapshotDepth_ = 1000;
    bool subscribeError_ = false;
    std::vector<std::string> krakenSymbols_;      // Kraken-format symbols, for re-subscription
    std::vector<std::string> subscribedSymbols_;  // canonical symbols actually subscribed

    std::jthread watchdogThread_;

    void handleWsMessage(const ix::WebSocketMessagePtr& msg);
    void handleBookSnapshot(const nlohmann::json& data);
    void handleBookUpdate(const nlohmann::json& data);
    void runWatchdog(std::stop_token stoken);
};

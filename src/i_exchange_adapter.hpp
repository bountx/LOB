#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "metrics.hpp"
#include "order_book.hpp"

// Interface for exchange feed adapters.
// Each adapter owns its WebSocket connection, snapshot logic, and rate-limit handling.
class IExchangeAdapter {
public:
    virtual ~IExchangeAdapter() = default;

    // Returns the exchange name (e.g. "binance", "kraken").
    virtual std::string_view exchangeName() const = 0;

    // Connects to the exchange, fetches initial snapshots for all symbols,
    // and begins streaming live updates into books and metricsMap.
    // Returns false if the connection or any snapshot fails.
    virtual bool start(const std::vector<std::string>& symbols,
                       std::unordered_map<std::string, std::unique_ptr<OrderBook>>& books,
                       std::unordered_map<std::string, std::unique_ptr<Metrics>>& metricsMap,
                       int snapshotDepth = 1000, int maxSnapshotRetries = 5) = 0;

    // Stops the WebSocket connection and all background threads.
    virtual void stop() = 0;

    IExchangeAdapter(const IExchangeAdapter&) = delete;
    IExchangeAdapter& operator=(const IExchangeAdapter&) = delete;
    IExchangeAdapter(IExchangeAdapter&&) = delete;
    IExchangeAdapter& operator=(IExchangeAdapter&&) = delete;

protected:
    IExchangeAdapter() = default;
};

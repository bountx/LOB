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
    /**
 * @brief Ensures derived adapters are destroyed correctly when deleted via the interface.
 *
 * Guarantees that deleting an IExchangeAdapter pointer calls the concrete adapter's destructor
 * so derived resources (connections, threads, etc.) are released properly.
 */
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

    /**
 * @brief Deleted copy constructor to prevent copying of adapter instances.
 *
 * Ensures that exchange adapter objects (and their derived types) cannot be copy-constructed, enforcing unique ownership of connection and thread resources.
 */
IExchangeAdapter(const IExchangeAdapter&) = delete;
    /**
 * @brief Deleted copy assignment operator to prevent copying of adapters.
 *
 * Adapter instances cannot be copy-assigned; ownership and resources must be managed by the concrete implementation.
 */
IExchangeAdapter& operator=(const IExchangeAdapter&) = delete;
    /**
 * @brief Deleted move constructor to prevent moving an adapter instance.
 *
 * Ensures concrete adapter implementations are not movable so ownership of
 * connections, threads, and other resources remains with the original instance.
 */
IExchangeAdapter(IExchangeAdapter&&) = delete;
    /**
 * @brief Deleted move-assignment operator to prevent move-assignment of adapters.
 *
 * Prevents transferring ownership of adapter resources (connections, threads, etc.)
 * via move-assignment; implementations must manage lifecycle without relying on move
 * semantics.
 */
IExchangeAdapter& operator=(IExchangeAdapter&&) = delete;

protected:
    /**
 * @brief Protected default constructor.
 *
 * Allows derived exchange adapter implementations to construct the base interface subobject.
 */
IExchangeAdapter() = default;
};

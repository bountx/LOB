#pragma once
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "metrics.hpp"
#include "ofi_types.hpp"
#include "order_book.hpp"

// Interface for exchange feed adapters.
// Each adapter owns its WebSocket connection, snapshot logic, and rate-limit handling.
class IExchangeAdapter {
public:
    // Called after each successful order book update.
    // exchange: adapter name (e.g. "binance"), symbol: e.g. "BTCUSDT".
    // deltas: mixed-kind list of price-level changes from this update. Consumers MUST inspect
    //   each LevelDelta::kind (Genuine vs Backfill) and both view flags:
    //   - wasInView: the level was in the OFI view before this update
    //   - inOfiView: the level is in the OFI view after this update
    //   Use (wasInView || inOfiView) to correctly capture entering, in-flight, and removed levels.
    //   Never assume all deltas are exchange-originated live price changes.
    // ts: event timestamp in milliseconds since epoch.
    using UpdateCallback = std::function<void(std::string_view exchange, std::string_view symbol,
                                              const std::vector<LevelDelta>& deltas, long long ts)>;

    // Register a callback invoked after each successfully applied book update.
    // Must be called before start(). Thread-safe: the callback is invoked from the
    /**
     * @brief Register a callback to be invoked after each successfully applied order-book update.
     *
     * The callback is stored and may be invoked from the adapter's internal message-processing
     * thread; the provided function must be safe to call from that thread context.
     *
     * @param cb Callback with signature `void(std::string_view exchange, std::string_view symbol,
     * const std::vector<LevelDelta>& deltas, long long ts)`; it is called after each applied
     * update with the exchange name, symbol, the list of level deltas, and the update timestamp.
     */
    void setUpdateCallback(UpdateCallback cb) { updateCallback_ = std::move(cb); }

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
     * Ensures that exchange adapter objects (and their derived types) cannot be copy-constructed,
     * enforcing unique ownership of connection and thread resources.
     */
    IExchangeAdapter(const IExchangeAdapter&) = delete;
    /**
     * @brief Deleted copy assignment operator to prevent copying of adapters.
     *
     * Adapter instances cannot be copy-assigned; ownership and resources must be managed by the
     * concrete implementation.
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

    UpdateCallback updateCallback_;
};

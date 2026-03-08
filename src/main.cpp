#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "feed_handler.hpp"
#include "i_exchange_adapter.hpp"
#include "kraken_adapter.hpp"
#include "metrics.hpp"
#include "metrics_server.hpp"
#include "order_book.hpp"
#include "subscriber_server.hpp"

/**
 * @brief Program entry point: loads configuration, initialises order books, metrics, the
 * exchange adapter and servers, then runs the monitoring loop.
 *
 * Accepts an optional first command-line argument as the path to the JSON configuration file;
 * defaults to "config.json". Symbols must be in canonical BASE-QUOTE format (e.g. "BTC-USDT").
 * The "exchange" key selects "binance" (default) or "kraken".
 *
 * @param argc Number of command-line arguments.
 * @param argv argv[1] may provide the config file path.
 * @return int 0 on normal exit (unreachable), -1 on startup error.
 */
int main(int argc, char* argv[]) {
    const char* configPath = (argc > 1) ? argv[1] : "config.json";

    nlohmann::json config;
    try {
        std::ifstream f(configPath);
        if (!f.is_open()) {
            fprintf(stderr, "can't open config: %s\n", configPath);
            return -1;
        }
        config = nlohmann::json::parse(f);
    } catch (const std::exception& e) {
        fprintf(stderr, "config parse error: %s\n", e.what());
        return -1;
    }

    // ─── Exchange selection ───────────────────────────────────────────────────
    std::string exchange = "binance";
    if (config.contains("exchange")) {
        if (!config["exchange"].is_string()) {
            fprintf(stderr, "config error: 'exchange' must be a string\n");
            return -1;
        }
        exchange = config["exchange"].get<std::string>();
        if (exchange != "binance" && exchange != "kraken") {
            fprintf(stderr, "config error: 'exchange' must be 'binance' or 'kraken'\n");
            return -1;
        }
    }

    // ─── Binance-specific options ─────────────────────────────────────────────
    int updateIntervalMs = 100;
    if (exchange == "binance" && config.contains("update_interval_ms")) {
        if (!config["update_interval_ms"].is_number_integer()) {
            fprintf(stderr,
                    "config error: 'update_interval_ms' must be an integer (100 or 1000)\n");
            return -1;
        }
        updateIntervalMs = config["update_interval_ms"].get<int>();
        if (updateIntervalMs != 100 && updateIntervalMs != 1000) {
            fprintf(stderr, "config error: 'update_interval_ms' must be 100 or 1000\n");
            return -1;
        }
    }

    // ─── Snapshot depth ───────────────────────────────────────────────────────
    int snapshotDepth = 1000;
    if (config.contains("snapshot_depth")) {
        if (!config["snapshot_depth"].is_number_integer()) {
            fprintf(stderr, "config error: 'snapshot_depth' must be an integer\n");
            return -1;
        }
        snapshotDepth = config["snapshot_depth"].get<int>();
        if (snapshotDepth < 5 || snapshotDepth > 5000) {
            fprintf(stderr, "config error: 'snapshot_depth' must be between 5 and 5000\n");
            return -1;
        }
    }

    // ─── Symbols (canonical BASE-QUOTE format) ────────────────────────────────
    if (!config.contains("symbols") || !config["symbols"].is_array() ||
        config["symbols"].empty()) {
        fprintf(stderr, "config error: 'symbols' must be a non-empty array\n");
        return -1;
    }
    if (!config.contains("primary_symbol") || !config["primary_symbol"].is_string()) {
        fprintf(stderr, "config error: 'primary_symbol' must be a string\n");
        return -1;
    }

    std::vector<std::string> symbols;
    std::unordered_set<std::string> seen;
    for (const auto& entry : config["symbols"]) {
        if (!entry.is_string()) {
            fprintf(stderr, "config error: every entry in 'symbols' must be a string\n");
            return -1;
        }
        std::string sym = entry.get<std::string>();
        std::transform(sym.begin(), sym.end(), sym.begin(), ::toupper);
        // Validate canonical BASE-QUOTE format: must contain exactly one '-'.
        const auto dashPos = sym.find('-');
        if (dashPos == std::string::npos || dashPos == 0 || dashPos == sym.size() - 1 ||
            sym.find('-', dashPos + 1) != std::string::npos) {
            fprintf(stderr,
                    "config error: symbol '%s' must be in canonical BASE-QUOTE format "
                    "(e.g. \"BTC-USDT\")\n",
                    sym.c_str());
            return -1;
        }
        if (seen.count(sym) != 0U) {
            fprintf(stderr, "config error: duplicate symbol '%s'\n", sym.c_str());
            return -1;
        }
        seen.insert(sym);
        symbols.push_back(sym);
    }

    std::string primarySymbol = config["primary_symbol"].get<std::string>();
    std::transform(primarySymbol.begin(), primarySymbol.end(), primarySymbol.begin(), ::toupper);

    if (std::find(symbols.begin(), symbols.end(), primarySymbol) == symbols.end()) {
        fprintf(stderr, "config error: primary_symbol '%s' is not in symbols list\n",
                primarySymbol.c_str());
        return -1;
    }

    // ─── Initialise books and metrics (keyed by canonical symbol) ─────────────
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> books;
    std::unordered_map<std::string, std::unique_ptr<Metrics>> metricsMap;
    for (const auto& sym : symbols) {
        books[sym] = std::make_unique<OrderBook>();
        metricsMap[sym] = std::make_unique<Metrics>();
    }

    // ─── Create adapter ───────────────────────────────────────────────────────
    std::unique_ptr<IExchangeAdapter> adapter;
    if (exchange == "binance") {
        adapter = std::make_unique<BinanceAdapter>(updateIntervalMs);
    } else {
        adapter = std::make_unique<KrakenAdapter>();
    }

    // ─── Start servers ────────────────────────────────────────────────────────
    SubscriberServer subServer(books);
    MetricsServer metricsServer(adapter->exchangeName(), metricsMap, books, 9090,
                                [&subServer]() { return subServer.getStats(); });
    if (!metricsServer.start()) {
        fprintf(stderr, "couldn't bind metrics server on port 9090\n");
        return -1;
    }

    if (!subServer.start()) {
        fprintf(stderr, "couldn't bind subscriber server on port 8765\n");
        return -1;
    }

    adapter->setUpdateCallback([&subServer](std::string_view exch, std::string_view symbol,
                                            const nlohmann::json& bids, const nlohmann::json& asks,
                                            long long ts) {
        subServer.broadcastUpdate(exch, symbol, bids, asks, ts);
    });

    printf("exchange:    %s\n", exchange.c_str());
    printf("metrics at   http://0.0.0.0:9090/metrics\n");
    printf("subscribers  ws://0.0.0.0:8765\n");
    printf("watching %zu symbol(s): ", symbols.size());
    for (const auto& sym : symbols) {
        printf("%s ", sym.c_str());
    }
    printf("\n");

    if (!adapter->start(symbols, books, metricsMap, snapshotDepth)) {
        fprintf(stderr, "failed to start exchange adapter\n");
        return -1;
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        for (const auto& sym : symbols) {
            books.at(sym)->printOrderBookStats();
            const auto& m = *metricsMap.at(sym);
            const long long sum = m.processingUsSum.load();
            const long long count = m.processingBuckets[10].load();
            const double avgUs = count > 0 ? static_cast<double>(sum) / count : 0.0;
            printf("[%s] Msgs: %lld  Lag: %lld ms  AvgProc: %.1f us  Updates: %lld\n",
                   sym.c_str(), m.msgCount.load(), m.lastEventLagMs.load(), avgUs, count);
        }
        printf("--------------------------------------------------\n");
    }

    return 0;
}

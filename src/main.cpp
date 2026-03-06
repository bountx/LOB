#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "feed_handler.hpp"
#include "i_exchange_adapter.hpp"
#include "metrics.hpp"
#include "metrics_server.hpp"
#include "order_book.hpp"

/*
  Setup (do once):
  1. Open WebSocket to combo @depth stream — start buffering messages immediately
  2. For each symbol, fetch REST snapshot: GET /api/v3/depth?symbol=X&limit=5000
  3. Apply buffered messages that arrived during snapshot fetch
  4. Process normally

  Every subsequent message:
  - If u < your current book's lastUpdateId → stale, ignore it
  - If U > your current book's lastUpdateId + 1 → you missed events, restart from scratch
  - Otherwise → apply the update, set your lastUpdateId = u
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

    int updateIntervalMs = 100;
    if (config.contains("update_interval_ms")) {
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

    if (!config.contains("symbols") || !config["symbols"].is_array() || config["symbols"].empty()) {
        fprintf(stderr, "config error: 'symbols' must be a non-empty array\n");
        return -1;
    }
    if (!config.contains("primary_symbol") || !config["primary_symbol"].is_string()) {
        fprintf(stderr, "config error: 'primary_symbol' must be a string\n");
        return -1;
    }

    // Validate each entry and canonicalise to UPPERCASE for map keys.
    std::vector<std::string> symbols;
    for (const auto& entry : config["symbols"]) {
        if (!entry.is_string()) {
            fprintf(stderr, "config error: every entry in 'symbols' must be a string\n");
            return -1;
        }
        std::string sym = entry.get<std::string>();
        std::transform(sym.begin(), sym.end(), sym.begin(), ::toupper);
        symbols.push_back(sym);
    }

    std::string primarySymbol = config["primary_symbol"].get<std::string>();
    std::transform(primarySymbol.begin(), primarySymbol.end(), primarySymbol.begin(), ::toupper);

    if (std::find(symbols.begin(), symbols.end(), primarySymbol) == symbols.end()) {
        fprintf(stderr, "config error: primary_symbol '%s' is not in symbols list\n",
                primarySymbol.c_str());
        return -1;
    }

    std::unordered_map<std::string, std::unique_ptr<OrderBook>> books;
    std::unordered_map<std::string, std::unique_ptr<Metrics>> metricsMap;
    for (const auto& sym : symbols) {
        books[sym] = std::make_unique<OrderBook>();
        metricsMap[sym] = std::make_unique<Metrics>();
    }

    BinanceAdapter adapter(updateIntervalMs);

    MetricsServer metricsServer(adapter.exchangeName(), metricsMap, books);
    if (!metricsServer.start()) {
        fprintf(stderr, "couldn't bind metrics server on port 9090\n");
        return -1;
    }
    printf("metrics at http://0.0.0.0:9090/metrics\n");
    printf("watching %zu symbol(s): ", symbols.size());
    for (const auto& sym : symbols) {
        printf("%s ", sym.c_str());
    }
    printf("\n");

    if (!adapter.start(symbols, books, metricsMap, snapshotDepth)) {
        fprintf(stderr, "failed to start exchange adapter\n");
        return -1;
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        for (const auto& sym : symbols) {
            books.at(sym)->printOrderBookStats();
            const auto& m = *metricsMap.at(sym);
            printf("[%s] Msgs: %lld  Lag: %lld ms  Proc: %lld us  MaxProc: %lld us\n", sym.c_str(),
                   m.msgCount.load(), m.lastEventLagMs.load(), m.lastProcessingUs.load(),
                   m.maxProcessingUs.load());
            metricsMap.at(sym)->maxProcessingUs.store(0);
        }
        printf("--------------------------------------------------\n");
    }

    return 0;
}

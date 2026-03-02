#include <ixwebsocket/IXHttpClient.h>

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
            fprintf(stderr, "Cannot open config file: %s\n", configPath);
            return -1;
        }
        config = nlohmann::json::parse(f);
    } catch (const std::exception& e) {
        fprintf(stderr, "Config parse error: %s\n", e.what());
        return -1;
    }

    if (!config.contains("symbols") || !config["symbols"].is_array() || config["symbols"].empty()) {
        fprintf(stderr, "Config error: 'symbols' must be a non-empty array\n");
        return -1;
    }
    if (!config.contains("primary_symbol") || !config["primary_symbol"].is_string()) {
        fprintf(stderr, "Config error: 'primary_symbol' must be a string\n");
        return -1;
    }

    // Validate each entry and canonicalise: UPPERCASE for map keys, lowercase for the URL.
    std::vector<std::string> symbols;
    std::string urlStreams;
    for (const auto& entry : config["symbols"]) {
        if (!entry.is_string()) {
            fprintf(stderr, "Config error: every entry in 'symbols' must be a string\n");
            return -1;
        }
        std::string sym = entry.get<std::string>();
        std::transform(sym.begin(), sym.end(), sym.begin(), ::toupper);
        symbols.push_back(sym);

        std::string lower = sym;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (!urlStreams.empty()) {
            urlStreams += "/";
        }
        urlStreams += lower + "@depth";
    }
    const std::string url = "wss://stream.binance.com:9443/stream?streams=" + urlStreams;

    std::string primarySymbol = config["primary_symbol"].get<std::string>();
    std::transform(primarySymbol.begin(), primarySymbol.end(), primarySymbol.begin(), ::toupper);

    if (std::find(symbols.begin(), symbols.end(), primarySymbol) == symbols.end()) {
        fprintf(stderr, "Config error: primary_symbol '%s' is not in symbols list\n",
                primarySymbol.c_str());
        return -1;
    }

    std::unordered_map<std::string, std::unique_ptr<OrderBook>> books;
    std::unordered_map<std::string, std::unique_ptr<Metrics>> metricsMap;
    for (const auto& sym : symbols) {
        books[sym] = std::make_unique<OrderBook>();
        metricsMap[sym] = std::make_unique<Metrics>();
    }

    ix::WebSocket webSocket;
    webSocket.setUrl(url);

    MetricsServer metricsServer(metricsMap, books);
    if (!metricsServer.start()) {
        fprintf(stderr, "Failed to bind metrics server on port 9090.\n");
        return -1;
    }
    printf("Metrics available at http://0.0.0.0:9090/metrics\n");
    printf("Subscribing to %zu symbol(s): ", symbols.size());
    for (const auto& sym : symbols) {
        printf("%s ", sym.c_str());
    }
    printf("\n");

    FeedHandler feedHandler;
    if (!feedHandler.initialize(symbols, books, webSocket, metricsMap)) {
        fprintf(stderr, "Failed to initialize FeedHandler.\n");
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

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

    const auto symbols = config["symbols"].get<std::vector<std::string>>();
    const auto primarySymbol = config["primary_symbol"].get<std::string>();

    // Build combo stream URL: wss://.../stream?streams=btcusdt@depth/ethusdt@depth/...
    std::string url = "wss://stream.binance.com:9443/stream?streams=";
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        std::string lower = symbols[i];
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        url += lower + "@depth";
        if (i + 1 < symbols.size()) {
            url += "/";
        }
    }

    std::unordered_map<std::string, std::unique_ptr<OrderBook>> books;
    std::unordered_map<std::string, std::unique_ptr<Metrics>> metricsMap;
    for (const auto& sym : symbols) {
        books[sym] = std::make_unique<OrderBook>();
        metricsMap[sym] = std::make_unique<Metrics>();
    }

    ix::WebSocket webSocket;
    webSocket.setUrl(url);

    MetricsServer metricsServer(metricsMap, books, primarySymbol);
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

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
#include <utility>
#include <vector>

#include "binance_adapter.hpp"
#include "i_exchange_adapter.hpp"
#include "kraken_adapter.hpp"
#include "metrics.hpp"
#include "metrics_server.hpp"
#include "order_book.hpp"
#include "subscriber_server.hpp"

struct ExchangeRuntime {
    std::string name;
    std::vector<std::string> symbols;
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> books;
    std::unordered_map<std::string, std::unique_ptr<Metrics>> metricsMap;
    std::unique_ptr<IExchangeAdapter> adapter;
};

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

    // ─── Exchanges array ──────────────────────────────────────────────────────
    if (!config.contains("exchanges") || !config["exchanges"].is_array() ||
        config["exchanges"].empty()) {
        fprintf(stderr, "config error: 'exchanges' must be a non-empty array\n");
        return -1;
    }

    // ─── Snapshot depth (global) ──────────────────────────────────────────────
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

    // ─── Parse per-exchange configs ────────────────────────────────────────────
    struct ExchangeConfig {
        std::string name;
        int updateIntervalMs = 100;
        std::vector<std::string> symbols;
    };

    std::vector<ExchangeConfig> exchangeConfigs;
    std::unordered_set<std::string> seenExchangeNames;

    for (const auto& entry : config["exchanges"]) {
        if (!entry.is_object()) {
            fprintf(stderr, "config error: each entry in 'exchanges' must be an object\n");
            return -1;
        }
        if (!entry.contains("name") || !entry["name"].is_string()) {
            fprintf(stderr, "config error: each exchange must have a string 'name'\n");
            return -1;
        }
        std::string exName = entry["name"].get<std::string>();
        if (exName != "binance" && exName != "kraken") {
            fprintf(stderr,
                    "config error: exchange 'name' must be 'binance' or 'kraken', got '%s'\n",
                    exName.c_str());
            return -1;
        }
        if (!seenExchangeNames.insert(exName).second) {
            fprintf(stderr, "config error: duplicate exchange '%s'\n", exName.c_str());
            return -1;
        }

        ExchangeConfig ec;
        ec.name = exName;

        if (exName == "binance" && entry.contains("update_interval_ms")) {
            if (!entry["update_interval_ms"].is_number_integer()) {
                fprintf(stderr,
                        "config error: 'update_interval_ms' must be an integer (100 or 1000)\n");
                return -1;
            }
            ec.updateIntervalMs = entry["update_interval_ms"].get<int>();
            if (ec.updateIntervalMs != 100 && ec.updateIntervalMs != 1000) {
                fprintf(stderr, "config error: 'update_interval_ms' must be 100 or 1000\n");
                return -1;
            }
        }

        if (!entry.contains("symbols") || !entry["symbols"].is_array() ||
            entry["symbols"].empty()) {
            fprintf(stderr, "config error: exchange '%s' must have a non-empty 'symbols' array\n",
                    exName.c_str());
            return -1;
        }

        std::unordered_set<std::string> seenSyms;
        for (const auto& symEntry : entry["symbols"]) {
            if (!symEntry.is_string()) {
                fprintf(stderr, "config error: every symbol in exchange '%s' must be a string\n",
                        exName.c_str());
                return -1;
            }
            std::string sym = symEntry.get<std::string>();
            std::transform(sym.begin(), sym.end(), sym.begin(), ::toupper);
            const auto dashPos = sym.find('-');
            if (dashPos == std::string::npos || dashPos == 0 || dashPos == sym.size() - 1 ||
                sym.find('-', dashPos + 1) != std::string::npos) {
                fprintf(stderr,
                        "config error: symbol '%s' in exchange '%s' must be in canonical "
                        "BASE-QUOTE format (e.g. \"BTC-USDT\")\n",
                        sym.c_str(), exName.c_str());
                return -1;
            }
            if (!seenSyms.insert(sym).second) {
                fprintf(stderr, "config error: duplicate symbol '%s' in exchange '%s'\n",
                        sym.c_str(), exName.c_str());
                return -1;
            }
            ec.symbols.push_back(sym);
        }

        exchangeConfigs.push_back(std::move(ec));
    }

    // ─── Validate primary_symbol ──────────────────────────────────────────────
    if (!config.contains("primary_symbol") || !config["primary_symbol"].is_string()) {
        fprintf(stderr, "config error: 'primary_symbol' must be a string\n");
        return -1;
    }
    std::string primarySymbol = config["primary_symbol"].get<std::string>();
    std::transform(primarySymbol.begin(), primarySymbol.end(), primarySymbol.begin(), ::toupper);
    bool primaryFound = false;
    for (const auto& ec : exchangeConfigs) {
        if (std::find(ec.symbols.begin(), ec.symbols.end(), primarySymbol) != ec.symbols.end()) {
            primaryFound = true;
            break;
        }
    }
    if (!primaryFound) {
        fprintf(stderr, "config error: primary_symbol '%s' not found in any exchange's symbols\n",
                primarySymbol.c_str());
        return -1;
    }

    // ─── Build per-exchange runtimes ──────────────────────────────────────────
    std::vector<ExchangeRuntime> runtimes;
    runtimes.reserve(exchangeConfigs.size());
    for (auto& ec : exchangeConfigs) {
        ExchangeRuntime rt;
        rt.name = ec.name;
        rt.symbols = ec.symbols;
        for (const auto& sym : ec.symbols) {
            rt.books[sym] = std::make_unique<OrderBook>();
            rt.metricsMap[sym] = std::make_unique<Metrics>();
        }
        if (ec.name == "binance") {
            rt.adapter = std::make_unique<BinanceAdapter>(ec.updateIntervalMs);
        } else {
            rt.adapter = std::make_unique<KrakenAdapter>();
        }
        runtimes.push_back(std::move(rt));
    }

    // ─── Build combined books map for SubscriberServer (keyed by "exchange.symbol") ─
    std::unordered_map<std::string, OrderBook*> booksPtrs;
    for (auto& rt : runtimes) {
        for (auto& [sym, book] : rt.books) {
            booksPtrs[rt.name + "." + sym] = book.get();
        }
    }

    // ─── Build MetricsServer views ────────────────────────────────────────────
    std::vector<ExchangeMetricsView> metricsViews;
    for (auto& rt : runtimes) {
        metricsViews.push_back({rt.name, &rt.metricsMap, &rt.books});
    }

    // ─── Start servers ────────────────────────────────────────────────────────
    SubscriberServer subServer(booksPtrs);
    MetricsServer metricsServer(std::move(metricsViews), 9090,
                                [&subServer]() { return subServer.getStats(); });

    if (!metricsServer.start()) {
        fprintf(stderr, "couldn't bind metrics server on port 9090\n");
        return -1;
    }

    // ─── Register update callbacks and start adapters ─────────────────────────
    for (auto& rt : runtimes) {
        rt.adapter->setUpdateCallback([&subServer](std::string_view exch, std::string_view symbol,
                                                   const nlohmann::json& bids,
                                                   const nlohmann::json& asks, long long ts) {
            subServer.broadcastUpdate(exch, symbol, bids, asks, ts);
        });
    }

    for (size_t i = 0; i < runtimes.size(); ++i) {
        if (!runtimes[i].adapter->start(runtimes[i].symbols, runtimes[i].books,
                                        runtimes[i].metricsMap, snapshotDepth)) {
            fprintf(stderr, "failed to start %s adapter\n", runtimes[i].name.c_str());
            for (size_t j = 0; j < i; ++j) {
                runtimes[j].adapter->stop();
            }
            return -1;
        }
    }

    // All books are now populated; open the subscriber server to incoming connections.
    if (!subServer.start()) {
        fprintf(stderr, "couldn't bind subscriber server on port 8765\n");
        for (auto& rt : runtimes) {
            rt.adapter->stop();
        }
        return -1;
    }

    printf("metrics at   http://0.0.0.0:9090/metrics\n");
    printf("subscribers  ws://0.0.0.0:8765\n");
    for (const auto& rt : runtimes) {
        printf("exchange:    %s (%zu symbol(s))\n", rt.name.c_str(), rt.symbols.size());
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        for (const auto& rt : runtimes) {
            for (const auto& sym : rt.symbols) {
                rt.books.at(sym)->printOrderBookStats();
                const auto& m = *rt.metricsMap.at(sym);
                const long long sum = m.processingUsSum.load();
                const long long count = m.processingBuckets[10].load();
                const double avgUs = count > 0 ? static_cast<double>(sum) / count : 0.0;
                printf("[%s/%s] Msgs: %lld  Lag: %lld ms  AvgProc: %.1f us  Updates: %lld\n",
                       rt.name.c_str(), sym.c_str(), m.msgCount.load(), m.lastEventLagMs.load(),
                       avgUs, count);
            }
        }
        printf("--------------------------------------------------\n");
    }

    return 0;
}

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
#include "kraken_utils.hpp"
#include "metrics.hpp"
#include "metrics_server.hpp"
#include "ofi_types.hpp"
#include "order_book.hpp"
#include "subscriber_server.hpp"
#include "utils/checked_arith.hpp"

struct ExchangeRuntime {
    std::string name;
    std::vector<std::string> symbols;
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> books;
    std::unordered_map<std::string, std::unique_ptr<Metrics>> metricsMap;
    std::unique_ptr<IExchangeAdapter> adapter;
};

/**
 * @brief Program entry point that loads configuration, validates settings, initializes
 * exchange runtimes (books, metrics, adapters), starts servers, and enters the monitoring loop.
 *
 * Loads JSON configuration (path from argv[1] or "config.json" by default), validates global and
 * per-exchange settings (including snapshot and OFI depths, exchange names, symbols, and primary
 * symbol), constructs per-exchange OrderBook and Metrics instances, wires adapter update callbacks
 * (computes and stores OFI values), starts exchange adapters and the subscriber/metrics servers,
 * then periodically prints runtime diagnostics.
 *
 * @param argc Number of command-line arguments.
 * @param argv Command-line arguments; argv[1], if present, is treated as the path to the JSON
 * config file.
 * @return int `0` on normal termination, `-1` on configuration or startup errors.
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

    // ─── OFI depth (global) ───────────────────────────────────────────────────
    // Controls how many top price levels per side are tracked in the OFI view.
    // Must be < snapshot_depth for Kraken to avoid edge-restoration backfill signals.
    int ofiDepth = 10;
    if (config.contains("ofi_depth")) {
        if (!config["ofi_depth"].is_number_integer()) {
            fprintf(stderr, "config error: 'ofi_depth' must be an integer\n");
            return -1;
        }
        ofiDepth = config["ofi_depth"].get<int>();
        if (ofiDepth < 1 || ofiDepth > snapshotDepth) {
            fprintf(stderr, "config error: 'ofi_depth' must be between 1 and snapshot_depth (%d)\n",
                    snapshotDepth);
            return -1;
        }
    }
    // Warn if ofi_depth is not strictly less than snapshot_depth: for Kraken the OFI
    // view must stay inside the subscribed depth window to avoid edge-restoration backfill.
    if (ofiDepth >= snapshotDepth) {
        fprintf(stderr,
                "warning: ofi_depth (%d) >= snapshot_depth (%d); Kraken edge-restoration "
                "events may corrupt the OFI signal. Consider setting ofi_depth < snapshot_depth.\n",
                ofiDepth, snapshotDepth);
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

    // ─── Kraken effective-depth guard ─────────────────────────────────────────
    // Kraken clamps the subscribed depth to a fixed set of values (10/25/100/500/1000).
    // The OFI view must fit entirely within that clamped depth; otherwise edge-restoration
    // events from Kraken will silently enter the OFI view and corrupt the signal.
    for (const auto& ec : exchangeConfigs) {
        if (ec.name == "kraken") {
            const int effectiveKrakenDepth = kraken::clampBookDepth(snapshotDepth);
            if (ofiDepth >= effectiveKrakenDepth) {
                fprintf(stderr,
                        "config error: ofi_depth (%d) >= Kraken effective depth (%d); "
                        "edge-restoration events will corrupt the OFI signal. "
                        "Set ofi_depth < %d.\n",
                        ofiDepth, effectiveKrakenDepth, effectiveKrakenDepth);
                return -1;
            }
            break;
        }
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
            rt.books[sym] = std::make_unique<OrderBook>(static_cast<std::size_t>(ofiDepth));
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
        // Capture raw pointer — rt.metricsMap outlives the callback (owned by runtimes).
        auto* metricsMapPtr = &rt.metricsMap;
        rt.adapter->setUpdateCallback(
            [&subServer, metricsMapPtr](std::string_view exch, std::string_view symbol,
                                        const std::vector<LevelDelta>& deltas, long long ts) {
                subServer.broadcastUpdate(exch, symbol, deltas, ts);

                // Compute OFI for this update: sum bid deltas minus ask deltas
                // for Genuine/Maintenance events that fall within the OFI view.
                // Only persist the result when the batch contains at least one
                // OFI-bearing delta — updates that only touch out-of-window levels
                // produce ofi=0 which must not overwrite the last meaningful reading.
                long long ofi = 0;
                bool hasGenuineOfi = false;
                for (const auto& d : deltas) {
                    // Include both exchange-originated (Genuine) and secondary view-change
                    // (Maintenance) deltas. Exclude Backfill (replay) deltas.
                    if (d.kind != EventKind::Backfill && (d.wasInView || d.inOfiView)) {
                        ofi += d.isBid ? d.deltaQty : -d.deltaQty;
                        hasGenuineOfi = true;
                    }
                }
                if (hasGenuineOfi) {
                    auto it = metricsMapPtr->find(std::string(symbol));
                    if (it != metricsMapPtr->end()) {
                        auto& acc = it->second->ofiAccumulator;
                        long long cur = acc.load(std::memory_order_relaxed);
                        long long next;
                        do {
                            next = checkedAdd(cur, ofi);
                        } while (!acc.compare_exchange_weak(cur, next, std::memory_order_relaxed));
                    }
                }
            });
    }

    for (size_t i = 0; i < runtimes.size(); ++i) {
        constexpr int kMaxStartAttempts = 10;
        constexpr int kBackoffCapSec    = 300;
        int backoffSec = 30;
        bool started = false;
        for (int attempt = 1; attempt <= kMaxStartAttempts; ++attempt) {
            if (runtimes[i].adapter->start(runtimes[i].symbols, runtimes[i].books,
                                           runtimes[i].metricsMap, snapshotDepth)) {
                started = true;
                break;
            }
            fprintf(stderr, "failed to start %s adapter (attempt %d/%d), retrying in %ds\n",
                    runtimes[i].name.c_str(), attempt, kMaxStartAttempts, backoffSec);
            std::this_thread::sleep_for(std::chrono::seconds(backoffSec));
            backoffSec = std::min(backoffSec * 2, kBackoffCapSec);
        }
        if (!started) {
            fprintf(stderr, "gave up starting %s adapter after %d attempts\n",
                    runtimes[i].name.c_str(), kMaxStartAttempts);
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

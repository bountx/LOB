#pragma once
#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "metrics.hpp"
#include "order_book.hpp"
#include "subscriber_stats.hpp"

// Returns the process RSS in bytes, or std::nullopt when unavailable (non-Linux,
// unreadable /proc/self/status, or unparseable VmRSS line).
inline std::optional<long long> readRssBytes() {
#ifdef __linux__
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, 6, "VmRSS:") == 0) {
            // "VmRSS:     1234 kB"
            const char* p = line.c_str() + 6;
            while (*p == ' ' || *p == '\t') ++p;
            try {
                return std::stoll(p) * 1024LL;
            } catch (const std::exception&) {
                return std::nullopt;
            }
        }
    }
#endif
    return std::nullopt;
}

// Escapes a Prometheus label value per the text exposition format spec:
/**
 * Escape a Prometheus label value by replacing special characters with backslash-escaped sequences.
 *
 * Replaces:
 * - backslash (`\`) with `\\`
 * - double-quote (`"`) with `\"`
 * - newline with `\n`
 *
 * @param v Label value to escape.
 * @return Escaped string where backslash is replaced with `\\`, double-quote with `\"`, and newline
 * with `\n`.
 */
inline std::string escapeLabelValue(std::string_view v) {
    std::string out;
    out.reserve(v.size());
    for (char c : v) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '"') {
            out += "\\\"";
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out;
}

// Builds a Prometheus text exposition for all symbols managed by one adapter.
//
// Each metric line gets both exchange="..." and symbol="..." labels so series
// from different adapters don't collide in a multi-exchange setup.
//
// Spread and best-price lines are skipped when the book is empty (no snapshot
/**
 * Builds a Prometheus text exposition containing adapter metrics and order book statistics,
 * emitting metric lines labeled with `exchange` and `symbol`.
 *
 * Emits the following metrics when available:
 * - lob_messages_total
 * - lob_event_lag_milliseconds
 * - lob_processing_time_microseconds (histogram: _bucket, _sum, _count)
 * - lob_orderbook_asks_count
 * - lob_orderbook_bids_count
 * - lob_orderbook_best_ask_price (only for positive best-ask)
 * - lob_orderbook_best_bid_price (only for positive best-bid)
 * - lob_orderbook_spread_price (only when both best-ask and best-bid are positive)
 * - lob_process_rss_bytes (process RSS)
 * - lob_subscriber_* (when subStats is non-null)
 *
 * @param exchange Identifier for the adapter; used as the `exchange` label value.
 * @param metricsMap Map from symbol to Metrics; used for message counts and processing/lag values.
 * @param books Map from symbol to OrderBook; used to obtain per-symbol order book stats.
 * @param subStats Optional subscriber server stats; omitted from output when null.
 * @return std::string Prometheus exposition text including HELP/TYPE headers and metric lines.
 */
inline std::string buildPrometheusOutput(
    std::string_view exchange,
    const std::unordered_map<std::string, std::unique_ptr<Metrics>>& metricsMap,
    const std::unordered_map<std::string, std::unique_ptr<OrderBook>>& books,
    const SubscriberStats* subStats = nullptr, bool emitHeaders = true) {
    std::ostringstream ss;

    auto writeCounterHeader = [&](const char* name, const char* help) {
        if (!emitHeaders) return;
        ss << "# HELP " << name << " " << help << "\n";
        ss << "# TYPE " << name << " counter\n";
    };
    auto writeGaugeHeader = [&](const char* name, const char* help) {
        if (!emitHeaders) return;
        ss << "# HELP " << name << " " << help << "\n";
        ss << "# TYPE " << name << " gauge\n";
    };
    auto writeLine = [&](const char* name, const std::string& symbol, double value) {
        ss << name << "{exchange=\"" << escapeLabelValue(exchange) << "\",symbol=\""
           << escapeLabelValue(symbol) << "\"} " << value << "\n";
    };

    writeCounterHeader("lob_messages_total", "Total messages received from exchange");
    for (const auto& [sym, m] : metricsMap) {
        writeLine("lob_messages_total", sym, static_cast<double>(m->msgCount.load()));
    }

    writeGaugeHeader("lob_event_lag_milliseconds", "Last event lag in milliseconds");
    for (const auto& [sym, m] : metricsMap) {
        writeLine("lob_event_lag_milliseconds", sym, static_cast<double>(m->lastEventLagMs.load()));
    }

    {
        const long long scrapeNowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count();
        std::vector<std::pair<std::string, double>> ageLines;
        for (const auto& [sym, m] : metricsMap) {
            const long long last = m->lastUpdateTimeMs.load();
            if (last == 0) continue;  // no data yet / reconnecting
            const double age = std::max(0.0, (scrapeNowMs - last) / 1000.0);
            ageLines.emplace_back(sym, age);
        }
        if (!ageLines.empty()) {
            writeGaugeHeader("lob_feed_data_age_seconds",
                             "Seconds since last order book update; absent until first update");
            for (const auto& [sym, age] : ageLines) {
                writeLine("lob_feed_data_age_seconds", sym, age);
            }
        }
    }

    // Processing time histogram — _bucket, _sum, _count per symbol.
    if (emitHeaders) {
        ss << "# HELP lob_processing_time_microseconds"
              " Time to process one order book update in microseconds\n";
        ss << "# TYPE lob_processing_time_microseconds histogram\n";
    }
    for (const auto& [sym, m] : metricsMap) {
        const std::string exch = escapeLabelValue(exchange);
        const std::string symEsc = escapeLabelValue(sym);
        // Build the shared label prefix (without closing brace).
        const std::string base = "{exchange=\"" + exch + "\",symbol=\"" + symEsc + "\"";
        for (int i = 0; i < 10; ++i) {
            ss << "lob_processing_time_microseconds_bucket" << base << ",le=\""
               << Metrics::kBucketBounds[i] << "\"} " << m->processingBuckets[i].load() << "\n";
        }
        ss << "lob_processing_time_microseconds_bucket" << base << ",le=\"+Inf\"} "
           << m->processingBuckets[10].load() << "\n";
        ss << "lob_processing_time_microseconds_sum" << base << "} " << m->processingUsSum.load()
           << "\n";
        ss << "lob_processing_time_microseconds_count" << base << "} "
           << m->processingBuckets[10].load() << "\n";
    }

    // Collect stats once per symbol to avoid locking each book multiple times.
    std::vector<std::pair<std::string, OrderBook::Stats>> allStats;
    allStats.reserve(books.size());
    for (const auto& [sym, book] : books) {
        allStats.emplace_back(sym, book->getStats());
    }

    writeGaugeHeader("lob_orderbook_asks_count", "Number of ask price levels in the order book");
    for (const auto& [sym, stats] : allStats) {
        writeLine("lob_orderbook_asks_count", sym, static_cast<double>(stats.asksCount));
    }

    writeGaugeHeader("lob_orderbook_bids_count", "Number of bid price levels in the order book");
    for (const auto& [sym, stats] : allStats) {
        writeLine("lob_orderbook_bids_count", sym, static_cast<double>(stats.bidsCount));
    }

    writeGaugeHeader("lob_orderbook_best_ask_price", "Best ask price in USD");
    for (const auto& [sym, stats] : allStats) {
        if (stats.bestAsk > 0.0) {
            writeLine("lob_orderbook_best_ask_price", sym, stats.bestAsk);
        }
    }

    writeGaugeHeader("lob_orderbook_best_bid_price", "Best bid price in USD");
    for (const auto& [sym, stats] : allStats) {
        if (stats.bestBid > 0.0) {
            writeLine("lob_orderbook_best_bid_price", sym, stats.bestBid);
        }
    }

    {
        std::vector<std::pair<std::string, double>> spreadLines;
        for (const auto& [sym, stats] : allStats) {
            if (stats.bestAsk > 0.0 && stats.bestBid > 0.0) {
                spreadLines.emplace_back(sym, stats.bestAsk - stats.bestBid);
            }
        }
        if (!spreadLines.empty()) {
            writeGaugeHeader("lob_orderbook_spread_price",
                             "Spread between best ask and best bid in USD");
            for (const auto& [sym, spread] : spreadLines) {
                writeLine("lob_orderbook_spread_price", sym, spread);
            }
        }
    }

    // Process-level memory — emitted only on the first view to avoid duplication.
    if (emitHeaders) {
        if (const auto rss = readRssBytes()) {
            ss << "# HELP lob_process_rss_bytes Resident set size of the process in bytes\n";
            ss << "# TYPE lob_process_rss_bytes gauge\n";
            ss << "lob_process_rss_bytes " << *rss << "\n";
        }
    }

    // Subscriber server stats (injected by MetricsServer when available).
    if (subStats) {
        ss << "# HELP lob_subscriber_connected_clients"
              " Current number of connected WebSocket subscriber clients\n";
        ss << "# TYPE lob_subscriber_connected_clients gauge\n";
        ss << "lob_subscriber_connected_clients " << subStats->connectedClients << "\n";

        ss << "# HELP lob_subscriber_active_subscriptions"
              " Total active stream subscriptions across all connected clients\n";
        ss << "# TYPE lob_subscriber_active_subscriptions gauge\n";
        ss << "lob_subscriber_active_subscriptions " << subStats->activeSubscriptions << "\n";

        ss << "# HELP lob_subscriber_messages_sent_total"
              " Total messages broadcast to subscriber clients\n";
        ss << "# TYPE lob_subscriber_messages_sent_total counter\n";
        ss << "lob_subscriber_messages_sent_total " << subStats->messagesSentTotal << "\n";

        ss << "# HELP lob_subscriber_backpressure_disconnects_total"
              " Clients disconnected because their send buffer exceeded the backpressure limit\n";
        ss << "# TYPE lob_subscriber_backpressure_disconnects_total counter\n";
        ss << "lob_subscriber_backpressure_disconnects_total "
           << subStats->backpressureDisconnectsTotal << "\n";
    }

    return ss.str();
}

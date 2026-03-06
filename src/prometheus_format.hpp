#pragma once
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "metrics.hpp"
#include "order_book.hpp"

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
 * - lob_processing_time_microseconds
 * - lob_max_processing_time_microseconds
 * - lob_orderbook_asks_count
 * - lob_orderbook_bids_count
 * - lob_orderbook_best_ask_price (only for positive best-ask)
 * - lob_orderbook_best_bid_price (only for positive best-bid)
 * - lob_orderbook_spread_price (only when both best-ask and best-bid are positive)
 *
 * @param exchange Identifier for the adapter; used as the `exchange` label value.
 * @param metricsMap Map from symbol to Metrics; used for message counts and processing/lag values.
 * @param books Map from symbol to OrderBook; used to obtain per-symbol order book stats (asks/bids
 * counts, best prices).
 * @return std::string Prometheus exposition text including HELP/TYPE headers and metric lines.
 * Best-price and spread metrics are omitted when price values are not positive.
 */
inline std::string buildPrometheusOutput(
    std::string_view exchange,
    const std::unordered_map<std::string, std::unique_ptr<Metrics>>& metricsMap,
    const std::unordered_map<std::string, std::unique_ptr<OrderBook>>& books) {
    std::ostringstream ss;

    auto writeCounterHeader = [&](const char* name, const char* help) {
        ss << "# HELP " << name << " " << help << "\n";
        ss << "# TYPE " << name << " counter\n";
    };
    auto writeGaugeHeader = [&](const char* name, const char* help) {
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

    writeGaugeHeader("lob_processing_time_microseconds",
                     "Last update processing time in microseconds");
    for (const auto& [sym, m] : metricsMap) {
        writeLine("lob_processing_time_microseconds", sym,
                  static_cast<double>(m->lastProcessingUs.load()));
    }

    writeGaugeHeader("lob_max_processing_time_microseconds",
                     "Maximum observed processing time in microseconds");
    for (const auto& [sym, m] : metricsMap) {
        writeLine("lob_max_processing_time_microseconds", sym,
                  static_cast<double>(m->maxProcessingUs.load()));
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

    return ss.str();
}

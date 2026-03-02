#pragma once
#include <httplib.h>

#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

#include "metrics.hpp"
#include "order_book.hpp"

class MetricsServer {
public:
    MetricsServer(std::unordered_map<std::string, std::unique_ptr<Metrics>>& metricsMap,
                  std::unordered_map<std::string, std::unique_ptr<OrderBook>>& books,
                  int port = 9090)
        : metricsMap(metricsMap), books(books), port(port) {}

    ~MetricsServer() {
        svr.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    MetricsServer(const MetricsServer&) = delete;
    MetricsServer& operator=(const MetricsServer&) = delete;
    MetricsServer(MetricsServer&&) = delete;
    MetricsServer& operator=(MetricsServer&&) = delete;

    // Returns true if the server successfully bound and is listening.
    // Safe to call only once; subsequent calls return true immediately.
    bool start() {
        if (started) {
            return true;
        }

        svr.Get("/metrics", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(buildPrometheusMetrics(), "text/plain; version=0.0.4; charset=utf-8");
        });
        svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("ok\n", "text/plain");
        });

        std::promise<bool> bound;
        auto future = bound.get_future();
        thread = std::thread([this, p = std::move(bound)]() mutable {
            const bool ok = svr.bind_to_port("0.0.0.0", port);
            p.set_value(ok);
            if (ok) {
                svr.listen_after_bind();
            }
        });

        started = future.get();
        if (!started) {
            thread.join();
        }
        return started;
    }

private:
    std::string buildPrometheusMetrics() {
        std::ostringstream ss;

        // Helper: emit HELP+TYPE header once, then one line per symbol.
        auto writeCounterHeader = [&](const char* name, const char* help) {
            ss << "# HELP " << name << " " << help << "\n";
            ss << "# TYPE " << name << " counter\n";
        };
        auto writeGaugeHeader = [&](const char* name, const char* help) {
            ss << "# HELP " << name << " " << help << "\n";
            ss << "# TYPE " << name << " gauge\n";
        };
        auto writeLine = [&](const char* name, const std::string& symbol, double value) {
            ss << name << "{symbol=\"" << symbol << "\"} " << value << "\n";
        };

        writeCounterHeader("lob_messages_total", "Total messages received from Binance");
        for (const auto& [sym, m] : metricsMap) {
            writeLine("lob_messages_total", sym, static_cast<double>(m->msgCount.load()));
        }

        writeGaugeHeader("lob_event_lag_milliseconds", "Last event lag in milliseconds");
        for (const auto& [sym, m] : metricsMap) {
            writeLine("lob_event_lag_milliseconds", sym,
                      static_cast<double>(m->lastEventLagMs.load()));
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

        writeGaugeHeader("lob_orderbook_asks_count",
                         "Number of ask price levels in the order book");
        for (const auto& [sym, stats] : allStats) {
            writeLine("lob_orderbook_asks_count", sym, static_cast<double>(stats.asksCount));
        }

        writeGaugeHeader("lob_orderbook_bids_count",
                         "Number of bid price levels in the order book");
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

        writeGaugeHeader("lob_orderbook_spread_price",
                         "Spread between best ask and best bid in USD");
        for (const auto& [sym, stats] : allStats) {
            if (stats.bestAsk > 0.0 && stats.bestBid > 0.0) {
                writeLine("lob_orderbook_spread_price", sym, stats.bestAsk - stats.bestBid);
            }
        }

        return ss.str();
    }

    std::unordered_map<std::string, std::unique_ptr<Metrics>>& metricsMap;
    std::unordered_map<std::string, std::unique_ptr<OrderBook>>& books;
    int port;
    httplib::Server svr;
    std::thread thread;
    bool started = false;
};

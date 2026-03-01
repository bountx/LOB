#pragma once
#include <httplib.h>

#include <future>
#include <sstream>
#include <string>
#include <thread>

#include "metrics.hpp"
#include "order_book.hpp"

class MetricsServer {
public:
    MetricsServer(Metrics& metrics, OrderBook& book, int port = 9090)
        : metrics(metrics), book(book), port(port) {}

    ~MetricsServer() {
        svr.stop();
        if (thread.joinable()) { thread.join(); }
    }

    MetricsServer(const MetricsServer&) = delete;
    MetricsServer& operator=(const MetricsServer&) = delete;
    MetricsServer(MetricsServer&&) = delete;
    MetricsServer& operator=(MetricsServer&&) = delete;

    // Returns true if the server successfully bound and is listening.
    // Safe to call only once; subsequent calls return true immediately.
    bool start() {
        if (started) { return true; }

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
            if (ok) { svr.listen_after_bind(); }
        });

        started = future.get();  // wait until bind succeeds or fails
        if (!started) { thread.join(); }
        return started;
    }

private:
    std::string buildPrometheusMetrics() {
        const auto stats = book.getStats();
        std::ostringstream ss;

        auto writeCounter = [&](const char* name, const char* help, long long value) {
            ss << "# HELP " << name << " " << help << "\n";
            ss << "# TYPE " << name << " counter\n";
            ss << name << " " << value << "\n";
        };
        auto writeGauge = [&](const char* name, const char* help, double value) {
            ss << "# HELP " << name << " " << help << "\n";
            ss << "# TYPE " << name << " gauge\n";
            ss << name << " " << value << "\n";
        };

        writeCounter("lob_messages_total", "Total messages received from Binance",
                     metrics.msgCount.load());
        writeGauge("lob_event_lag_milliseconds", "Last event lag in milliseconds",
                   static_cast<double>(metrics.lastEventLagMs.load()));
        writeGauge("lob_processing_time_microseconds",
                   "Last update processing time in microseconds",
                   static_cast<double>(metrics.lastProcessingUs.load()));
        writeGauge("lob_max_processing_time_microseconds",
                   "Maximum observed processing time in microseconds",
                   static_cast<double>(metrics.maxProcessingUs.load()));
        writeGauge("lob_orderbook_asks_count", "Number of ask price levels in the order book",
                   static_cast<double>(stats.asksCount));
        writeGauge("lob_orderbook_bids_count", "Number of bid price levels in the order book",
                   static_cast<double>(stats.bidsCount));

        // Only emit price and spread metrics when the book has been populated.
        if (stats.bestAsk > 0.0 && stats.bestBid > 0.0) {
            writeGauge("lob_orderbook_best_ask_price", "Best ask price in USD", stats.bestAsk);
            writeGauge("lob_orderbook_best_bid_price", "Best bid price in USD", stats.bestBid);
            writeGauge("lob_orderbook_spread_price",
                       "Spread between best ask and best bid in USD",
                       stats.bestAsk - stats.bestBid);
        }

        return ss.str();
    }

    Metrics& metrics;
    OrderBook& book;
    int port;
    httplib::Server svr;
    std::thread thread;
    bool started = false;
};

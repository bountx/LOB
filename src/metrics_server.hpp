#pragma once
#include <httplib.h>

#include <sstream>
#include <string>
#include <thread>

#include "metrics.hpp"
#include "order_book.hpp"

class MetricsServer {
public:
    MetricsServer(Metrics& metrics, OrderBook& book, int port = 9090)
        : metrics_(metrics), book_(book), port_(port) {}

    ~MetricsServer() {
        svr_.stop();
        if (thread_.joinable()) thread_.join();
    }

    void start() {
        svr_.Get("/metrics", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(buildPrometheusMetrics(), "text/plain; version=0.0.4; charset=utf-8");
        });
        svr_.Get("/health", [](const httplib::Request&, httplib::Response& res) {
            res.set_content("ok\n", "text/plain");
        });
        thread_ = std::thread([this] { svr_.listen("0.0.0.0", port_); });
    }

private:
    std::string buildPrometheusMetrics() {
        const auto stats = book_.getStats();
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
                     metrics_.msgCount.load());
        writeGauge("lob_event_lag_milliseconds", "Last event lag in milliseconds",
                   static_cast<double>(metrics_.lastEventLagMs.load()));
        writeGauge("lob_processing_time_microseconds",
                   "Last update processing time in microseconds",
                   static_cast<double>(metrics_.lastProcessingUs.load()));
        writeGauge("lob_max_processing_time_microseconds",
                   "Maximum observed processing time in microseconds",
                   static_cast<double>(metrics_.maxProcessingUs.load()));
        writeGauge("lob_orderbook_asks_count", "Number of ask price levels in the order book",
                   static_cast<double>(stats.asksCount));
        writeGauge("lob_orderbook_bids_count", "Number of bid price levels in the order book",
                   static_cast<double>(stats.bidsCount));
        writeGauge("lob_orderbook_best_ask_price", "Best ask price in USD", stats.bestAsk);
        writeGauge("lob_orderbook_best_bid_price", "Best bid price in USD", stats.bestBid);
        writeGauge("lob_orderbook_spread_price", "Spread between best ask and best bid in USD",
                   stats.bestAsk - stats.bestBid);

        return ss.str();
    }

    Metrics& metrics_;
    OrderBook& book_;
    int port_;
    httplib::Server svr_;
    std::thread thread_;
};

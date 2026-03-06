#pragma once
#include <httplib.h>

#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include "metrics.hpp"
#include "order_book.hpp"
#include "prometheus_format.hpp"

class MetricsServer {
public:
    MetricsServer(std::string_view exchange,
                  std::unordered_map<std::string, std::unique_ptr<Metrics>>& metricsMap,
                  std::unordered_map<std::string, std::unique_ptr<OrderBook>>& books,
                  int port = 9090)
        : exchange(exchange), metricsMap(metricsMap), books(books), port(port) {}

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
        return buildPrometheusOutput(exchange, metricsMap, books);
    }

    std::string exchange;
    std::unordered_map<std::string, std::unique_ptr<Metrics>>& metricsMap;
    std::unordered_map<std::string, std::unique_ptr<OrderBook>>& books;
    int port;
    httplib::Server svr;
    std::thread thread;
    bool started = false;
};

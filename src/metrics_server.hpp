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
    /**
         * @brief Construct a MetricsServer that serves Prometheus metrics and health for an exchange.
         *
         * @param exchange Identifier of the exchange whose metrics will be exposed.
         * @param metricsMap Reference to the map of metric objects indexed by symbol; the server stores this reference and reads metrics from it.
         * @param books Reference to the map of order books indexed by symbol; the server stores this reference and reads book state from it.
         * @param port TCP port to bind the HTTP metrics/health server to (default 9090).
         */
        MetricsServer(std::string_view exchange,
                  std::unordered_map<std::string, std::unique_ptr<Metrics>>& metricsMap,
                  std::unordered_map<std::string, std::unique_ptr<OrderBook>>& books,
                  int port = 9090)
        : exchange(exchange), metricsMap(metricsMap), books(books), port(port) {}

    /**
     * @brief Stop the internal HTTP server and join the worker thread.
     *
     * Ensures the HTTP server is stopped and, if the internal thread is joinable,
     * waits for the thread to finish before destruction.
     */
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
    /**
     * @brief Produce Prometheus-formatted metrics for the server's configured exchange.
     *
     * Builds the full Prometheus exposition text representing the current state of the stored
     * metrics and order books for the exchange associated with this MetricsServer.
     *
     * @return std::string Prometheus exposition body ready to be served (plain text in Prometheus format).
     */
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

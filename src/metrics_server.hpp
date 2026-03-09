#pragma once
#include <httplib.h>

#include <functional>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "metrics.hpp"
#include "order_book.hpp"
#include "prometheus_format.hpp"
#include "subscriber_stats.hpp"

struct ExchangeMetricsView {
    std::string name;
    const std::unordered_map<std::string, std::unique_ptr<Metrics>>* metricsMap;
    const std::unordered_map<std::string, std::unique_ptr<OrderBook>>* books;
};

class MetricsServer {
public:
    /**
     * @brief Construct a MetricsServer that serves Prometheus metrics and health for one or more
     * exchanges.
     *
     * @param views Per-exchange views; each view holds a name and non-owning pointers to its
     * metrics and order-book maps. Subscriber stats are emitted only for the first view to avoid
     * duplicating process-wide metrics.
     * @param port TCP port to bind the HTTP metrics/health server to (default 9090).
     * @param subStatsFn Optional callback returning subscriber server counters for the scrape.
     */
    MetricsServer(std::vector<ExchangeMetricsView> views, int port = 9090,
                  std::function<SubscriberStats()> subStatsFn = nullptr)
        : views_(std::move(views)), port(port), subStatsFn_(std::move(subStatsFn)) {}

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
     * @brief Produce Prometheus-formatted metrics for all configured exchanges.
     *
     * Iterates over all views and concatenates their Prometheus exposition text. Subscriber stats
     * are included only for the first view to avoid duplicating process-wide metrics.
     *
     * @return std::string Prometheus exposition body ready to be served (plain text in Prometheus
     * format).
     */
    std::string buildPrometheusMetrics() {
        SubscriberStats subStats;
        const SubscriberStats* subStatsPtr = nullptr;
        if (subStatsFn_) {
            subStats = subStatsFn_();
            subStatsPtr = &subStats;
        }
        std::string result;
        for (const auto& v : views_) {
            result += buildPrometheusOutput(v.name, *v.metricsMap, *v.books,
                                            &v == &views_.front() ? subStatsPtr : nullptr);
        }
        return result;
    }

    std::vector<ExchangeMetricsView> views_;
    int port;
    std::function<SubscriberStats()> subStatsFn_;
    httplib::Server svr;
    std::thread thread;
    bool started = false;
};

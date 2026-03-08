#pragma once
#include <ixwebsocket/IXWebSocketServer.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "nlohmann/json.hpp"
#include "order_book.hpp"
#include "subscriber_protocol.hpp"

class SubscriberServer {
public:
    // Clients with more than this many bytes queued get disconnected.
    static constexpr size_t kBackpressureLimit = 1 * 1024 * 1024;  // 1 MB

    explicit SubscriberServer(
        std::unordered_map<std::string, std::unique_ptr<OrderBook>>& books, int port = 8765)
        : books_(books), port_(port), server_(port) {}

    ~SubscriberServer() { stop(); }

    SubscriberServer(const SubscriberServer&) = delete;
    SubscriberServer& operator=(const SubscriberServer&) = delete;
    SubscriberServer(SubscriberServer&&) = delete;
    SubscriberServer& operator=(SubscriberServer&&) = delete;

    // Bind and begin serving subscribers in a background thread.
    // Returns false if the port cannot be bound.
    bool start() {
        if (started_) return true;

        server_.setOnConnectionCallback(
            [this](std::weak_ptr<ix::WebSocket> ws,
                   std::shared_ptr<ix::ConnectionState> state) {
                const std::string id = state->getId();
                {
                    std::lock_guard lock(mu_);
                    clients_[id] = {ws, {}};
                }
                auto sws = ws.lock();
                if (!sws) return;

                sws->setOnMessageCallback(
                    [this, ws, id](const ix::WebSocketMessagePtr& msg) {
                        if (msg->type == ix::WebSocketMessageType::Message) {
                            onMessage(ws, id, msg->str);
                        } else if (msg->type == ix::WebSocketMessageType::Close) {
                            std::lock_guard lock(mu_);
                            clients_.erase(id);
                        }
                    });
            });

        auto [ok, err] = server_.listen();
        if (!ok) {
            fprintf(stderr, "SubscriberServer: couldn't bind on port %d: %s\n", port_,
                    err.c_str());
            return false;
        }
        server_.start();
        started_ = true;
        return true;
    }

    void stop() {
        if (started_) {
            server_.stop();
            started_ = false;
        }
    }

    // Fan out an incremental update to all clients subscribed to exchange.SYMBOL.
    // bids/asks are JSON arrays of ["price","qty"] changed levels (Binance "b"/"a" fields).
    void broadcastUpdate(std::string_view exchange, std::string_view symbol,
                         const nlohmann::json& bids, const nlohmann::json& asks,
                         long long timestamp) {
        const std::string streamKey = std::string(exchange) + "." + std::string(symbol);
        const std::string msg = subscriber::buildUpdate(exchange, symbol, timestamp, bids, asks);

        std::lock_guard lock(mu_);
        for (auto& [id, client] : clients_) {
            if (!client.streams.count(streamKey)) continue;
            auto ws = client.ws.lock();
            if (!ws) continue;
            if (ws->bufferedAmount() > kBackpressureLimit) {
                ws->close();
                continue;
            }
            ws->send(msg);
        }
    }

private:
    struct ClientState {
        std::weak_ptr<ix::WebSocket> ws;
        std::unordered_set<std::string> streams;  // "exchange.SYMBOL" keys
    };

    void onMessage(std::weak_ptr<ix::WebSocket> wws, const std::string& id,
                   const std::string& text) {
        auto ws = wws.lock();
        if (!ws) return;

        auto req = subscriber::parseClientMessage(text);
        if (!req) {
            ws->send(subscriber::buildError("invalid request"));
            return;
        }

        if (req->op == "subscribe") {
            handleSubscribe(ws, id, req->streams);
        } else {
            handleUnsubscribe(id, req->streams);
        }
    }

    void handleSubscribe(std::shared_ptr<ix::WebSocket> ws, const std::string& id,
                         const std::vector<std::string>& streams) {
        for (const auto& stream : streams) {
            auto parsed = subscriber::parseStream(stream);
            if (!parsed) {
                ws->send(subscriber::buildError("bad stream: " + stream));
                continue;
            }
            const auto& [exchange, symbol] = *parsed;
            const std::string key = exchange + "." + symbol;

            {
                std::lock_guard lock(mu_);
                clients_[id].streams.insert(key);
            }

            auto it = books_.find(symbol);
            if (it != books_.end() && it->second->isSnapshotApplied()) {
                const long long ts =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
                auto bids = subscriber::bookLevelsToJson(it->second->getBids());
                auto asks = subscriber::bookLevelsToJson(it->second->getAsks());
                ws->send(subscriber::buildSnapshot(exchange, symbol, ts, bids, asks));
            }
        }
    }

    void handleUnsubscribe(const std::string& id, const std::vector<std::string>& streams) {
        std::lock_guard lock(mu_);
        auto it = clients_.find(id);
        if (it == clients_.end()) return;
        for (const auto& stream : streams) {
            auto parsed = subscriber::parseStream(stream);
            if (!parsed) continue;
            it->second.streams.erase(parsed->first + "." + parsed->second);
        }
    }

    std::unordered_map<std::string, std::unique_ptr<OrderBook>>& books_;
    int port_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, ClientState> clients_;
    ix::WebSocketServer server_;
    bool started_ = false;
};

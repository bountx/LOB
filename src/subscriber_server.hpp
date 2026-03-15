#pragma once
#include <ixwebsocket/IXWebSocketServer.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "nlohmann/json.hpp"
#include "ofi_types.hpp"
#include "order_book.hpp"
#include "subscriber_protocol.hpp"
#include "subscriber_stats.hpp"

class SubscriberServer {
public:
    // Clients with more than this many bytes queued get disconnected.
    static constexpr size_t kBackpressureLimit =
        1 * 1024 *
        1024; /**
               * @brief Constructs a SubscriberServer that manages WebSocket subscribers for order
               * book streams.
               *
               * Stores a reference to the shared order-book map and configures the server to listen
               * on the specified TCP port.
               *
               * @param books Reference to the map of order books keyed by "exchange.symbol".
               * @param port TCP port to listen on (default 8765).
               */

    explicit SubscriberServer(const std::unordered_map<std::string, OrderBook*>& books,
                              int port = 8765, int ofiDepth = 10)
        : books_(books), port_(port), ofiDepth_(ofiDepth), server_(port, "0.0.0.0") {}

    /**
     * @brief Stops the server and releases associated resources when the instance is destroyed.
     *
     * Ensures the underlying WebSocket server is stopped and internal state is cleaned up before
     * destruction.
     */
    ~SubscriberServer() { stop(); }

    /**
     * @brief Disables copy construction of SubscriberServer.
     *
     * Copying a SubscriberServer is not allowed; attempts to copy an instance are prohibited and
     * will fail to compile.
     */
    SubscriberServer(const SubscriberServer&) = delete;
    /**
     * @brief Deleted copy assignment operator to disable copying of SubscriberServer instances.
     *
     * Copying is disallowed because SubscriberServer manages internal connection state and server
     * resources that must not be duplicated.
     */
    SubscriberServer& operator=(const SubscriberServer&) = delete;
    /**
     * @brief Disable move construction for SubscriberServer.
     *
     * Prevents transferring ownership of internal resources (connections, server state)
     * by deleting the move constructor.
     */
    SubscriberServer(SubscriberServer&&) = delete;
    /**
     * @brief Disable move-assignment for SubscriberServer.
     *
     * Deleting this operator prevents moving a SubscriberServer instance, preserving its
     * non-transferable ownership semantics.
     */
    SubscriberServer& operator=(SubscriberServer&&) = delete;

    // Bind and begin serving subscribers in a background thread.
    /**
     * @brief Starts the WebSocket server and installs connection and message callbacks for new
     * clients.
     *
     * When started, incoming connections are tracked and per-connection message handlers are
     * attached.
     *
     * @return `true` if the server was started successfully or was already running, `false` if
     * binding to the configured port failed.
     */
    bool start() {
        if (started_) return true;

        server_.setOnConnectionCallback(
            [this](std::weak_ptr<ix::WebSocket> ws, std::shared_ptr<ix::ConnectionState> state) {
                const std::string id = state->getId();
                {
                    std::lock_guard lock(mu_);
                    clients_[id] = {ws, {}};
                }
                auto sws = ws.lock();
                if (!sws) return;

                sws->setOnMessageCallback([this, ws, id](const ix::WebSocketMessagePtr& msg) {
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
            fprintf(stderr, "SubscriberServer: couldn't bind on port %d: %s\n", port_, err.c_str());
            return false;
        }
        server_.start();
        started_ = true;
        return true;
    }

    /**
     * @brief Stops the subscriber server and marks it as not started.
     *
     * If the server is running, shuts down the underlying WebSocket server and updates internal
     * state. Calling this when the server is not started has no effect.
     */
    void stop() {
        if (started_) {
            server_.stop();
            started_ = false;
        }
    }

    // Returns a snapshot of subscriber server counters for the metrics scrape.
    SubscriberStats getStats() const {
        SubscriberStats s;
        {
            std::lock_guard lock(mu_);
            s.connectedClients = static_cast<long long>(clients_.size());
            for (const auto& [id, client] : clients_) {
                s.activeSubscriptions += static_cast<long long>(client.streams.size());
            }
        }
        s.messagesSentTotal = messagesSent_.load();
        s.backpressureDisconnectsTotal = backpressureDisconnects_.load();
        return s;
    }

    // Fan out an incremental update to all clients subscribed to exchange.SYMBOL.
    // ofiDelta: LOB-computed OFI change for this batch (native units). Computed in main.cpp
    //           from Genuine+Maintenance deltas within the OFI view; excludes Backfill.
    void broadcastUpdate(std::string_view exchange, std::string_view symbol,
                         const std::vector<LevelDelta>& deltas, long long timestamp,
                         double ofiDelta) {
        // Reconstruct bids/asks JSON arrays from Genuine level deltas only.
        // Backfill and synthetic OFI-maintenance entries must not be sent to subscribers.
        nlohmann::json bids = nlohmann::json::array();
        nlohmann::json asks = nlohmann::json::array();
        for (const auto& d : deltas) {
            if (d.kind != EventKind::Genuine) continue;
            (d.isBid ? bids : asks)
                .push_back({subscriber::formatScaled(d.price), subscriber::formatScaled(d.newQty)});
        }
        if (bids.empty() && asks.empty()) return;
        const std::string streamKey = std::string(exchange) + "." + std::string(symbol);

        // Increment the per-stream sequence counter under the same lock used to read clients.
        uint64_t seq = 0;
        {
            std::lock_guard lock(mu_);
            seq = ++seqMap_[streamKey];
        }

        const std::string msg =
            subscriber::buildUpdate(exchange, symbol, timestamp, bids, asks, ofiDelta, seq);

        // Collect target sockets with their IDs under the lock, then do I/O outside it.
        std::vector<std::pair<std::string, std::shared_ptr<ix::WebSocket>>> targets;
        {
            std::lock_guard lock(mu_);
            for (auto& [id, client] : clients_) {
                if (!client.streams.count(streamKey)) continue;
                auto ws = client.ws.lock();
                if (ws) targets.emplace_back(id, std::move(ws));
            }
        }
        std::vector<std::string> backpressureIds;
        for (auto& [id, ws] : targets) {
            if (ws->bufferedAmount() > kBackpressureLimit) {
                ws->close();
                backpressureIds.push_back(id);
            } else {
                ws->send(msg);
                messagesSent_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Remove backpressure-closed clients immediately so getStats() stays consistent.
        // The async Close callback will call clients_.erase() again, which is a safe no-op.
        if (!backpressureIds.empty()) {
            std::lock_guard lock(mu_);
            for (const auto& id : backpressureIds) {
                clients_.erase(id);
            }
            backpressureDisconnects_.fetch_add(static_cast<long long>(backpressureIds.size()),
                                               std::memory_order_relaxed);
        }
    }

private:
    struct ClientState {
        std::weak_ptr<ix::WebSocket> ws;
        std::unordered_set<std::string> streams;  // "exchange.SYMBOL" keys
    };

    /**
     * @brief Processes a raw message from a client WebSocket and routes subscription actions.
     *
     * Parses the client's text message, sends an error response if the message is invalid,
     * and invokes subscribe or unsubscribe handling for the parsed streams.
     *
     * @param wws Weak pointer to the client's WebSocket; no action is taken if it has expired.
     * @param id Identifier for the client connection.
     * @param text Raw client message payload.
     */
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

    /**
     * @brief Register a client's subscriptions and emit initial snapshots when available.
     *
     * Validates each requested stream, adds the "exchange.symbol" key to the client's subscription
     * set, sends an error message to the client for any malformed stream, and — if an OrderBook for
     * the symbol exists and has a snapshot applied — sends the current snapshot for that
     * exchange/symbol.
     *
     * @param ws Shared pointer to the client's WebSocket connection.
     * @param id Client identifier used to track subscriptions.
     * @param streams Vector of stream strings to subscribe to; each string is expected in
     * "exchange.symbol" form.
     */
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

            // Snapshot capture and seq read must be atomic with adding the client to clients_.
            // If the client enters clients_ before seqMap_ is read, a concurrent broadcastUpdate
            // can deliver seq=N+1 to the client before the snapshot message carrying seq=N,
            // leaving the subscriber unable to determine which updates post-date the snapshot.
            //
            // getSnapshot() acquires only the OrderBook's own internal lock. It is safe to call
            // while holding mu_ because no execution path holds an OrderBook lock while acquiring
            // mu_ (adapters release the OrderBook lock before invoking the update callback, which
            // is the only place mu_ is acquired on the hot path via broadcastUpdate).
            bool sendSnap = false;
            std::vector<std::pair<long long, long long>> snapBids, snapAsks;
            long long snapTs = 0;
            uint64_t snapSeq = 0;
            {
                std::lock_guard lock(mu_);
                auto it = books_.find(key);
                if (it != books_.end()) {
                    auto snap = it->second->getSnapshot();
                    if (snap.applied) {
                        snapTs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
                        auto sit = seqMap_.find(key);
                        if (sit != seqMap_.end()) snapSeq = sit->second;
                        snapBids = std::move(snap.bids);
                        snapAsks = std::move(snap.asks);
                        sendSnap = true;
                    }
                }
                // Client becomes visible to broadcastUpdate only after snapshot+seq are fixed.
                clients_[id].streams.insert(key);
            }

            // Send outside the lock — any broadcasts that arrive after the lock is released
            // will carry seq > snapSeq; subscribers apply them after processing the snapshot.
            if (sendSnap) {
                auto snap_bids = subscriber::bookLevelsToJson(snapBids);
                auto snap_asks = subscriber::bookLevelsToJson(snapAsks);
                ws->send(subscriber::buildSnapshot(exchange, symbol, snapTs, snap_bids, snap_asks,
                                                   ofiDepth_, snapSeq));
            }
        }
    }

    /**
     * @brief Removes the given stream subscriptions for a specific client.
     *
     * Locates the client by its identifier and erases each valid "exchange.symbol"
     * stream from the client's subscription set. Invalid stream strings are
     * ignored; if the client is not found, the call has no effect.
     *
     * @param id Client identifier used to look up subscription state.
     * @param streams List of stream descriptors (e.g., "exchange/SYMBOL") to unsubscribe.
     */
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

    const std::unordered_map<std::string, OrderBook*>& books_;
    int port_;
    int ofiDepth_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, ClientState> clients_;
    std::unordered_map<std::string, uint64_t> seqMap_;  // per-stream monotonic update counter
    ix::WebSocketServer server_;
    bool started_ = false;
    std::atomic<long long> messagesSent_{0};
    std::atomic<long long> backpressureDisconnects_{0};
};

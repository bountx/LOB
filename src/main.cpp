#include <iostream>
#include <fstream>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXHttpClient.h>
#include <nlohmann/json.hpp>
#include <map>
#include <mutex>
#include <atomic>
#include <vector>
#include <chrono>
#include <thread>
#include <iomanip>
#include <charconv>

class OrderBook {
private:
    mutable std::mutex orderBookMutex;
    long long lastUpdateId = 0;
    std::atomic<bool> snapshotApplied{false};
    std::map<long long /*price*/, long long /*quantity*/, std::less<long long>> asks;
    std::map<long long /*price*/, long long /*quantity*/, std::greater<long long>> bids;

    long long parseDecimal(const std::string& str) {
        std::from_chars_result result;
        long long value;
        result = std::from_chars(str.data(), str.data() + str.size(), value);
        if (result.ec == std::errc()) {
            return value;
        } else {
            throw std::runtime_error("Failed to parse decimal string: " + str);
        }
    }

public:
    void applySnapshot(const nlohmann::json& snapshot) {
        std::lock_guard<std::mutex> lock(orderBookMutex);
        lastUpdateId = snapshot["lastUpdateId"].get<long long>();
        for (const auto& ask : snapshot["asks"]) {
            long long price = parseDecimal(ask[0].get<std::string>());
            long long quantity = parseDecimal(ask[1].get<std::string>());
            asks[price] = quantity;
        }
        for (const auto& bid : snapshot["bids"]) {
            long long price = parseDecimal(bid[0].get<std::string>());
            long long quantity = parseDecimal(bid[1].get<std::string>());
            bids[price] = quantity;
        }
        snapshotApplied.store(true);
        printf("Snapshot applied successfully.\n");
    }

    void applyUpdate(const nlohmann::json& update) {
        std::lock_guard<std::mutex> lock(orderBookMutex);
        long long U = update["U"].get<long long>(); // first update ID in the stream
        long long u = update["u"].get<long long>(); // last update ID in the stream
        if (u <= lastUpdateId) {
            // Stale update, ignore
            printf("Stale update received, ignoring.\n");
            return;
        }
        if (U > lastUpdateId + 1) {
            // Missed updates, restart from scratch
            printf("Missed updates, restarting from scratch.\n");
            exit(1);
        }
        for (const auto& ask : update["a"]) {
            long long price = parseDecimal(ask[0].get<std::string>());
            long long quantity = parseDecimal(ask[1].get<std::string>());
            if (quantity == 0) {
                asks.erase(price);
            } else {
                asks[price] = quantity;
            }
        }
        for (const auto& bid : update["b"]) {
            long long price = parseDecimal(bid[0].get<std::string>());
            long long quantity = parseDecimal(bid[1].get<std::string>());
            if (quantity == 0) {
                bids.erase(price);
            } else {
                bids[price] = quantity;
            }
        }
        lastUpdateId = u;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(orderBookMutex);
        lastUpdateId = 0;
        asks.clear();
        bids.clear();
    }

    long long getLastUpdateId() const {
        std::lock_guard<std::mutex> lock(orderBookMutex);
        return lastUpdateId;
    }
    
    std::map<long long, long long, std::less<long long>> getAsks() const {
        std::lock_guard<std::mutex> lock(orderBookMutex);
        return asks;
    }

    std::map<long long, long long, std::greater<long long>> getBids() const {
        std::lock_guard<std::mutex> lock(orderBookMutex);
        return bids;
    }

    bool isSnapshotApplied() const {
        return snapshotApplied.load();
    }

    void printOrderBook() const {
        std::lock_guard<std::mutex> lock(orderBookMutex);
        std::cout << "Last Update ID: " << lastUpdateId << std::endl;
        std::cout << "Asks:" << std::endl;
        for (const auto& ask : asks) {
            std::cout << "Price: " << ask.first / 100000000.0 << ", Quantity: " << ask.second / 100000000.0 << std::endl;
        }
        std::cout << "Bids:" << std::endl;
        for (const auto& bid : bids) {
            std::cout << "Price: " << bid.first / 100000000.0 << ", Quantity: " << bid.second / 100000000.0 << std::endl;
        }
    }

    void printOrderBookStats() const {
        std::lock_guard<std::mutex> lock(orderBookMutex);
        std::cout << "Total Asks: " << asks.size() << ", Total Bids: " << bids.size() << std::endl;
        std::cout << std::fixed << std::setprecision(2)
            << "Best Ask: " << (asks.empty() ? 0 : asks.begin()->first / 100000000.0)
            << ", Best Bid: " << (bids.empty() ? 0 : bids.begin()->first / 100000000.0)
            << std::endl;
    }
};

struct Metrics {
    std::atomic<long long> msgCount{0};
    std::atomic<long long> lastEventLagMs{0}; // local_now - message["E"]
    std::atomic<long long> lastProcessingUs{0}; // how long applyUpdate took
    std::atomic<long long> maxProcessingUs{0}; // worst case processing time observed
};


/*
  Setup (do once):
  1. Open WebSocket to @depth stream — start buffering messages immediately
  2. Fetch REST snapshot: GET /api/v3/depth?symbol=BTCUSDT&limit=5000
  3. Check: snapshot's lastUpdateId must be less than the first buffered event's U
  4. Discard any buffered events where u <= snapshot.lastUpdateId
  5. Initialize your book with the snapshot

  Every subsequent message:
  - If u < your current book's lastUpdateId → stale, ignore it
  - If U > your current book's lastUpdateId + 1 → you missed events, restart from scratch
  - Otherwise → apply the update, set your lastUpdateId = u
*/

int main() {
    ix::WebSocket webSocket;
    webSocket.setUrl("wss://stream.binance.com:9443/ws/btcusdt@depth@100ms");
    OrderBook orderBook;

    // Before snapshot is applied we buffer incoming messages and apply them after snapshot is applied
    std::mutex bufferMutex;
    std::vector<nlohmann::json> bufferedMessages;
    std::condition_variable wsReady;
    std::mutex wsReadyMutex;
    bool wsConnected = false;
    Metrics metrics;
    webSocket.setOnMessageCallback([&orderBook, &bufferedMessages, &bufferMutex, &wsReady, &wsReadyMutex, &wsConnected, &metrics](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
          std::lock_guard<std::mutex> lock(wsReadyMutex);
          wsConnected = true;
          wsReady.notify_one();   // wake up main thread
          return;
        }
        if (msg->type != ix::WebSocketMessageType::Message) {
            return; // ignore non-message events
        }

        if (!orderBook.isSnapshotApplied()) {
            // Buffer messages until snapshot is applied
            try {
                auto json = nlohmann::json::parse(msg->str);
                std::lock_guard<std::mutex> lock(bufferMutex);
                bufferedMessages.push_back(json);
            } catch (const nlohmann::json::parse_error& e) {
                std::cerr << "JSON parse error: " << e.what() << std::endl;
            }
        } else {
            // If snapshot is already applied, process messages in real-time
            try {
                auto json = nlohmann::json::parse(msg->str);

                // event lag metric
                auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                long long eventLagMs = nowMs - json["E"].get<long long>();

                // processing time metric
                auto t0 = std::chrono::high_resolution_clock::now();
                orderBook.applyUpdate(json);
                auto t1 = std::chrono::high_resolution_clock::now();
                long long processingUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

                metrics.msgCount++;
                metrics.lastEventLagMs.store(eventLagMs);
                metrics.lastProcessingUs.store(processingUs);
                if (processingUs > metrics.maxProcessingUs.load()) {
                    metrics.maxProcessingUs.store(processingUs);
                }
            } catch (const nlohmann::json::parse_error& e) {
                std::cerr << "JSON parse error: " << e.what() << std::endl;
            }
        }
    });
    webSocket.start();

    {
        std::unique_lock<std::mutex> lock(wsReadyMutex);
        wsReady.wait(lock, [&wsConnected] { return wsConnected; }); // wait until WebSocket is connected
    }

    // Fetch REST snapshot: GET /api/v3/depth?symbol=BTCUSDT&limit=5000 automatically here
    ix::HttpClient httpClient;
    auto response = httpClient.get("https://api.binance.com/api/v3/depth?symbol=BTCUSDT&limit=5000", std::make_shared<ix::HttpRequestArgs>());
    if (response->statusCode == 200) {
        try {
            auto snapshot = nlohmann::json::parse(response->body);
            orderBook.applySnapshot(snapshot);

            // Apply buffered messages
            {
                std::lock_guard<std::mutex> lock(bufferMutex);
                for (const auto& msg : bufferedMessages) {
                    orderBook.applyUpdate(msg);
                }
                bufferedMessages.clear();
            }
        } catch (const nlohmann::json::parse_error& e) {
            std::cerr << "JSON parse error: " << e.what() << std::endl;
        }
    } else {
        std::cerr << "Failed to fetch snapshot: HTTP " << response->statusCode << std::endl;
    }


    while(true) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        orderBook.printOrderBookStats();
        printf("Metrics - Total Messages: %lld, Last Event Lag (ms): %lld, Last Processing Time (us): %lld, Max Processing Time (us): %lld\n",
               metrics.msgCount.load(), metrics.lastEventLagMs.load(), metrics.lastProcessingUs.load(), metrics.maxProcessingUs.load());
        metrics.maxProcessingUs.store(0); // reset max processing time for the next interval
        printf("--------------------------------------------------\n");
    }

    return 0;
}
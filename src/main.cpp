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
        size_t dotPos = str.find('.');
        if (dotPos == std::string::npos) {
            // No decimal point
            long long value;
            auto result = std::from_chars(str.data(), str.data() + str.size(), value);
            if (result.ec != std::errc()) {
                throw std::runtime_error("Failed to parse integer: " + str);
            }
            return value * 100000000;
        } else {
            // Parse integer part
            long long intPart = 0;
            if (dotPos > 0) {
                auto result = std::from_chars(str.data(), str.data() + dotPos, intPart);
                if (result.ec != std::errc()) {
                    throw std::runtime_error("Failed to parse integer part: " + str);
                }
            }
            
            // Parse fractional part (up to 8 digits)
            long long fracPart = 0;
            if (dotPos + 1 < str.size()) {
                std::string fracStr = str.substr(dotPos + 1);
                while (fracStr.length() < 8) fracStr += '0';
                fracStr = fracStr.substr(0, 8);
                auto result = std::from_chars(fracStr.data(), fracStr.data() + fracStr.size(), fracPart);
                if (result.ec != std::errc()) {
                    throw std::runtime_error("Failed to parse fractional part: " + str);
                }
            }
            
            return intPart * 100000000 + fracPart;
        }
    };

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

    bool applyUpdate(const nlohmann::json& update) {
        std::lock_guard<std::mutex> lock(orderBookMutex);
        long long U = update["U"].get<long long>(); // first update ID in the stream
        long long u = update["u"].get<long long>(); // last update ID in the stream
        if (u <= lastUpdateId) {
            // Stale update, ignore
            printf("Stale update received, ignoring.\n");
            return true;
        }
        if (U > lastUpdateId + 1) {
            // Missed updates, restart from scratch
            printf("Missed updates, restarting from scratch.\n");
            return false;
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
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(orderBookMutex);
        lastUpdateId = 0;
        snapshotApplied.store(false);
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
class LOBApp {
private:
    std::mutex bufferMutex;
    std::vector<nlohmann::json> bufferedMessages;
    std::mutex wsReadyMutex;
    std::condition_variable wsReady;
    bool wsConnected = false;

    // Fetch snapshot via REST, apply it, then replay buffered messages.
    // Returns true on success, false on failure.
    bool fetchAndApplySnapshot(OrderBook& orderBook) {
        ix::HttpClient httpClient;
        auto response = httpClient.get(
            "https://api.binance.com/api/v3/depth?symbol=BTCUSDT&limit=5000",
            std::make_shared<ix::HttpRequestArgs>());

        if (response->statusCode != 200) {
            fprintf(stderr, "Failed to fetch snapshot: HTTP %d\n", response->statusCode);
            return false;
        }

        try {
            auto snapshot = nlohmann::json::parse(response->body);
            orderBook.applySnapshot(snapshot);

            // Apply buffered messages that arrived while we were fetching
            std::lock_guard<std::mutex> lock(bufferMutex);
            for (const auto& msg : bufferedMessages) {
                orderBook.applyUpdate(msg);
            }
            bufferedMessages.clear();
            return true;
        } catch (const std::exception& e) {
            fprintf(stderr, "Snapshot parse/apply error: %s\n", e.what());
            return false;
        }
    }

public:
    bool initialize(OrderBook& orderBook, ix::WebSocket& webSocket, Metrics& metrics,
                    int maxSnapshotRetries = 5) {
        // Reset state for a fresh initialization
        orderBook.clear();
        {
            std::lock_guard<std::mutex> lock(bufferMutex);
            bufferedMessages.clear();
        }
        {
            std::lock_guard<std::mutex> lock(wsReadyMutex);
            wsConnected = false;
        }

        webSocket.setOnMessageCallback(
            [&orderBook, &metrics, this, &webSocket](const ix::WebSocketMessagePtr& msg) {
            if (msg->type == ix::WebSocketMessageType::Open) {
                printf("WebSocket connection opened.\n");
                std::lock_guard<std::mutex> lock(wsReadyMutex);
                wsConnected = true;
                wsReady.notify_one();
                return;
            }
            if (msg->type == ix::WebSocketMessageType::Close) {
                printf("WebSocket connection closed.\n");
                return;
            }
            if (msg->type == ix::WebSocketMessageType::Error) {
                printf("WebSocket error: %s\n", msg->errorInfo.reason.c_str());
                return;
            }
            if (msg->type != ix::WebSocketMessageType::Message) {
                return;
            }

            try {
                auto jsonMsg = nlohmann::json::parse(msg->str);

                // Buffer until snapshot is applied
                {
                    std::lock_guard<std::mutex> lock(bufferMutex);
                    if (!orderBook.isSnapshotApplied()) {
                        printf("Buffering message until snapshot is applied. Message event time: %lld\n",
                               jsonMsg["E"].get<long long>());
                        bufferedMessages.push_back(std::move(jsonMsg));
                        return;
                    }
                }

                // Event lag metric
                long long eventTime = jsonMsg["E"].get<long long>();
                long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
                metrics.lastEventLagMs.store(now - eventTime);

                // Processing time metric
                auto start = std::chrono::high_resolution_clock::now();
                if (!orderBook.applyUpdate(jsonMsg)) {
                    // Missed updates — trigger re-sync on a separate thread so we
                    // don't block the WebSocket callback.
                    printf("Restarting from scratch due to missed updates.\n");
                    orderBook.clear();
                    std::thread([this, &orderBook, &webSocket, &metrics]() {
                        this->initialize(orderBook, webSocket, metrics);
                    }).detach();
                    return;
                }
                auto end = std::chrono::high_resolution_clock::now();
                long long processingUs =
                    std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
                metrics.lastProcessingUs.store(processingUs);

                // Update max processing time if needed
                long long prevMax = metrics.maxProcessingUs.load();
                while (processingUs > prevMax &&
                       !metrics.maxProcessingUs.compare_exchange_weak(prevMax, processingUs)) {}

                metrics.msgCount.fetch_add(1);
            } catch (const std::exception& ex) {
                printf("Error processing message: %s\n", ex.what());
            }
        });

        webSocket.start();

        // Wait until WebSocket is connected before fetching the snapshot
        {
            std::unique_lock<std::mutex> lock(wsReadyMutex);
            wsReady.wait(lock, [this] { return wsConnected; });
        }

        // Retry logic for snapshot fetching
        for (int attempt = 1; attempt <= maxSnapshotRetries; ++attempt) {
            printf("Fetching snapshot (attempt %d/%d)...\n", attempt, maxSnapshotRetries);
            if (fetchAndApplySnapshot(orderBook)) {
                return true;
            }
            if (attempt < maxSnapshotRetries) {
                int delayMs = 1000 * attempt; // simple linear back-off
                printf("Snapshot fetch failed, retrying in %d ms...\n", delayMs);
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            }
        }

        fprintf(stderr, "Failed to fetch snapshot after %d attempts.\n", maxSnapshotRetries);
        return false;
    }
};


int main() {
    ix::WebSocket webSocket;
    webSocket.setUrl("wss://stream.binance.com:9443/ws/btcusdt@depth@100ms");
    OrderBook orderBook;
    Metrics metrics;
    LOBApp app;

    if (!app.initialize(orderBook, webSocket, metrics)) {
        printf("Failed to initialize LOBApp.\n");
        return -1;
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        orderBook.printOrderBookStats();
        printf("Metrics - Total Messages: %lld, Last Event Lag (ms): %lld, Last Processing Time (us): %lld, Max Processing Time (us): %lld\n",
               metrics.msgCount.load(), metrics.lastEventLagMs.load(), metrics.lastProcessingUs.load(), metrics.maxProcessingUs.load());
        metrics.maxProcessingUs.store(0);
        printf("--------------------------------------------------\n");
    }

    return 0;
}
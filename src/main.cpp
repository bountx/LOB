#include <ixwebsocket/IXHttpClient.h>

#include <chrono>
#include <cstdio>
#include <thread>

#include "feed_handler.hpp"
#include "metrics.hpp"
#include "order_book.hpp"

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
    Metrics metrics;
    FeedHandler feedHandler;

    if (!feedHandler.initialize(orderBook, webSocket, metrics)) {
        printf("Failed to initialize FeedHandler.\n");
        return -1;
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        orderBook.printOrderBookStats();
        printf(
            "Metrics - Total Messages: %lld, Last Event Lag (ms): %lld, Last Processing Time (us): "
            "%lld, Max Processing Time (us): %lld\n",
            metrics.msgCount.load(), metrics.lastEventLagMs.load(), metrics.lastProcessingUs.load(),
            metrics.maxProcessingUs.load());
        metrics.maxProcessingUs.store(0);
        printf("--------------------------------------------------\n");
    }

    return 0;
}
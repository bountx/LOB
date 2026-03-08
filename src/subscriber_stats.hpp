#pragma once

// Snapshot of subscriber server counters, passed to the metrics server on each scrape.
struct SubscriberStats {
    long long connectedClients = 0;
    long long activeSubscriptions = 0;
    long long messagesSentTotal = 0;
    long long backpressureDisconnectsTotal = 0;
};

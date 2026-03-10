#pragma once
#include <array>
#include <atomic>

struct Metrics {
    std::atomic<long long> msgCount{0};
    std::atomic<long long> lastEventLagMs{0};  // local_now - message["E"] (system_clock ms)
    std::atomic<long long> lastUpdateTimeMs{
        0};  // steady_clock ms of last book update; 0 = no data yet / reconnecting
    // Accumulated OFI since last Prometheus scrape: sum of bid deltaQty minus sum of ask deltaQty
    // for Genuine/Maintenance, in-OFI-view levels. Scaled by 1e8 (same units as order quantities).
    // Reset to 0 on each read (see prometheus_format.hpp).
    std::atomic<long long> ofiAccumulator{0};

    // Histogram for update processing time (µs).
    // Buckets are cumulative (Prometheus convention): each count includes all
    // observations <= the upper bound.  Index 10 is the +Inf bucket (== total count).
    static constexpr std::array<long long, 10> kBucketBounds{1,   5,   10,  25,   50,
                                                             100, 250, 500, 1000, 5000};
    std::atomic<long long> processingBuckets[11]{{0}, {0}, {0}, {0}, {0}, {0},
                                                 {0}, {0}, {0}, {0}, {0}};
    std::atomic<long long> processingUsSum{0};

    void recordProcessingUs(long long us) {
        processingUsSum.fetch_add(us, std::memory_order_relaxed);
        for (int i = 0; i < 10; ++i) {
            if (us <= kBucketBounds[i])
                processingBuckets[i].fetch_add(1, std::memory_order_relaxed);
        }
        processingBuckets[10].fetch_add(1, std::memory_order_relaxed);  // +Inf
    }
};

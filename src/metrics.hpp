#pragma once
#include <array>
#include <atomic>

struct Metrics {
    std::atomic<long long> msgCount{0};
    std::atomic<long long> lastEventLagMs{0};  // local_now - message["E"]

    // Histogram for update processing time (µs).
    // Buckets are cumulative (Prometheus convention): each count includes all
    // observations <= the upper bound.  Index 10 is the +Inf bucket (== total count).
    static constexpr std::array<long long, 10> kBucketBounds{
        1, 5, 10, 25, 50, 100, 250, 500, 1000, 5000};
    std::atomic<long long> processingBuckets[11]{
        {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}};
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

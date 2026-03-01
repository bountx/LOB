#pragma once
#include <atomic>

struct Metrics {
    std::atomic<long long> msgCount{0};
    std::atomic<long long> lastEventLagMs{0};    // local_now - message["E"]
    std::atomic<long long> lastProcessingUs{0};  // how long applyUpdate took
    std::atomic<long long> maxProcessingUs{0};   // worst case processing time observed
};
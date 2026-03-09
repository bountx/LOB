#pragma once
#include <cstdint>
#include <vector>

enum class EventKind : uint8_t { Genuine, Backfill };

// Represents a single price-level change emitted by OrderBook::applyUpdate / applyDelta.
// newQty  – absolute quantity after the change (0 = level removed)
// deltaQty – signed change: newQty - prevQty
// inOfiView – true if the price falls within the book's top-ofiDepth OFI view after the update
// kind     – Genuine for live stream events; Backfill for snapshot replay
struct LevelDelta {
    long long price = 0;
    long long newQty = 0;
    long long deltaQty = 0;
    bool isBid = false;
    bool inOfiView = false;
    EventKind kind = EventKind::Genuine;
};

struct UpdateResult {
    bool success = false;            // false = sequence gap detected; caller must resync
    std::vector<LevelDelta> deltas;  // populated on success
};

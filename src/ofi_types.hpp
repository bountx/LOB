#pragma once
#include <cstdint>
#include <vector>

enum class EventKind : uint8_t {
    Genuine,     // live stream event from the exchange
    Backfill,    // snapshot replay / buffer drain
    Maintenance  // secondary OFI-view entry or eviction not directly from the exchange
};

// Represents a single price-level change emitted by OrderBook::applyUpdate / applyDelta.
// newQty    – actual state quantity after the change (0 = level removed from state)
// deltaQty  – for Genuine/Backfill: newQty - prevQty (state change)
//             for Maintenance: OFI-view contribution change (±lvlQty), NOT a state change
// wasInView – true if the price was in the OFI view before this update
// inOfiView – true if the price is in the OFI view after this update
// kind      – Genuine for live stream; Backfill for snapshot replay;
//             Maintenance for secondary OFI-view evictions/replacements caused by a Genuine update
//             (secondary deltas caused by a Backfill update carry kind == Backfill)
//
// OFI consumers should filter on kind != Backfill and (wasInView || inOfiView).
// Maintenance deltas must be included to account for evicted and replacement levels.
struct LevelDelta {
    long long price = 0;
    long long newQty = 0;
    long long deltaQty = 0;
    bool isBid = false;
    bool wasInView = false;
    bool inOfiView = false;
    EventKind kind = EventKind::Genuine;
};

struct UpdateResult {
    bool success = false;            // false = sequence gap detected; caller must resync
    std::vector<LevelDelta> deltas;  // populated on success
};

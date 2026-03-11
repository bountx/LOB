#pragma once

#include <cstddef>
#include <cstring>
#include <memory>

// Flat-array order book side, indexed by price tick.
//
// All prices must be integer multiples of tickSize (in internal 1e8 units).
// Supports O(1) insert/update/delete, O(1) best-price lookup, and O(gap)
// "next active level above/below a boundary" — which is O(1) amortised for
// dense books (BTC/USDT at $0.01 tick).
//
// Memory: (2 * halfRange + 1) * 8 bytes per ladder.
// Default: ±5 000 ticks = ±$50 at $0.01 tick ≈ 78 KB per ladder.
class PriceLadder {
public:
    static constexpr long long kDefaultTick = 1'000'000LL;  // $0.01 in 1e8 units
    static constexpr int kDefaultHalfRange = 5'000;         // ±$50 at default tick

    explicit PriceLadder(long long tickSizeArg = kDefaultTick,
                         int halfRangeArg = kDefaultHalfRange);

    // Set (or remove if qty == 0) a price level.
    // Triggers re-centering if price falls outside the current window.
    void set(long long price, long long qty);

    // Returns qty for price; 0 if absent or out of current window.
    long long get(long long price) const;

    // Remove all levels and reset the centre so the next set() re-initialises.
    void clear();

    std::size_t activeCount() const { return count; }

    // Highest price with qty > 0.  Returns 0 if empty.
    long long bestHigh() const;

    // Lowest price with qty > 0.  Returns 0 if empty.
    long long bestLow() const;

    // Highest price with qty > 0 strictly less than 'below'.  Returns 0 if none.
    long long prevBelow(long long below) const;

    // Lowest price with qty > 0 strictly greater than 'above'.  Returns 0 if none.
    long long nextAbove(long long above) const;

    // Returns true if price is within the current window (false when uninitialised).
    bool inRange(long long price) const noexcept;

    // Visit every active level in ascending price order.  fn(price, qty).
    template <typename Fn>
    void forEach(Fn&& fn) const {
        for (int i = 0; i < size; ++i) {
            if (qtys[i] > 0) {
                fn(toPrice(i), qtys[i]);
            }
        }
    }

private:
    long long tickSize;
    int halfRange;
    int size;                 // halfRange * 2 + 1
    long long basePrice = 0;  // price at index 0; always a multiple of tickSize
    bool initialized = false;

    std::unique_ptr<long long[]> qtys;
    std::size_t count = 0;

    int bestHighIdx = -1;  // highest index with qty > 0; -1 when empty
    int bestLowIdx  = -1;  // lowest  index with qty > 0; -1 when empty

    int toIdx(long long price) const noexcept;
    long long toPrice(int idx) const noexcept;

    void initCenter(long long price);
    void recenter(long long price);
    void rebuildBestIndices();
};

#include "price_ladder.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

PriceLadder::PriceLadder(long long tickSizeArg, int halfRangeArg) {
    if (tickSizeArg <= 0) {
        throw std::invalid_argument("PriceLadder: tickSizeArg must be positive");
    }
    if (halfRangeArg < 0 || halfRangeArg > (std::numeric_limits<int>::max() - 1) / 2) {
        throw std::invalid_argument("PriceLadder: halfRangeArg out of range");
    }
    tickSize  = tickSizeArg;
    halfRange = halfRangeArg;
    size      = (halfRangeArg * 2) + 1;
    qtys      = std::make_unique<long long[]>(static_cast<std::size_t>(size));
    std::memset(qtys.get(), 0, static_cast<std::size_t>(size) * sizeof(long long));
}

int PriceLadder::toIdx(long long price) const noexcept {
    // Caller must ensure price is inRange() — toIdx does NOT bounds-check.
    return static_cast<int>((price - basePrice) / tickSize);
}

long long PriceLadder::toPrice(int idx) const noexcept {
    return basePrice + (static_cast<long long>(idx) * tickSize);
}

bool PriceLadder::inRange(long long price) const noexcept {
    if (!initialized) {
        return false;
    }
    const long long offset = price - basePrice;
    if (offset < 0) {
        return false;
    }
    // Compare as long long to avoid int overflow on extreme prices.
    return (offset / tickSize) < static_cast<long long>(size);
}

void PriceLadder::initCenter(long long price) {
    const long long rounded = (price / tickSize) * tickSize;
    basePrice = rounded - (static_cast<long long>(halfRange) * tickSize);
    initialized = true;
}

void PriceLadder::rebuildBestIndices() {
    bestHighIdx = -1;
    bestLowIdx  = -1;
    count = 0;
    for (int i = 0; i < size; ++i) {
        if (qtys[i] > 0) {
            ++count;
            bestHighIdx = std::max(bestHighIdx, i);
            if (bestLowIdx < 0) { bestLowIdx = i; }  // ascending scan: first found is lowest
        }
    }
}

void PriceLadder::recenter(long long price) {
    const long long rounded = (price / tickSize) * tickSize;
    const long long newBase = rounded - (static_cast<long long>(halfRange) * tickSize);
    const long long shiftLL = (newBase - basePrice) / tickSize;

    if (shiftLL > 0 && shiftLL < static_cast<long long>(size)) {
        const int shift = static_cast<int>(shiftLL);
        const int keep = size - shift;
        std::memmove(qtys.get(), qtys.get() + shift,
                     static_cast<std::size_t>(keep) * sizeof(long long));
        std::memset(qtys.get() + keep, 0, static_cast<std::size_t>(shift) * sizeof(long long));
    } else if (shiftLL < 0 && -shiftLL < static_cast<long long>(size)) {
        const int shift = static_cast<int>(shiftLL);
        const int keep = size + shift;  // shift is negative
        std::memmove(qtys.get() - shift, qtys.get(),
                     static_cast<std::size_t>(keep) * sizeof(long long));
        std::memset(qtys.get(), 0, static_cast<std::size_t>(-shift) * sizeof(long long));
    } else {
        // Shift >= array size: all existing entries fall outside the new window.
        std::memset(qtys.get(), 0, static_cast<std::size_t>(size) * sizeof(long long));
    }
    basePrice = newBase;
    rebuildBestIndices();
}

bool PriceLadder::set(long long price, long long qty) {
    assert(qty >= 0 && "qty must be non-negative");
    bool recentered = false;
    if (!initialized) {
        initCenter(price);
    } else if (!inRange(price)) {
        recenter(price);
        recentered = true;
    }

    const int idx = toIdx(price);
    assert(idx >= 0 && idx < size && "PriceLadder::set: index out of bounds after inRange check");
    const long long old = qtys[idx];
    qtys[idx] = qty;

    if (old == 0 && qty > 0) {
        ++count;
    } else if (old > 0 && qty == 0) {
        --count;
    }

    // Maintain bestHighIdx.
    if (qty > 0 && idx > bestHighIdx) {
        bestHighIdx = idx;
    } else if (qty == 0 && idx == bestHighIdx) {
        while (bestHighIdx >= 0 && qtys[bestHighIdx] == 0) {
            --bestHighIdx;
        }
    }

    // Maintain bestLowIdx.
    if (qty > 0 && (bestLowIdx < 0 || idx < bestLowIdx)) {
        bestLowIdx = idx;
    } else if (qty == 0 && idx == bestLowIdx) {
        ++bestLowIdx;
        while (bestLowIdx < size && qtys[bestLowIdx] == 0) {
            ++bestLowIdx;
        }
        if (bestLowIdx >= size) { bestLowIdx = -1; }
    }
    return recentered;
}

long long PriceLadder::get(long long price) const {
    if (!inRange(price)) {
        return 0;
    }
    return qtys[toIdx(price)];
}

void PriceLadder::clear() {
    if (initialized) {
        std::memset(qtys.get(), 0, static_cast<std::size_t>(size) * sizeof(long long));
    }
    count = 0;
    bestHighIdx = -1;
    bestLowIdx  = -1;
    initialized = false;
}

long long PriceLadder::bestHigh() const {
    if (bestHighIdx < 0) {
        return 0;
    }
    return toPrice(bestHighIdx);
}

long long PriceLadder::bestLow() const {
    if (bestLowIdx < 0) {
        return 0;
    }
    return toPrice(bestLowIdx);
}

long long PriceLadder::prevBelow(long long below) const {
    if (!initialized || bestHighIdx < 0) {
        return 0;
    }
    const long long offset = below - basePrice;
    if (offset <= 0) {
        return 0;
    }
    const long long rawIdx = (offset - 1) / tickSize;
    int startIdx = (rawIdx >= static_cast<long long>(size)) ? size - 1 : static_cast<int>(rawIdx);
    for (int i = startIdx; i >= 0; --i) {
        if (qtys[i] > 0) {
            return toPrice(i);
        }
    }
    return 0;
}

long long PriceLadder::nextAbove(long long above) const {
    if (!initialized || bestLowIdx < 0) {
        return 0;
    }
    const long long offset = above - basePrice;
    int startIdx;
    if (offset < 0) {
        startIdx = 0;
    } else {
        const long long rawIdx = offset / tickSize + 1;
        if (rawIdx >= static_cast<long long>(size)) return 0;
        startIdx = static_cast<int>(rawIdx);
    }
    for (int i = startIdx; i < size; ++i) {
        if (qtys[i] > 0) {
            return toPrice(i);
        }
    }
    return 0;
}

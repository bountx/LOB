#include "price_ladder.hpp"

#include <algorithm>
#include <cstring>

PriceLadder::PriceLadder(long long tickSizeArg, int halfRangeArg)
    : tickSize(tickSizeArg),
      halfRange(halfRangeArg),
      size((halfRangeArg * 2) + 1),
      bestLowIdx((halfRangeArg * 2) + 1) {
    qtys = std::make_unique<long long[]>(size);
    std::memset(qtys.get(), 0, static_cast<std::size_t>(size) * sizeof(long long));
}

int PriceLadder::toIdx(long long price) const noexcept {
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
    return static_cast<int>(offset / tickSize) < size;
}

void PriceLadder::initCenter(long long price) {
    const long long rounded = (price / tickSize) * tickSize;
    basePrice = rounded - (static_cast<long long>(halfRange) * tickSize);
    initialized = true;
}

void PriceLadder::rebuildBestIndices() {
    bestHighIdx = -1;
    bestLowIdx = size;
    count = 0;
    for (int i = 0; i < size; ++i) {
        if (qtys[i] > 0) {
            ++count;
            bestHighIdx = std::max(bestHighIdx, i);
            bestLowIdx = std::min(bestLowIdx, i);
        }
    }
}

void PriceLadder::recenter(long long price) {
    const long long rounded = (price / tickSize) * tickSize;
    const long long newBase = rounded - (static_cast<long long>(halfRange) * tickSize);
    const int shift = static_cast<int>((newBase - basePrice) / tickSize);

    if (shift > 0 && shift < size) {
        const int keep = size - shift;
        std::memmove(qtys.get(), qtys.get() + shift,
                     static_cast<std::size_t>(keep) * sizeof(long long));
        std::memset(qtys.get() + keep, 0, static_cast<std::size_t>(shift) * sizeof(long long));
    } else if (shift < 0 && -shift < size) {
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

void PriceLadder::set(long long price, long long qty) {
    if (!initialized) {
        initCenter(price);
    } else if (!inRange(price)) {
        recenter(price);
    }

    const int idx = toIdx(price);
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
    if (qty > 0 && idx < bestLowIdx) {
        bestLowIdx = idx;
    } else if (qty == 0 && idx == bestLowIdx) {
        while (bestLowIdx < size && qtys[bestLowIdx] == 0) {
            ++bestLowIdx;
        }
    }
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
    bestLowIdx = size;
    initialized = false;
}

long long PriceLadder::bestHigh() const {
    if (bestHighIdx < 0) {
        return 0;
    }
    return toPrice(bestHighIdx);
}

long long PriceLadder::bestLow() const {
    if (bestLowIdx >= size) {
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
    int startIdx = static_cast<int>(offset / tickSize) - 1;
    startIdx = std::min(startIdx, size - 1);
    for (int i = startIdx; i >= 0; --i) {
        if (qtys[i] > 0) {
            return toPrice(i);
        }
    }
    return 0;
}

long long PriceLadder::nextAbove(long long above) const {
    if (!initialized || bestLowIdx >= size) {
        return 0;
    }
    const long long offset = above - basePrice;
    int startIdx;
    if (offset < 0) {
        startIdx = 0;
    } else {
        startIdx = static_cast<int>(offset / tickSize) + 1;
    }
    if (startIdx >= size) {
        return 0;
    }
    for (int i = startIdx; i < size; ++i) {
        if (qtys[i] > 0) {
            return toPrice(i);
        }
    }
    return 0;
}

#pragma once
#include <climits>

// Saturating (clamped) arithmetic on long long.
// All functions return a result clamped to [LLONG_MIN, LLONG_MAX] on overflow
// rather than wrapping, making them safe for use with unbounded accumulators.

inline long long checkedAdd(long long a, long long b) noexcept {
    if (b > 0 && a > LLONG_MAX - b) return LLONG_MAX;
    if (b < 0 && a < LLONG_MIN - b) return LLONG_MIN;
    return a + b;
}

inline long long checkedSubtract(long long a, long long b) noexcept {
    if (b < 0 && a > LLONG_MAX + b) return LLONG_MAX;
    if (b > 0 && a < LLONG_MIN + b) return LLONG_MIN;
    return a - b;
}

inline long long checkedMultiply(long long a, long long b) noexcept {
    if (a == 0 || b == 0) return 0;
    // Use __int128 (GCC/Clang) to detect overflow without UB.
    const __int128 result = static_cast<__int128>(a) * b;
    if (result > LLONG_MAX) return LLONG_MAX;
    if (result < LLONG_MIN) return LLONG_MIN;
    return static_cast<long long>(result);
}

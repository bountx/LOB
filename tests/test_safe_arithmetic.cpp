#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>

#include "safe_arithmetic.hpp"

// ─── safeMultiply ─────────────────────────────────────────────────────────────

TEST(SafeMultiply, NormalPositiveCases) {
    EXPECT_EQ(safeMultiply(5LL, 3LL), 15LL);
    EXPECT_EQ(safeMultiply(1LL, 0LL), 0LL);
    EXPECT_EQ(safeMultiply(100'000'000LL, 100'000LL), 10'000'000'000'000LL);
}

TEST(SafeMultiply, NegativeTimesPositive) { EXPECT_EQ(safeMultiply(-5LL, 3LL), -15LL); }

TEST(SafeMultiply, PositiveOverflowThrows) {
    long long big = std::numeric_limits<long long>::max();
    EXPECT_THROW(safeMultiply(big, 2LL), std::overflow_error);
}

TEST(SafeMultiply, NegativeUnderflowThrows) {
    long long min = std::numeric_limits<long long>::lowest();
    EXPECT_THROW(safeMultiply(min, 2LL), std::overflow_error);
}

TEST(SafeMultiply, MinTimesMinusOneThrows) {
    // LLONG_MIN * -1 = 2^63 which overflows long long max (2^63 - 1)
    long long min = std::numeric_limits<long long>::lowest();
    EXPECT_THROW(safeMultiply(min, -1LL), std::overflow_error);
}

// ─── safeAdd ──────────────────────────────────────────────────────────────────

TEST(SafeAdd, NormalCases) {
    EXPECT_EQ(safeAdd(5LL, 3LL), 8LL);
    EXPECT_EQ(safeAdd(-5LL, 3LL), -2LL);
    EXPECT_EQ(safeAdd(0LL, 0LL), 0LL);
}

TEST(SafeAdd, PositiveOverflowThrows) {
    long long big = std::numeric_limits<long long>::max();
    EXPECT_THROW(safeAdd(big, 1LL), std::overflow_error);
}

TEST(SafeAdd, NegativeUnderflowThrows) {
    long long min = std::numeric_limits<long long>::lowest();
    EXPECT_THROW(safeAdd(min, -1LL), std::overflow_error);
}

// ─── safeSubstract ───────────────────────────────────────────────────────────

TEST(SafeSubstract, NormalCases) {
    EXPECT_EQ(safeSubstract(5LL, 3LL), 2LL);
    EXPECT_EQ(safeSubstract(0LL, 0LL), 0LL);
    EXPECT_EQ(safeSubstract(-5LL, -3LL), -2LL);
}

TEST(SafeSubstract, PositiveOverflowThrows) {
    long long big = std::numeric_limits<long long>::max();
    // big - (-1) would overflow
    EXPECT_THROW(safeSubstract(big, -1LL), std::overflow_error);
}

TEST(SafeSubstract, NegativeUnderflowThrows) {
    long long min = std::numeric_limits<long long>::lowest();
    // min - 1 underflows
    EXPECT_THROW(safeSubstract(min, 1LL), std::overflow_error);
}

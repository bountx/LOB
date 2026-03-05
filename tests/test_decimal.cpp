#include <gtest/gtest.h>

#include <stdexcept>

#include "decimal.hpp"

constexpr long long SCALE = 100'000'000LL;

// ─── Integer inputs ───────────────────────────────────────────────────────────

TEST(ParseDecimal, IntegerWithoutDot) {
    EXPECT_EQ(parseDecimal("100"), 100LL * SCALE);
    EXPECT_EQ(parseDecimal("0"), 0LL);
    EXPECT_EQ(parseDecimal("1"), SCALE);
}

// ─── Decimal inputs ───────────────────────────────────────────────────────────

TEST(ParseDecimal, HalfUnit) { EXPECT_EQ(parseDecimal("0.5"), SCALE / 2); }

TEST(ParseDecimal, TrailingZeroesAreIgnored) {
    EXPECT_EQ(parseDecimal("1.10"), 110'000'000LL);
    EXPECT_EQ(parseDecimal("50000.00"), 50000LL * SCALE);
}

TEST(ParseDecimal, FullEightDecimalPlaces) { EXPECT_EQ(parseDecimal("0.12345678"), 12'345'678LL); }

TEST(ParseDecimal, ShortDecimalIsPaddedToEightPlaces) {
    EXPECT_EQ(parseDecimal("0.1"), 10'000'000LL);
    EXPECT_EQ(parseDecimal("1.25"), 125'000'000LL);
}

TEST(ParseDecimal, NoIntegerPartBeforeDot) {
    // ".5" → intPart=0, frac=50000000
    EXPECT_EQ(parseDecimal(".5"), 50'000'000LL);
}

TEST(ParseDecimal, TypicalBtcPrice) {
    // 94500.50 * 1e8
    EXPECT_EQ(parseDecimal("94500.50"), 9'450'050'000'000LL);
}

TEST(ParseDecimal, NegativeValue) {
    // -1.5 → -(1 * SCALE + SCALE/2)
    EXPECT_EQ(parseDecimal("-1.5"), -(SCALE + SCALE / 2));
}

// ─── Error cases ──────────────────────────────────────────────────────────────

TEST(ParseDecimal, BareDecimalPointThrows) { EXPECT_THROW(parseDecimal("."), std::runtime_error); }

TEST(ParseDecimal, TooManyFractionalDigitsThrows) {
    EXPECT_THROW(parseDecimal("1.123456789"), std::runtime_error);
}

TEST(ParseDecimal, NonNumericStringThrows) {
    EXPECT_THROW(parseDecimal("abc"), std::runtime_error);
}

TEST(ParseDecimal, NonNumericFractionalPartThrows) {
    EXPECT_THROW(parseDecimal("1.2a"), std::runtime_error);
}

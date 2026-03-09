#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "kraken_utils.hpp"

// ─── doubleToString ───────────────────────────────────────────────────────────

TEST(KrakenUtils, DoubleToString_WholeNumber) {
    EXPECT_EQ(kraken::doubleToString(50000.0), "50000.00000000");
}

TEST(KrakenUtils, DoubleToString_Fractional) {
    EXPECT_EQ(kraken::doubleToString(0.00001), "0.00001000");
}

TEST(KrakenUtils, DoubleToString_Zero) {
    EXPECT_EQ(kraken::doubleToString(0.0), "0.00000000");
}

TEST(KrakenUtils, DoubleToString_NegativeRoundtrip) {
    // Negative prices don't occur in order books, but the function must not crash.
    const std::string s = kraken::doubleToString(-1.5);
    EXPECT_EQ(s, "-1.50000000");
}

TEST(KrakenUtils, DoubleToString_HighPrecision) {
    EXPECT_EQ(kraken::doubleToString(94500.12345678), "94500.12345678");
}

// ─── levelToStringPair ────────────────────────────────────────────────────────

TEST(KrakenUtils, LevelToStringPair_Basic) {
    nlohmann::json level = {{"price", 50000.0}, {"qty", 1.5}};
    auto pair = kraken::levelToStringPair(level);
    ASSERT_EQ(pair.size(), 2u);
    EXPECT_EQ(pair[0].get<std::string>(), "50000.00000000");
    EXPECT_EQ(pair[1].get<std::string>(), "1.50000000");
}

TEST(KrakenUtils, LevelToStringPair_ZeroQty) {
    nlohmann::json level = {{"price", 49999.99}, {"qty", 0.0}};
    auto pair = kraken::levelToStringPair(level);
    EXPECT_EQ(pair[1].get<std::string>(), "0.00000000");
}

// ─── levelsToStringPairs ─────────────────────────────────────────────────────

TEST(KrakenUtils, LevelsToStringPairs_Empty) {
    auto arr = nlohmann::json::array();
    auto result = kraken::levelsToStringPairs(arr);
    EXPECT_TRUE(result.empty());
}

TEST(KrakenUtils, LevelsToStringPairs_Multiple) {
    nlohmann::json levels = nlohmann::json::array();
    levels.push_back({{"price", 50001.0}, {"qty", 0.5}});
    levels.push_back({{"price", 50002.0}, {"qty", 1.0}});

    auto result = kraken::levelsToStringPairs(levels);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0][0].get<std::string>(), "50001.00000000");
    EXPECT_EQ(result[0][1].get<std::string>(), "0.50000000");
    EXPECT_EQ(result[1][0].get<std::string>(), "50002.00000000");
    EXPECT_EQ(result[1][1].get<std::string>(), "1.00000000");
}

TEST(KrakenUtils, LevelsToStringPairs_PreservesOrder) {
    nlohmann::json levels = nlohmann::json::array();
    levels.push_back({{"price", 49999.0}, {"qty", 2.0}});
    levels.push_back({{"price", 49998.0}, {"qty", 3.0}});

    auto result = kraken::levelsToStringPairs(levels);
    // Order must be preserved (no sorting applied here).
    EXPECT_EQ(result[0][0].get<std::string>(), "49999.00000000");
    EXPECT_EQ(result[1][0].get<std::string>(), "49998.00000000");
}

// ─── isoTimestampToMs ─────────────────────────────────────────────────────────

TEST(KrakenUtils, IsoTimestampToMs_UnixEpoch) {
    // 1970-01-01T00:00:00.000000Z == 0 ms
    EXPECT_EQ(kraken::isoTimestampToMs("1970-01-01T00:00:00.000000Z"), 0LL);
}

TEST(KrakenUtils, IsoTimestampToMs_KnownTimestamp) {
    // 2024-01-01T00:00:00.000000Z = 1704067200000 ms
    EXPECT_EQ(kraken::isoTimestampToMs("2024-01-01T00:00:00.000000Z"), 1704067200000LL);
}

TEST(KrakenUtils, IsoTimestampToMs_WithMicroseconds) {
    // 2024-01-01T00:00:00.123456Z → 1704067200000 + 123 ms
    EXPECT_EQ(kraken::isoTimestampToMs("2024-01-01T00:00:00.123456Z"), 1704067200123LL);
}

TEST(KrakenUtils, IsoTimestampToMs_ShortStringReturnsZero) {
    EXPECT_EQ(kraken::isoTimestampToMs(""), 0LL);
    EXPECT_EQ(kraken::isoTimestampToMs("2024"), 0LL);
}

// ─── clampBookDepth ───────────────────────────────────────────────────────────

TEST(KrakenUtils, ClampBookDepth_ExactMatch) {
    EXPECT_EQ(kraken::clampBookDepth(10), 10);
    EXPECT_EQ(kraken::clampBookDepth(25), 25);
    EXPECT_EQ(kraken::clampBookDepth(100), 100);
    EXPECT_EQ(kraken::clampBookDepth(500), 500);
    EXPECT_EQ(kraken::clampBookDepth(1000), 1000);
}

TEST(KrakenUtils, ClampBookDepth_RoundsDown) {
    EXPECT_EQ(kraken::clampBookDepth(50), 25);
    EXPECT_EQ(kraken::clampBookDepth(200), 100);
    EXPECT_EQ(kraken::clampBookDepth(999), 500);
}

TEST(KrakenUtils, ClampBookDepth_BelowMinimum) {
    EXPECT_EQ(kraken::clampBookDepth(1), 10);
    EXPECT_EQ(kraken::clampBookDepth(0), 10);
}

TEST(KrakenUtils, ClampBookDepth_AboveMaximum) {
    EXPECT_EQ(kraken::clampBookDepth(5000), 1000);
    EXPECT_EQ(kraken::clampBookDepth(9999), 1000);
}

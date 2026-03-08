#include <gtest/gtest.h>

#include "symbol_normalizer.hpp"

// ─── Explicit mapping ─────────────────────────────────────────────────────────

TEST(SymbolNormalizer, ExplicitToCanonical) {
    SymbolNormalizer n;
    n.add("binance", "BTCUSDT", "BTC-USDT");
    auto result = n.toCanonical("binance", "BTCUSDT");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "BTC-USDT");
}

TEST(SymbolNormalizer, ExplicitFromCanonical) {
    SymbolNormalizer n;
    n.add("binance", "BTCUSDT", "BTC-USDT");
    auto result = n.fromCanonical("binance", "BTC-USDT");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "BTCUSDT");
}

TEST(SymbolNormalizer, ExplicitOverridesAutoRule) {
    // Explicit entry maps "MY/PAIR" to a custom canonical, overriding auto-rule.
    SymbolNormalizer n;
    n.add("kraken", "MY/PAIR", "OVERRIDE-CANONICAL");
    auto result = n.toCanonical("kraken", "MY/PAIR");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "OVERRIDE-CANONICAL");
}

TEST(SymbolNormalizer, UnknownExchangeReturnsNullopt) {
    SymbolNormalizer n;
    EXPECT_FALSE(n.toCanonical("coinbase", "BTC-USD").has_value());
    EXPECT_FALSE(n.fromCanonical("coinbase", "BTC-USD").has_value());
}

// ─── Binance auto-rules ───────────────────────────────────────────────────────

TEST(SymbolNormalizer, BinanceAutoToCanonical_USDT) {
    SymbolNormalizer n;
    auto r = n.toCanonical("binance", "BTCUSDT");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "BTC-USDT");
}

TEST(SymbolNormalizer, BinanceAutoToCanonical_ETH) {
    SymbolNormalizer n;
    auto r = n.toCanonical("binance", "ETHUSDT");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "ETH-USDT");
}

TEST(SymbolNormalizer, BinanceAutoToCanonical_BTC_quote) {
    SymbolNormalizer n;
    auto r = n.toCanonical("binance", "ETHBTC");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "ETH-BTC");
}

TEST(SymbolNormalizer, BinanceAutoToCanonical_EUR) {
    SymbolNormalizer n;
    auto r = n.toCanonical("binance", "BTCEUR");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "BTC-EUR");
}

TEST(SymbolNormalizer, BinanceAutoToCanonical_BUSD) {
    SymbolNormalizer n;
    auto r = n.toCanonical("binance", "SOLUSDT");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "SOL-USDT");
}

TEST(SymbolNormalizer, BinanceUnknownSuffixReturnsNullopt) {
    SymbolNormalizer n;
    // "BTCXYZ" doesn't match any known quote suffix.
    EXPECT_FALSE(n.toCanonical("binance", "BTCXYZ").has_value());
}

TEST(SymbolNormalizer, BinanceAutoFromCanonical) {
    SymbolNormalizer n;
    auto r = n.fromCanonical("binance", "BTC-USDT");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "BTCUSDT");
}

TEST(SymbolNormalizer, BinanceAutoFromCanonical_BTC_quote) {
    SymbolNormalizer n;
    auto r = n.fromCanonical("binance", "ETH-BTC");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "ETHBTC");
}

TEST(SymbolNormalizer, BinanceAutoRoundTrip) {
    SymbolNormalizer n;
    // BTCUSDT → BTC-USDT → BTCUSDT
    auto canonical = n.toCanonical("binance", "BTCUSDT");
    ASSERT_TRUE(canonical.has_value());
    auto local = n.fromCanonical("binance", *canonical);
    ASSERT_TRUE(local.has_value());
    EXPECT_EQ(*local, "BTCUSDT");
}

// ─── Kraken auto-rules ────────────────────────────────────────────────────────

TEST(SymbolNormalizer, KrakenAutoToCanonical_SlashToCanonical) {
    SymbolNormalizer n;
    auto r = n.toCanonical("kraken", "BTC/USDT");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "BTC-USDT");
}

TEST(SymbolNormalizer, KrakenAutoToCanonical_ETHUSD) {
    SymbolNormalizer n;
    auto r = n.toCanonical("kraken", "ETH/USD");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "ETH-USD");
}

TEST(SymbolNormalizer, KrakenAutoToCanonical_XBT_BTC_alias) {
    // Kraken's legacy "XBT" ticker for Bitcoin should map to canonical "BTC".
    SymbolNormalizer n;
    auto r = n.toCanonical("kraken", "XBT/USDT");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "BTC-USDT");
}

TEST(SymbolNormalizer, KrakenAutoToCanonical_XBTUSD) {
    SymbolNormalizer n;
    auto r = n.toCanonical("kraken", "XBT/USD");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "BTC-USD");
}

TEST(SymbolNormalizer, KrakenAutoToCanonical_XBTEUR) {
    SymbolNormalizer n;
    auto r = n.toCanonical("kraken", "XBT/EUR");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "BTC-EUR");
}

TEST(SymbolNormalizer, KrakenAutoFromCanonical_DashToSlash) {
    SymbolNormalizer n;
    auto r = n.fromCanonical("kraken", "BTC-USDT");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "BTC/USDT");
}

TEST(SymbolNormalizer, KrakenAutoFromCanonical_ETHUSD) {
    SymbolNormalizer n;
    auto r = n.fromCanonical("kraken", "ETH-USD");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "ETH/USD");
}

TEST(SymbolNormalizer, KrakenAutoRoundTrip) {
    // BTC/USDT → BTC-USDT → BTC/USDT
    SymbolNormalizer n;
    auto canonical = n.toCanonical("kraken", "BTC/USDT");
    ASSERT_TRUE(canonical.has_value());
    auto local = n.fromCanonical("kraken", *canonical);
    ASSERT_TRUE(local.has_value());
    EXPECT_EQ(*local, "BTC/USDT");
}

// ─── defaultNormalizer ────────────────────────────────────────────────────────

TEST(SymbolNormalizer, DefaultNormalizerBinanceAutoRules) {
    auto n = defaultNormalizer();
    // Auto-rules still work with a default-constructed normalizer.
    auto r = n.toCanonical("binance", "BTCUSDT");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "BTC-USDT");
}

TEST(SymbolNormalizer, DefaultNormalizerKrakenAutoRules) {
    auto n = defaultNormalizer();
    auto r = n.toCanonical("kraken", "ETH/USDT");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "ETH-USDT");
}

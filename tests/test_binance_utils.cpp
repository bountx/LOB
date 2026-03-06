#include <gtest/gtest.h>

#include "binance_utils.hpp"

// ─── streamToSymbol ───────────────────────────────────────────────────────────
// Covers the stream name to uppercase symbol conversion used as map keys.

TEST(StreamToSymbol, BasicDepthStreamExtractsSymbol) {
    EXPECT_EQ(streamToSymbol("btcusdt@depth"), "BTCUSDT");
}

TEST(StreamToSymbol, DepthWithIntervalSuffixIsStripped) {
    // The @100ms / @1000ms parameter must not bleed into the symbol.
    EXPECT_EQ(streamToSymbol("ethusdt@depth@100ms"), "ETHUSDT");
    EXPECT_EQ(streamToSymbol("solusdt@depth@1000ms"), "SOLUSDT");
}

TEST(StreamToSymbol, OutputIsAlwaysUppercase) {
    EXPECT_EQ(streamToSymbol("BtcUsdt@depth"), "BTCUSDT");
}

TEST(StreamToSymbol, AlreadyUppercaseInputIsIdempotent) {
    EXPECT_EQ(streamToSymbol("BNBUSDT@depth"), "BNBUSDT");
}

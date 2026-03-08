#include <gtest/gtest.h>

#include <map>
#include <nlohmann/json.hpp>

#include "subscriber_protocol.hpp"

// ─── parseStream ─────────────────────────────────────────────────────────────

TEST(ParseStream, ValidStream) {
    auto result = subscriber::parseStream("binance.BTCUSDT.book");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->first, "binance");
    EXPECT_EQ(result->second, "BTCUSDT");
}

TEST(ParseStream, ValidStreamKraken) {
    auto result = subscriber::parseStream("kraken.XBTUSDT.book");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->first, "kraken");
    EXPECT_EQ(result->second, "XBTUSDT");
}

TEST(ParseStream, MissingBookSuffix) {
    EXPECT_FALSE(subscriber::parseStream("binance.BTCUSDT.trade").has_value());
    EXPECT_FALSE(subscriber::parseStream("binance.BTCUSDT").has_value());
}

TEST(ParseStream, EmptyExchange) {
    EXPECT_FALSE(subscriber::parseStream(".BTCUSDT.book").has_value());
}

TEST(ParseStream, EmptySymbol) {
    EXPECT_FALSE(subscriber::parseStream("binance..book").has_value());
}

TEST(ParseStream, NoDot) {
    EXPECT_FALSE(subscriber::parseStream("binanceBTCUSDTbook").has_value());
}

TEST(ParseStream, EmptyString) {
    EXPECT_FALSE(subscriber::parseStream("").has_value());
}

// ─── parseClientMessage ──────────────────────────────────────────────────────

TEST(ParseClientMessage, ValidSubscribe) {
    auto result = subscriber::parseClientMessage(
        R"({"op":"subscribe","streams":["binance.BTCUSDT.book"]})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->op, "subscribe");
    ASSERT_EQ(result->streams.size(), 1u);
    EXPECT_EQ(result->streams[0], "binance.BTCUSDT.book");
}

TEST(ParseClientMessage, ValidUnsubscribe) {
    auto result = subscriber::parseClientMessage(
        R"({"op":"unsubscribe","streams":["binance.BTCUSDT.book","kraken.XBTUSDT.book"]})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->op, "unsubscribe");
    EXPECT_EQ(result->streams.size(), 2u);
}

TEST(ParseClientMessage, MultipleStreams) {
    auto result = subscriber::parseClientMessage(
        R"({"op":"subscribe","streams":["binance.BTCUSDT.book","binance.ETHUSDT.book"]})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->streams.size(), 2u);
}

TEST(ParseClientMessage, EmptyStreams) {
    auto result = subscriber::parseClientMessage(R"({"op":"subscribe","streams":[]})");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->streams.empty());
}

TEST(ParseClientMessage, UnknownOp) {
    EXPECT_FALSE(
        subscriber::parseClientMessage(R"({"op":"ping","streams":[]})").has_value());
}

TEST(ParseClientMessage, MissingOp) {
    EXPECT_FALSE(subscriber::parseClientMessage(R"({"streams":[]})").has_value());
}

TEST(ParseClientMessage, MissingStreams) {
    EXPECT_FALSE(subscriber::parseClientMessage(R"({"op":"subscribe"})").has_value());
}

TEST(ParseClientMessage, StreamsNotArray) {
    EXPECT_FALSE(
        subscriber::parseClientMessage(R"({"op":"subscribe","streams":"not-array"})").has_value());
}

TEST(ParseClientMessage, NonStringStreamEntry) {
    EXPECT_FALSE(
        subscriber::parseClientMessage(R"({"op":"subscribe","streams":[42]})").has_value());
}

TEST(ParseClientMessage, InvalidJson) {
    EXPECT_FALSE(subscriber::parseClientMessage("not json at all").has_value());
}

// ─── formatScaled ────────────────────────────────────────────────────────────

TEST(FormatScaled, WholeNumber) {
    // 1.0 * 1e8 = 100000000
    EXPECT_EQ(subscriber::formatScaled(100'000'000LL), "1");
}

TEST(FormatScaled, WithDecimals) {
    // 50001.5 * 1e8 = 5000150000000
    EXPECT_EQ(subscriber::formatScaled(5'000'150'000'000LL), "50001.5");
}

TEST(FormatScaled, SmallFraction) {
    // 0.00000001 * 1e8 = 1
    EXPECT_EQ(subscriber::formatScaled(1LL), "0.00000001");
}

TEST(FormatScaled, Zero) {
    EXPECT_EQ(subscriber::formatScaled(0LL), "0");
}

TEST(FormatScaled, TrailingZerosTrimmed) {
    // 1.50000000 * 1e8 = 150000000
    EXPECT_EQ(subscriber::formatScaled(150'000'000LL), "1.5");
}

TEST(FormatScaled, TypicalBtcPrice) {
    // 94500.00 * 1e8 = 9450000000000
    EXPECT_EQ(subscriber::formatScaled(9'450'000'000'000LL), "94500");
}

TEST(FormatScaled, NegativeWholeNumber) {
    // -1.0 * 1e8 = -100000000
    EXPECT_EQ(subscriber::formatScaled(-100'000'000LL), "-1");
}

TEST(FormatScaled, NegativeSmallFraction) {
    // -0.5 * 1e8 = -50000000  (intPart=0, so must produce "-0.5" not "0.-5")
    EXPECT_EQ(subscriber::formatScaled(-50'000'000LL), "-0.5");
}

/**
 * @brief Convert a map of scaled price levels into a JSON array of price/size pairs.
 *
 * @param levels Map whose keys are prices scaled by 1e8 and whose values are sizes scaled by 1e8.
 * @return nlohmann::json An array where each element is a two-element array `[price, size]`:
 *         both `price` and `size` are decimal strings produced from the corresponding scaled integers;
 *         elements appear in the same iteration order as `levels`.
 */

TEST(BookLevelsToJson, AscendingAsks) {
    std::map<long long, long long, std::less<>> asks;
    asks[5'000'100'000'000LL] = 150'000'000LL;   // 50001.0 @ 1.5
    asks[5'000'200'000'000LL] = 50'000'000LL;    // 50002.0 @ 0.5

    auto j = subscriber::bookLevelsToJson(asks);
    ASSERT_EQ(j.size(), 2u);
    EXPECT_EQ(j[0][0], "50001");
    EXPECT_EQ(j[0][1], "1.5");
    EXPECT_EQ(j[1][0], "50002");
    EXPECT_EQ(j[1][1], "0.5");
}

TEST(BookLevelsToJson, DescendingBids) {
    std::map<long long, long long, std::greater<>> bids;
    bids[4'999'900'000'000LL] = 200'000'000LL;  // 49999.0 @ 2.0
    bids[4'999'800'000'000LL] = 100'000'000LL;  // 49998.0 @ 1.0

    auto j = subscriber::bookLevelsToJson(bids);
    ASSERT_EQ(j.size(), 2u);
    // Descending: best bid first
    EXPECT_EQ(j[0][0], "49999");
    EXPECT_EQ(j[1][0], "49998");
}

TEST(BookLevelsToJson, EmptyBook) {
    std::map<long long, long long, std::less<>> empty;
    auto j = subscriber::bookLevelsToJson(empty);
    EXPECT_TRUE(j.empty());
}

/**
 * @brief Build a raw JSON snapshot message for an order book.
 *
 * Constructs a JSON object with type "snapshot" containing the provided exchange,
 * symbol, timestamp, and the supplied bids and asks arrays, then returns it as a
 * serialized string.
 *
 * @param exchange Exchange identifier (e.g., "binance").
 * @param symbol Market symbol (e.g., "BTCUSDT").
 * @param ts Timestamp in milliseconds since epoch.
 * @param bids JSON array of bid entries where each entry is [price, size] as strings.
 * @param asks JSON array of ask entries where each entry is [price, size] as strings.
 * @return std::string Serialized JSON snapshot containing fields: type, exchange, symbol, ts, bids, asks.
 */

TEST(BuildSnapshot, CorrectStructure) {
    nlohmann::json bids = nlohmann::json::array();
    bids.push_back({"49999", "2"});
    nlohmann::json asks = nlohmann::json::array();
    asks.push_back({"50001", "1"});

    auto raw = subscriber::buildSnapshot("binance", "BTCUSDT", 1712000000000LL, bids, asks);
    auto j = nlohmann::json::parse(raw);

    EXPECT_EQ(j["type"], "snapshot");
    EXPECT_EQ(j["exchange"], "binance");
    EXPECT_EQ(j["symbol"], "BTCUSDT");
    EXPECT_EQ(j["ts"], 1712000000000LL);
    EXPECT_EQ(j["bids"].size(), 1u);
    EXPECT_EQ(j["asks"].size(), 1u);
    EXPECT_EQ(j["bids"][0][0], "49999");
    EXPECT_EQ(j["asks"][0][0], "50001");
}

// ─── buildUpdate ─────────────────────────────────────────────────────────────

TEST(BuildUpdate, CorrectStructure) {
    nlohmann::json bids = nlohmann::json::array();
    bids.push_back({"94500.00", "0.000"});
    nlohmann::json asks = nlohmann::json::array();
    asks.push_back({"94502.00", "1.800"});

    auto raw = subscriber::buildUpdate("binance", "BTCUSDT", 1712000000123LL, bids, asks);
    auto j = nlohmann::json::parse(raw);

    EXPECT_EQ(j["type"], "update");
    EXPECT_EQ(j["exchange"], "binance");
    EXPECT_EQ(j["symbol"], "BTCUSDT");
    EXPECT_EQ(j["ts"], 1712000000123LL);
    EXPECT_EQ(j["bids"][0][0], "94500.00");
    EXPECT_EQ(j["asks"][0][0], "94502.00");
}

/**
 * @brief Constructs a JSON-formatted error message.
 *
 * @param message Human-readable error description to include in the JSON.
 * @return std::string Raw JSON string containing an object with "type" set to "error" and "message" set to the provided message.
 */

TEST(BuildError, CorrectStructure) {
    auto raw = subscriber::buildError("bad stream: foo");
    auto j = nlohmann::json::parse(raw);

    EXPECT_EQ(j["type"], "error");
    EXPECT_EQ(j["message"], "bad stream: foo");
}

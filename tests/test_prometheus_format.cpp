#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>

#include "prometheus_format.hpp"

namespace {

/**
 * @brief Build a minimal JSON snapshot for OrderBook::applySnapshot.
 *
 * Constructs a JSON object containing a numeric `lastUpdateId` and two arrays
 * `"asks"` and `"bids"`, where each entry is a two-element array holding
 * price and quantity as strings.
 *
 * @param lastUpdateId The snapshot's last update identifier.
 * @param asks Vector of (price, quantity) pairs to populate the `"asks"` array.
 * @param bids Vector of (price, quantity) pairs to populate the `"bids"` array.
 * @return nlohmann::json JSON object with keys `lastUpdateId`, `asks`, and `bids`.
 */
nlohmann::json makeSnap(long long lastUpdateId,
                        const std::vector<std::pair<std::string, std::string>>& asks,
                        const std::vector<std::pair<std::string, std::string>>& bids) {
    nlohmann::json snap;
    snap["lastUpdateId"] = lastUpdateId;
    snap["asks"] = nlohmann::json::array();
    for (const auto& [p, q] : asks) snap["asks"].push_back({p, q});
    snap["bids"] = nlohmann::json::array();
    for (const auto& [p, q] : bids) snap["bids"].push_back({p, q});
    return snap;
}

/**
 * Extracts all non-empty, non-comment data lines from a Prometheus exposition text block.
 *
 * @param output The full Prometheus exposition text (may contain comment lines starting with `#`
 * and blank lines).
 * @return std::vector<std::string> A vector containing each data line (lines that are not empty and
 * do not start with `#`), in their original order without trailing newlines.
 */
std::vector<std::string> dataLines(const std::string& output) {
    std::vector<std::string> result;
    std::istringstream ss(output);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line[0] != '#') {
            result.push_back(line);
        }
    }
    return result;
}

// Fixture that owns a single-symbol metrics + book pair for BTCUSDT on "testex".
struct PrometheusFormatTest : testing::Test {
    std::unordered_map<std::string, std::unique_ptr<Metrics>> metrics;
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> books;

    /**
     * @brief Prepare test fixture state by creating default entries for "BTCUSDT".
     *
     * Initializes the fixture's metrics and books maps with a fresh Metrics and
     * OrderBook instance at key "BTCUSDT".
     */
    void SetUp() override {
        metrics["BTCUSDT"] = std::make_unique<Metrics>();
        books["BTCUSDT"] = std::make_unique<OrderBook>();
    }

    /**
     * @brief Builds a Prometheus exposition-format text block for the fixture's metrics and order
     * books.
     *
     * @param exchange Exchange label value to include on each metric line; defaults to "testex".
     * @return std::string Prometheus-formatted text containing HELP/TYPE headers and metric data
     * lines for the fixture's metrics and books.
     */
    std::string build(std::string_view exchange = "testex") {
        return buildPrometheusOutput(exchange, metrics, books);
    }
};

}  // namespace

// ─── Label correctness ────────────────────────────────────────────────────────

TEST_F(PrometheusFormatTest, ExchangeLabelAppearsOnEveryDataLine) {
    const auto lines = dataLines(build("binance"));
    ASSERT_FALSE(lines.empty());
    for (const auto& line : lines) {
        // Process-level metrics (e.g. lob_process_rss_bytes) carry no labels — skip them.
        if (line.find('{') == std::string::npos) continue;
        EXPECT_NE(line.find("exchange=\"binance\""), std::string::npos) << line;
    }
}

TEST_F(PrometheusFormatTest, SymbolLabelAppearsOnEveryDataLine) {
    const auto lines = dataLines(build());
    ASSERT_FALSE(lines.empty());
    for (const auto& line : lines) {
        // Process-level metrics (e.g. lob_process_rss_bytes) carry no labels — skip them.
        if (line.find('{') == std::string::npos) continue;
        EXPECT_NE(line.find("symbol=\"BTCUSDT\""), std::string::npos) << line;
    }
}

// ─── Counter value ────────────────────────────────────────────────────────────

TEST_F(PrometheusFormatTest, MessageCountIsReflectedInOutput) {
    metrics["BTCUSDT"]->msgCount.store(42);
    const auto output = build();
    // The exact label+value fragment must be present.
    EXPECT_NE(output.find("lob_messages_total{exchange=\"testex\",symbol=\"BTCUSDT\"} 42"),
              std::string::npos);
}

// ─── Spread guard ─────────────────────────────────────────────────────────────

TEST_F(PrometheusFormatTest, SpreadOmittedWhenBookIsEmpty) {
    // No snapshot applied → both ask and bid are 0.0 → spread must not appear.
    const auto output = build();
    EXPECT_EQ(output.find("lob_orderbook_spread_price"), std::string::npos);
}

TEST_F(PrometheusFormatTest, SpreadOmittedWhenOnlyAsksPopulated) {
    books["BTCUSDT"]->applySnapshot(makeSnap(1, {{"50001.00", "1.0"}}, {}));
    const auto output = build();
    EXPECT_EQ(output.find("lob_orderbook_spread_price"), std::string::npos);
}

TEST_F(PrometheusFormatTest, SpreadEmittedWhenBothSidesPopulated) {
    books["BTCUSDT"]->applySnapshot(makeSnap(1, {{"50002.00", "1.0"}}, {{"50000.00", "1.0"}}));
    const auto output = build();
    EXPECT_NE(output.find("lob_orderbook_spread_price"), std::string::npos);
}

// ─── HELP / TYPE headers ──────────────────────────────────────────────────────

TEST_F(PrometheusFormatTest, HelpAndTypeHeadersPresentForAllMetrics) {
    const auto output = build();
    for (const auto* name :
         {"lob_messages_total", "lob_event_lag_milliseconds", "lob_processing_time_microseconds",
          "lob_orderbook_asks_count", "lob_orderbook_bids_count"}) {
        EXPECT_NE(output.find(std::string("# HELP ") + name), std::string::npos) << name;
        EXPECT_NE(output.find(std::string("# TYPE ") + name), std::string::npos) << name;
    }
}

// ─── Multi-symbol ─────────────────────────────────────────────────────────────

TEST_F(PrometheusFormatTest, AllSymbolsAppearInOutput) {
    // Add a second symbol.
    metrics["ETHUSDT"] = std::make_unique<Metrics>();
    books["ETHUSDT"] = std::make_unique<OrderBook>();
    metrics["ETHUSDT"]->msgCount.store(7);

    const auto output = build();
    EXPECT_NE(output.find("symbol=\"BTCUSDT\""), std::string::npos);
    EXPECT_NE(output.find("symbol=\"ETHUSDT\""), std::string::npos);
}

// ─── Feed data age (staleness sentinel) ───────────────────────────────────────

// lastUpdateTimeMs == 0 is the "no data yet / reconnecting" sentinel.
// The metric must not appear at all in that state — Prometheus would otherwise
// show a spurious 0-age reading during startup or after a reconnect.
TEST_F(PrometheusFormatTest, DataAgeAbsentWhenSentinelIsZero) {
    // Default-constructed Metrics has lastUpdateTimeMs == 0.
    const auto output = build();
    EXPECT_EQ(output.find("lob_feed_data_age_seconds"), std::string::npos);
}

// Once a symbol receives its first update the sentinel moves off 0 and the
// metric must be present with a non-negative value.
TEST_F(PrometheusFormatTest, DataAgeEmittedAfterFirstUpdate) {
    const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
    metrics["BTCUSDT"]->lastUpdateTimeMs.store(nowMs - 500);  // 0.5 s ago

    const auto output = build();
    EXPECT_NE(output.find("lob_feed_data_age_seconds"), std::string::npos);
    EXPECT_NE(output.find("symbol=\"BTCUSDT\""), std::string::npos);
}

// The emitted value must reflect the actual elapsed time (within a generous
// 2-second window to keep the test stable under slow CI).
TEST_F(PrometheusFormatTest, DataAgeValueApproximatelyCorrect) {
    const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
    // Pretend the last update was exactly 3 seconds ago.
    metrics["BTCUSDT"]->lastUpdateTimeMs.store(nowMs - 3000);

    const auto output = build();

    // Extract the numeric value from the data line.
    const std::string key = "lob_feed_data_age_seconds{exchange=\"testex\",symbol=\"BTCUSDT\"} ";
    const auto pos = output.find(key);
    ASSERT_NE(pos, std::string::npos) << "metric line not found";
    const double age = std::stod(output.substr(pos + key.size()));
    // 3 s target, allow ±2 s for scheduling jitter.
    EXPECT_GE(age, 1.0);
    EXPECT_LE(age, 5.0);
}

// In a multi-symbol setup symbols still at sentinel 0 (awaiting snapshot after
// reconnect) must be suppressed while symbols with real data are still emitted.
TEST_F(PrometheusFormatTest, DataAgeSkipsZeroSentinelSymbolsInMultiSymbol) {
    metrics["ETHUSDT"] = std::make_unique<Metrics>();
    books["ETHUSDT"] = std::make_unique<OrderBook>();
    // BTCUSDT stays at sentinel 0 (no snapshot yet).
    // ETHUSDT has received updates.
    const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
    metrics["ETHUSDT"]->lastUpdateTimeMs.store(nowMs - 1000);

    const auto output = build();
    // ETHUSDT age must be present.
    EXPECT_NE(output.find("lob_feed_data_age_seconds{exchange=\"testex\",symbol=\"ETHUSDT\"}"),
              std::string::npos);
    // BTCUSDT must NOT appear in feed-age lines.
    EXPECT_EQ(output.find("lob_feed_data_age_seconds{exchange=\"testex\",symbol=\"BTCUSDT\"}"),
              std::string::npos);
}

// HELP/TYPE headers must appear once when at least one symbol has data.
TEST_F(PrometheusFormatTest, DataAgeHeadersPresentWhenDataExists) {
    const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
    metrics["BTCUSDT"]->lastUpdateTimeMs.store(nowMs - 100);

    const auto output = build();
    EXPECT_NE(output.find("# HELP lob_feed_data_age_seconds"), std::string::npos);
    EXPECT_NE(output.find("# TYPE lob_feed_data_age_seconds gauge"), std::string::npos);
}

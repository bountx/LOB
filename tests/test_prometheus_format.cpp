#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>

#include "prometheus_format.hpp"

namespace {

// Builds a minimal JSON snapshot suitable for OrderBook::applySnapshot.
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

// Collect every non-comment, non-empty line from a Prometheus exposition block.
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

    void SetUp() override {
        metrics["BTCUSDT"] = std::make_unique<Metrics>();
        books["BTCUSDT"] = std::make_unique<OrderBook>();
    }

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
        EXPECT_NE(line.find("exchange=\"binance\""), std::string::npos) << line;
    }
}

TEST_F(PrometheusFormatTest, SymbolLabelAppearsOnEveryDataLine) {
    const auto lines = dataLines(build());
    ASSERT_FALSE(lines.empty());
    for (const auto& line : lines) {
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

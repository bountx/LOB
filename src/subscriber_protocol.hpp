#pragma once
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace subscriber {

struct ParsedRequest {
    std::string op;
    std::vector<std::string> streams;
};

// Parse "exchange.SYMBOL.book" → {exchange, symbol}.
/**
 * @brief Parse a stream descriptor of the form "exchange.SYMBOL.book" into exchange and symbol.
 *
 * @param stream Stream descriptor, expected in the format "exchange.SYMBOL.book".
 * @return std::optional<std::pair<std::string, std::string>> Containing {exchange, symbol} when
 * parsing succeeds; `std::nullopt` if the input is not in the expected format, the symbol is empty,
 * or the suffix is not "book".
 */
inline std::optional<std::pair<std::string, std::string>> parseStream(const std::string& stream) {
    size_t dot1 = stream.find('.');
    if (dot1 == std::string::npos || dot1 == 0) return std::nullopt;

    size_t dot2 = stream.find('.', dot1 + 1);
    if (dot2 == std::string::npos || dot2 == dot1 + 1) return std::nullopt;

    std::string_view suffix{stream.data() + dot2 + 1, stream.size() - dot2 - 1};
    if (suffix != "book") return std::nullopt;

    std::string exchange = stream.substr(0, dot1);
    std::string symbol = stream.substr(dot1 + 1, dot2 - dot1 - 1);
    if (symbol.empty()) return std::nullopt;

    return std::make_pair(std::move(exchange), std::move(symbol));
}

// Parse a subscribe/unsubscribe JSON message from a client.
// Expected: {"op": "subscribe"|"unsubscribe", "streams": ["..."]}
/**
 * @brief Parse a client JSON message into a ParsedRequest.
 *
 * Parses `msg` as JSON and extracts an `op` and a list of `streams`. Accepts only
 * `"subscribe"` or `"unsubscribe"` for `op` and requires `streams` to be an array
 * of strings.
 *
 * @param msg JSON text containing an `op` field and a `streams` array.
 * @return std::optional<ParsedRequest> A populated ParsedRequest when `msg` is a valid
 * request; `std::nullopt` if parsing fails, required fields are missing or have
 * the wrong types, `op` is not `"subscribe"` or `"unsubscribe"`, or any stream
 * element is not a string.
 */
inline std::optional<ParsedRequest> parseClientMessage(const std::string& msg) {
    try {
        auto j = nlohmann::json::parse(msg);
        if (!j.contains("op") || !j["op"].is_string()) return std::nullopt;
        if (!j.contains("streams") || !j["streams"].is_array()) return std::nullopt;

        ParsedRequest req;
        req.op = j["op"].get<std::string>();
        if (req.op != "subscribe" && req.op != "unsubscribe") return std::nullopt;

        for (const auto& s : j["streams"]) {
            if (!s.is_string()) return std::nullopt;
            req.streams.push_back(s.get<std::string>());
        }
        return req;
    } catch (...) {
        return std::nullopt;
    }
}

// Format a price or quantity stored as long long (value * 1e8) into a decimal string.
/**
 * @brief Formats an integer value scaled by 1e8 into a decimal string.
 *
 * Converts a scaled integer (true value = scaled / 100000000) into its
 * canonical decimal representation. Omits the fractional part when it is zero,
 * trims trailing zeros from the fractional part, and preserves a leading '-'
 * for negative values. Correctly handles the full `long long` range (including
 * `LLONG_MIN`).
 *
 * @return std::string Decimal string representation of the scaled value.
 */
inline std::string formatScaled(long long scaled) {
    constexpr unsigned long long SCALE = 100'000'000ULL;
    const bool sign = scaled < 0;
    // Cast to unsigned before negating: avoids UB when scaled == LLONG_MIN,
    // whose positive value cannot be represented as long long.
    const unsigned long long absScaled =
        sign ? -static_cast<unsigned long long>(scaled) : static_cast<unsigned long long>(scaled);
    const unsigned long long intPart = absScaled / SCALE;
    const unsigned long long fracPart = absScaled % SCALE;
    std::string result = std::to_string(intPart);
    if (fracPart != 0) {
        std::string frac = std::to_string(fracPart);
        while (frac.size() < 8) frac = "0" + frac;
        while (!frac.empty() && frac.back() == '0') frac.pop_back();
        result += "." + frac;
    }
    if (sign) result = "-" + result;
    return result;
}

// Convert an ordered book side map {scaled_price → scaled_qty} to a JSON array of
// ["price", "qty"] string pairs, preserving the map's iteration order.
template <typename Compare>
/**
 * @brief Convert an ordered map of scaled price→quantity levels into a JSON array.
 *
 * Each map entry becomes a two-element JSON array of strings [price, qty], where
 * price and qty are decimal strings derived from the stored scaled integers.
 * The output preserves the map's iteration order.
 *
 * @tparam Compare Comparison functor used to order the map keys.
 * @param levels Ordered map from price to quantity, where each value is a
 *               fixed-point integer scaled by 1e8.
 * @return nlohmann::json JSON array of two-element string arrays: [[price, qty], ...].
 */
inline nlohmann::json bookLevelsToJson(const std::map<long long, long long, Compare>& levels) {
    auto arr = nlohmann::json::array();
    for (const auto& [price, qty] : levels) {
        arr.push_back({formatScaled(price), formatScaled(qty)});
    }
    return arr;
}

/**
 * @brief Convert an ordered list of scaled price/quantity pairs into a JSON array of formatted
 * entries.
 *
 * Converts each pair {price, qty}, where values are fixed-point integers scaled by 1e8, into a
 * two-element JSON array [ "price", "qty" ] with both numbers rendered as canonical decimal
 * strings. The input order is preserved in the output.
 *
 * @param levels Ordered vector of {scaled_price, scaled_qty} pairs; each value is an integer
 * representing the real number multiplied by 1e8.
 * @return nlohmann::json A JSON array of two-element arrays, each containing the formatted price
 * and quantity as strings.
 */
inline nlohmann::json bookLevelsToJson(const std::vector<std::pair<long long, long long>>& levels) {
    auto arr = nlohmann::json::array();
    for (const auto& [price, qty] : levels) {
        arr.push_back({formatScaled(price), formatScaled(qty)});
    }
    return arr;
}

// Build a full order book snapshot message.
// seq: the last update sequence number reflected in this snapshot (0 if no updates yet).
//      Subscribers should apply only updates with seq > this value after receiving the snapshot.
// ofi_depth: number of top price levels per side used for OFI computation in the LOB.
//            Subscribers need this to validate their independent OFI reconstruction.
inline std::string buildSnapshot(std::string_view exchange, std::string_view symbol,
                                 long long timestamp, const nlohmann::json& bids,
                                 const nlohmann::json& asks, int ofi_depth, uint64_t seq) {
    nlohmann::json j;
    j["type"] = "snapshot";
    j["exchange"] = exchange;
    j["symbol"] = symbol;
    j["ts"] = timestamp;
    j["seq"] = seq;
    j["ofi_depth"] = ofi_depth;
    j["bids"] = bids;
    j["asks"] = asks;
    return j.dump();
}

// Build an incremental update message.
// ofi_delta: LOB-computed OFI change for this batch (native units, e.g. BTC).
//            Includes Genuine + Maintenance deltas within the OFI view; excludes Backfill.
//            Subscribers can accumulate this to cross-check against lob_ofi_value in Prometheus.
// seq: monotonically increasing per-stream counter; gaps indicate dropped messages.
inline std::string buildUpdate(std::string_view exchange, std::string_view symbol,
                               long long timestamp, const nlohmann::json& bids,
                               const nlohmann::json& asks, double ofi_delta, uint64_t seq) {
    nlohmann::json j;
    j["type"] = "update";
    j["exchange"] = exchange;
    j["symbol"] = symbol;
    j["ts"] = timestamp;
    j["seq"] = seq;
    j["ofi_delta"] = ofi_delta;
    j["bids"] = bids;
    j["asks"] = asks;
    return j.dump();
}

/**
 * @brief Build a JSON-formatted error message.
 *
 * Constructs a JSON object with fields "type" set to "error" and "message"
 * containing the provided text, then returns the object as a string.
 *
 * @param message Human-readable error message to include in the JSON.
 * @return std::string JSON string like {"type":"error","message":"..."}.
 */
inline std::string buildError(std::string_view message) {
    nlohmann::json j;
    j["type"] = "error";
    j["message"] = message;
    return j.dump();
}

}  // namespace subscriber

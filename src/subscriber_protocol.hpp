#pragma once
#include <cstdlib>
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
// Returns nullopt if the format is invalid or the suffix is not ".book".
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
// Returns nullopt on parse error, missing/wrong fields, or unknown op.
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
// E.g., 5000100000000 → "50001", 100000000 → "1", 5000000 → "0.05".
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
inline nlohmann::json bookLevelsToJson(const std::map<long long, long long, Compare>& levels) {
    auto arr = nlohmann::json::array();
    for (const auto& [price, qty] : levels) {
        arr.push_back({formatScaled(price), formatScaled(qty)});
    }
    return arr;
}

// Build a full order book snapshot message.
// bids/asks are JSON arrays of ["price","qty"] string pairs.
inline std::string buildSnapshot(std::string_view exchange, std::string_view symbol,
                                  long long timestamp, const nlohmann::json& bids,
                                  const nlohmann::json& asks) {
    nlohmann::json j;
    j["type"] = "snapshot";
    j["exchange"] = exchange;
    j["symbol"] = symbol;
    j["ts"] = timestamp;
    j["bids"] = bids;
    j["asks"] = asks;
    return j.dump();
}

// Build an incremental update message.
// bids/asks contain only changed levels; zero quantity means the level was removed.
inline std::string buildUpdate(std::string_view exchange, std::string_view symbol,
                                long long timestamp, const nlohmann::json& bids,
                                const nlohmann::json& asks) {
    nlohmann::json j;
    j["type"] = "update";
    j["exchange"] = exchange;
    j["symbol"] = symbol;
    j["ts"] = timestamp;
    j["bids"] = bids;
    j["asks"] = asks;
    return j.dump();
}

// Build an error response message.
inline std::string buildError(std::string_view message) {
    nlohmann::json j;
    j["type"] = "error";
    j["message"] = message;
    return j.dump();
}

}  // namespace subscriber

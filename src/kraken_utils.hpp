#pragma once
#include <array>
#include <cstdio>
#include <ctime>
#include <nlohmann/json.hpp>
#include <string>

#ifdef _WIN32
#define timegm _mkgmtime
#endif

namespace kraken {

// Convert a double price/qty to a fixed-8-decimal-place string suitable for parseDecimal().
inline std::string doubleToString(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.8f", v);
    return buf;
}

// Convert a Kraken level {"price": X, "qty": Y} to a ["price_str", "qty_str"] JSON array.
inline nlohmann::json levelToStringPair(const nlohmann::json& level) {
    return {doubleToString(level.at("price").get<double>()),
            doubleToString(level.at("qty").get<double>())};
}

// Convert a Kraken book side array [{"price":X,"qty":Y},...] to [["p","q"],...] string pairs.
inline nlohmann::json levelsToStringPairs(const nlohmann::json& krakenLevels) {
    auto arr = nlohmann::json::array();
    for (const auto& lvl : krakenLevels) {
        arr.push_back(levelToStringPair(lvl));
    }
    return arr;
}

// Parse a Kraken ISO 8601 timestamp "YYYY-MM-DDTHH:MM:SS.xxxxxxZ" to ms since epoch.
// Assumes 6 fractional-second digits (microseconds); returns 0 on parse failure.
inline long long isoTimestampToMs(const std::string& iso) {
    if (iso.size() < 19) return 0;
    std::tm tm = {};
    int us = 0;
    // NOLINTNEXTLINE(cert-err34-c)
    std::sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d.%dZ", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &us);
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = 0;
    time_t t = timegm(&tm);
    if (t == static_cast<time_t>(-1)) {
        return 0;
    }
    return (static_cast<long long>(t) * 1000LL) + (us / 1000);
}

// Clamp a requested depth to the nearest Kraken-supported book depth (10/25/100/500/1000).
// Always rounds down; values below 10 stay at 10.
inline int clampBookDepth(int requested) {
    constexpr std::array<int, 5> kValid = {10, 25, 100, 500, 1000};
    int best = kValid[0];
    for (int v : kValid) {
        if (v <= requested) {
            best = v;
        }
    }
    return best;
}

}  // namespace kraken

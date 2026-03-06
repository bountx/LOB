#pragma once
#include <algorithm>
#include <cctype>
#include <string>

// Extracts the symbol from a Binance combined-stream name and uppercases it.
//
// Binance stream names look like:
//   "btcusdt@depth"          (basic depth stream)
//   "ethusdt@depth@100ms"    (depth stream with interval suffix)
//
// Returns everything before the first '@', uppercased, which is the key
/**
 * Extracts the symbol portion from a Binance combined-stream name and returns it uppercased.
 *
 * The function takes the substring before the first '@' in `stream` and converts it to uppercase.
 * If `stream` contains no '@', the entire input is uppercased. If `stream` is empty, an empty
 * string is returned.
 *
 * @param stream Combined-stream name (e.g., "btcusdt@depth" or "ethusdt@aggTrade").
 * @return Uppercased symbol extracted from before the first '@' (or the entire uppercased input if no '@').
 */
inline std::string streamToSymbol(const std::string& stream) {
    std::string sym = stream.substr(0, stream.find('@'));
    std::transform(sym.begin(), sym.end(), sym.begin(),
                   [](unsigned char c) { return ::toupper(c); });
    return sym;
}

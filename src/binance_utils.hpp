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
// used in the order book and metrics maps.
inline std::string streamToSymbol(const std::string& stream) {
    std::string sym = stream.substr(0, stream.find('@'));
    std::transform(sym.begin(), sym.end(), sym.begin(),
                   [](unsigned char c) { return ::toupper(c); });
    return sym;
}

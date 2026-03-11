#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

// Maps between exchange-specific symbols and canonical BASE-QUOTE names.
//
// Canonical format: uppercase "BASE-QUOTE", e.g. "BTC-USDT", "ETH-USD".
//
// Built-in auto-conversion rules (no explicit entry needed):
//   Binance:
//     toCanonical:   split on trailing quote suffix  "BTCUSDT" → "BTC-USDT"
//                    (tries USDT/BUSD/USDC/USD/BTC/ETH/BNB/EUR in that order)
//     fromCanonical: remove '-'                      "BTC-USDT" → "BTCUSDT"
//
//   Kraken:
//     toCanonical:   replace '/' → '-'               "BTC/USDT" → "BTC-USDT"
//                    XBT-* → BTC-*                   "XBT/USDT" → "BTC-USDT"
//     fromCanonical: replace '-' → '/'               "BTC-USDT" → "BTC/USDT"
//
// Explicit entries added via add() always override auto-conversion.
class SymbolNormalizer {
public:
    // Register a bidirectional explicit mapping.
    // e.g. add("binance", "BTCUSDT", "BTC-USDT")
    void add(std::string exchange, std::string local, std::string canonical);

    // Convert an exchange-specific symbol to canonical form.
    // Returns nullopt if no mapping and no auto-rule applies.
    std::optional<std::string> toCanonical(std::string_view exchange, std::string_view local) const;

    // Convert a canonical symbol to the exchange-specific form.
    // Returns nullopt if no mapping and no auto-rule applies.
    std::optional<std::string> fromCanonical(std::string_view exchange,
                                             std::string_view canonical) const;

private:
    static std::string makeKey(std::string_view a, std::string_view b) {
        std::string k;
        k.reserve(a.size() + 1 + b.size());
        k.append(a);
        k += '\x01';
        k.append(b);
        return k;
    }

    std::unordered_map<std::string, std::string>
        toCanonicalMap_;  // "exchange\x01local" → canonical
    std::unordered_map<std::string, std::string>
        fromCanonicalMap_;  // "exchange\x01canonical" → local
};

inline void SymbolNormalizer::add(std::string exchange, std::string local, std::string canonical) {
    toCanonicalMap_[makeKey(exchange, local)] = canonical;
    fromCanonicalMap_[makeKey(exchange, canonical)] = local;
}

inline std::optional<std::string> SymbolNormalizer::toCanonical(std::string_view exchange,
                                                                std::string_view local) const {
    auto it = toCanonicalMap_.find(makeKey(exchange, local));
    if (it != toCanonicalMap_.end()) return it->second;

    if (exchange == "kraken") {
        std::string result(local);
        for (char& c : result) {
            if (c == '/') c = '-';
        }
        // Map Kraken's legacy "XBT" ticker for Bitcoin to canonical "BTC".
        if (result.starts_with("XBT-")) {
            result.replace(0, 3, "BTC");
        }
        return result;
    }

    if (exchange == "binance") {
        // Split by trying common quote-currency suffixes; longest suffixes first to
        // avoid mis-matching (e.g. "USDT" before "USD" prevents "BTCUSDT" → "BTCUSD-T").
        static constexpr std::string_view kQuotes[] = {"USDT", "BUSD", "USDC", "USD",
                                                       "BTC",  "ETH",  "BNB",  "EUR"};
        for (std::string_view q : kQuotes) {
            if (local.size() > q.size() && local.substr(local.size() - q.size()) == q) {
                return std::string(local.substr(0, local.size() - q.size())) + "-" + std::string(q);
            }
        }
    }

    return std::nullopt;
}

inline std::optional<std::string> SymbolNormalizer::fromCanonical(
    std::string_view exchange, std::string_view canonical) const {
    auto it = fromCanonicalMap_.find(makeKey(exchange, canonical));
    if (it != fromCanonicalMap_.end()) return it->second;

    if (exchange == "kraken") {
        std::string result(canonical);
        for (char& c : result) {
            if (c == '-') c = '/';
        }
        // Map canonical base currencies back to Kraken legacy tickers.
        if (result.starts_with("BTC/")) {
            result.replace(0, 3, "XBT");
        } else if (result.starts_with("DOGE/")) {
            result.replace(0, 4, "XDG");
        }
        return result;
    }

    if (exchange == "binance") {
        std::string result;
        result.reserve(canonical.size());
        for (char c : canonical) {
            if (c != '-') result += c;
        }
        return result;
    }

    return std::nullopt;
}

// Returns a SymbolNormalizer whose auto-conversion rules handle all standard
// Binance and Kraken pairs. Add explicit entries via add() to override edge cases.
inline SymbolNormalizer defaultNormalizer() { return SymbolNormalizer{}; }

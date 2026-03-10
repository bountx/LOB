#pragma once
#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>

#include "safe_arithmetic.hpp"

// Takes only the fractional digits (after the decimal point, no dot included).
// Pads to 8 decimal places by multiplying — no string allocation.
inline long long parseFractionalPart(std::string_view frac) {
    if (frac.empty()) return 0LL;
    if (frac.size() > 8)
        throw std::runtime_error("Too many fractional digits: " + std::string(frac));
    for (char c : frac) {
        if (c < '0' || c > '9')
            throw std::runtime_error("Failed to parse fractional part: " + std::string(frac));
    }
    long long fracPart = 0;
    auto result = std::from_chars(frac.data(), frac.data() + frac.size(), fracPart);
    if (result.ec != std::errc() || result.ptr != frac.data() + frac.size())
        throw std::runtime_error("Failed to parse fractional part: " + std::string(frac));
    // Multiply by 10^(8 - len) to pad to 8 fractional digits.
    static constexpr long long kPow10[9] = {100000000LL, 10000000LL, 1000000LL, 100000LL, 10000LL,
                                            1000LL,      100LL,      10LL,      1LL};
    return fracPart * kPow10[frac.size()];
}

// Accepts std::string_view (no copy needed); std::string implicitly converts.
inline long long parseDecimal(std::string_view str) {
    constexpr long long SCALE = 100000000LL;
    bool negative = !str.empty() && str[0] == '-';
    auto dotPos = str.find('.');
    if (dotPos == std::string_view::npos) {
        long long value = 0;
        auto result = std::from_chars(str.data(), str.data() + str.size(), value);
        if (result.ec != std::errc() || result.ptr != str.data() + str.size())
            throw std::runtime_error("Failed to parse integer: " + std::string(str));
        return safeMultiply(value, SCALE, "parseDecimal");
    }
    if (str == ".") throw std::runtime_error("Failed to parse decimal: .");

    long long intPart = 0;
    if (dotPos > 0) {
        auto result = std::from_chars(str.data(), str.data() + dotPos, intPart);
        if (result.ec != std::errc() || result.ptr != str.data() + dotPos)
            throw std::runtime_error("Failed to parse integer part: " + std::string(str));
    }
    // str.substr() on string_view returns string_view — zero copy.
    long long fracPart = parseFractionalPart(str.substr(dotPos + 1));
    return negative
               ? safeSubstract(safeMultiply(intPart, SCALE, "parseDecimal"), fracPart,
                               "parseDecimal")
               : safeAdd(safeMultiply(intPart, SCALE, "parseDecimal"), fracPart, "parseDecimal");
}

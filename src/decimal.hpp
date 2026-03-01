#pragma once
#include <charconv>
#include <stdexcept>
#include <string>

#include "safe_arithmetic.hpp"

inline long long parseDecimal(const std::string& str) {
    constexpr long long SCALE = 100000000LL;
    bool negative = !str.empty() && str[0] == '-';
    size_t dotPos = str.find('.');
    if (dotPos == std::string::npos) {
        // No decimal point
        long long value;
        auto result = std::from_chars(str.data(), str.data() + str.size(), value);
        if (result.ec != std::errc() || result.ptr != str.data() + str.size()) {
            throw std::runtime_error("Failed to parse integer: " + str);
        }
        return safeMultiply(value, SCALE, "parseDecimal");
    } else {
        if (str == ".") {
            throw std::runtime_error("Failed to parse decimal: " + str);
        }

        // Parse integer part
        long long intPart = 0;
        if (dotPos > 0) {
            auto result = std::from_chars(str.data(), str.data() + dotPos, intPart);
            if (result.ec != std::errc() || result.ptr != str.data() + dotPos) {
                throw std::runtime_error("Failed to parse integer part: " + str);
            }
        }

        // Parse fractional part (up to 8 digits)
        long long fracPart = 0;
        if (dotPos + 1 < str.size()) {
            std::string fracStr = str.substr(dotPos + 1);
            if (fracStr.find_first_not_of("0123456789") != std::string::npos) {
                throw std::runtime_error("Failed to parse fractional part: " + str);
            }
            if (fracStr.length() > 8) {
                throw std::runtime_error("Too many fractional digits: " + str);
            }
            while (fracStr.length() < 8) fracStr += '0';
            auto result =
                std::from_chars(fracStr.data(), fracStr.data() + fracStr.size(), fracPart);
            if (result.ec != std::errc() || result.ptr != fracStr.data() + fracStr.size()) {
                throw std::runtime_error("Failed to parse fractional part: " + str);
            }
        }

        // For negative intPart, subtract fracPart instead of adding
        return negative ? safeSubstract(safeMultiply(intPart, SCALE, "parseDecimal"), fracPart,
                                        "parseDecimal")
                        : safeAdd(safeMultiply(intPart, SCALE, "parseDecimal"), fracPart,
                                  "parseDecimal");
    }
};
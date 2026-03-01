#pragma once
#include <charconv>
#include <stdexcept>
#include <string>

inline long long parseDecimal(const std::string& str) {
    size_t dotPos = str.find('.');
    if (dotPos == std::string::npos) {
        // No decimal point
        long long value;
        auto result = std::from_chars(str.data(), str.data() + str.size(), value);
        if (result.ec != std::errc()) {
            throw std::runtime_error("Failed to parse integer: " + str);
        }
        return value * 100000000;
    } else {
        // Parse integer part
        long long intPart = 0;
        if (dotPos > 0) {
            auto result = std::from_chars(str.data(), str.data() + dotPos, intPart);
            if (result.ec != std::errc()) {
                throw std::runtime_error("Failed to parse integer part: " + str);
            }
        }

        // Parse fractional part (up to 8 digits)
        long long fracPart = 0;
        if (dotPos + 1 < str.size()) {
            std::string fracStr = str.substr(dotPos + 1);
            while (fracStr.length() < 8) fracStr += '0';
            fracStr = fracStr.substr(0, 8);
            auto result =
                std::from_chars(fracStr.data(), fracStr.data() + fracStr.size(), fracPart);
            if (result.ec != std::errc()) {
                throw std::runtime_error("Failed to parse fractional part: " + str);
            }
        }

        return intPart * 100000000 + fracPart;
    }
};
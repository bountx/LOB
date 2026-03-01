#pragma once
#include <limits>
#include <stdexcept>
#include <string>

template <typename T>
inline T safeMultiply(T a, T b, const std::string& context = "") {
    if (a > 0 && b > 0 && a > std::numeric_limits<T>::max() / b) {
        throw std::overflow_error("Overflow in multiplication" +
                                  (context.empty() ? "" : ": " + context));
    }
    if (a < 0 && b < 0 && a < std::numeric_limits<T>::max() / b) {
        throw std::overflow_error("Overflow in multiplication" +
                                  (context.empty() ? "" : ": " + context));
    }
    if (a > 0 && b < 0 && b < std::numeric_limits<T>::lowest() / a) {
        throw std::overflow_error("Underflow in multiplication" +
                                  (context.empty() ? "" : ": " + context));
    }
    if (a < 0 && b > 0 && a < std::numeric_limits<T>::lowest() / b) {
        throw std::overflow_error("Underflow in multiplication" +
                                  (context.empty() ? "" : ": " + context));
    }
    return a * b;
}

template <typename T>
inline T safeAdd(T a, T b, const std::string& context = "") {
    if (b > 0 && a > std::numeric_limits<T>::max() - b) {
        throw std::overflow_error("Overflow in addition" + (context.empty() ? "" : ": " + context));
    }
    if (b < 0 && a < std::numeric_limits<T>::lowest() - b) {
        throw std::overflow_error("Underflow in addition" +
                                  (context.empty() ? "" : ": " + context));
    }
    return a + b;
}

template <typename T>
inline T safeSubstract(T a, T b, const std::string& context = "") {
    if (b < 0 && a > std::numeric_limits<T>::max() + b) {
        throw std::overflow_error("Overflow in subtraction" +
                                  (context.empty() ? "" : ": " + context));
    }
    if (b > 0 && a < std::numeric_limits<T>::lowest() + b) {
        throw std::overflow_error("Underflow in subtraction" +
                                  (context.empty() ? "" : ": " + context));
    }
    return a - b;
}
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpragma-once-outside-header"
#endif
#pragma once
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <charconv>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <cmath>
#include <type_traits>

namespace praccy::utils {

/// Strips leading and trailing whitespace characters (' ', '\t', '\r', '\n')
[[nodiscard]] constexpr std::string_view trim(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
        sv.remove_suffix(1);
    }
    return sv;
}

/// Non-throwing integer parser using C++20 std::from_chars with safe fallback default
template <typename T>
[[nodiscard]] inline T parseInteger(std::string_view sv, T defaultValue = T{}, int base = 10) noexcept {
    static_assert(std::is_integral_v<T>, "parseInteger requires an integral type");
    sv = trim(sv);
    if (sv.empty()) return defaultValue;

    // std::from_chars does not accept leading '+' sign by ISO standard.
    // Ensure leading '+' is only stripped if followed by a digit. If followed by '-' or non-digit, return defaultValue.
    if (sv.front() == '+') {
        if (sv.size() < 2) return defaultValue;
        char next = sv[1];
        bool isDigit = false;
        if (base <= 10) {
            isDigit = (next >= '0' && next < ('0' + base));
        } else {
            isDigit = (next >= '0' && next <= '9') ||
                      (next >= 'a' && next < ('a' + base - 10)) ||
                      (next >= 'A' && next < ('A' + base - 10));
        }
        if (!isDigit) return defaultValue;
        sv.remove_prefix(1);
    }

    T result = defaultValue;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result, base);
    if (ec == std::errc() && ptr == (sv.data() + sv.size())) {
        return result;
    }
    return defaultValue;
}

/// Non-throwing float parser using C++20 std::from_chars with safe fallback default
[[nodiscard]] inline float parseFloat(std::string_view sv, float defaultValue = 0.0f) noexcept {
    sv = trim(sv);
    if (sv.empty()) return defaultValue;

    if (sv.front() == '+') {
        if (sv.size() < 2) return defaultValue;
        char next = sv[1];
        if (!((next >= '0' && next <= '9') || next == '.')) return defaultValue;
        sv.remove_prefix(1);
    }

    float result = defaultValue;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result);
    if (ec == std::errc() && ptr == (sv.data() + sv.size())) {
        if (!std::isfinite(result)) return defaultValue;
        return result;
    }
    return defaultValue;
}

/// Non-throwing double parser using C++20 std::from_chars with safe fallback default
[[nodiscard]] inline double parseDouble(std::string_view sv, double defaultValue = 0.0) noexcept {
    sv = trim(sv);
    if (sv.empty()) return defaultValue;

    if (sv.front() == '+') {
        if (sv.size() < 2) return defaultValue;
        char next = sv[1];
        if (!((next >= '0' && next <= '9') || next == '.')) return defaultValue;
        sv.remove_prefix(1);
    }

    double result = defaultValue;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result);
    if (ec == std::errc() && ptr == (sv.data() + sv.size())) {
        if (!std::isfinite(result)) return defaultValue;
        return result;
    }
    return defaultValue;
}

/// Non-throwing 2-character hex byte parser (e.g. "A5" -> 0xA5)
[[nodiscard]] inline bool parseHexByte(std::string_view sv, uint8_t& outByte) noexcept {
    if (sv.size() != 2) return false;
    uint8_t val = 0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + 2, val, 16);
    if (ec == std::errc() && ptr == (sv.data() + 2)) {
        outByte = val;
        return true;
    }
    return false;
}

/// Non-throwing hex string to byte vector decoder
/// Returns empty vector on odd length or corrupted non-hex characters
[[nodiscard]] inline std::vector<uint8_t> hexToBytes(std::string_view hex) {
    hex = trim(hex);
    std::vector<uint8_t> bytes;
    if (hex.length() % 2 != 0) return bytes;
    bytes.reserve(hex.length() / 2);
    for (size_t i = 0; i < hex.length(); i += 2) {
        uint8_t b = 0;
        if (!parseHexByte(hex.substr(i, 2), b)) {
            bytes.clear(); // Corrupted hex stream, fail safe
            return bytes;
        }
        bytes.push_back(b);
    }
    return bytes;
}

/// Escapes delimiter characters ('\', '|', '=', ',', '\n', '\r') in INI field values using collision-free hex escapes
[[nodiscard]] inline std::string escapeIniField(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (unsigned char c : input) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '|':  out += "\\x7C"; break;
            case '=':  out += "\\x3D"; break;
            case ',':  out += "\\x2C"; break;
            case '\n': out += "\\x0A"; break;
            case '\r': out += "\\x0D"; break;
            default:   out.push_back(static_cast<char>(c)); break;
        }
    }
    return out;
}

/// Decodes hex-escaped delimiter sequences in INI field values while preserving unescaped Windows paths untouched
[[nodiscard]] inline std::string unescapeIniField(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\\' && i + 1 < input.size()) {
            const char next = input[i + 1];
            if (next == '\\') {
                out.push_back('\\');
                ++i;
                continue;
            }
            if ((next == 'x' || next == 'X') && i + 3 < input.size()) {
                uint8_t b = 0;
                if (parseHexByte(input.substr(i + 2, 2), b)) {
                    if (b == 0x7C || b == 0x3D || b == 0x2C || b == 0x0A || b == 0x0D || b == 0x5C) {
                        out.push_back(static_cast<char>(b));
                        i += 3;
                        continue;
                    }
                }
            }
            out.push_back('\\');
        } else {
            out.push_back(input[i]);
        }
    }
    return out;
}

} // namespace praccy::utils

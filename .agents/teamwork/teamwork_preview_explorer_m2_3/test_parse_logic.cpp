#include <charconv>
#include <string_view>
#include <vector>
#include <cstdint>
#include <cassert>
#include <iostream>
#include <cmath>

namespace praccy::utils {

inline std::string_view trim(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
        sv.remove_suffix(1);
    }
    return sv;
}

template <typename T>
inline T parseInteger(std::string_view sv, T defaultValue = T{}, int base = 10) noexcept {
    sv = trim(sv);
    if (sv.empty()) return defaultValue;
    if (sv.front() == '+') {
        sv.remove_prefix(1);
        if (sv.empty()) return defaultValue;
    }
    T result = defaultValue;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result, base);
    if (ec == std::errc() && ptr == (sv.data() + sv.size())) {
        return result;
    }
    return defaultValue;
}

inline float parseFloat(std::string_view sv, float defaultValue = 0.0f) noexcept {
    sv = trim(sv);
    if (sv.empty()) return defaultValue;
    if (sv.front() == '+') {
        sv.remove_prefix(1);
        if (sv.empty()) return defaultValue;
    }
    float result = defaultValue;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result);
    if (ec == std::errc() && ptr == (sv.data() + sv.size())) {
        return result;
    }
    return defaultValue;
}

inline double parseDouble(std::string_view sv, double defaultValue = 0.0) noexcept {
    sv = trim(sv);
    if (sv.empty()) return defaultValue;
    if (sv.front() == '+') {
        sv.remove_prefix(1);
        if (sv.empty()) return defaultValue;
    }
    double result = defaultValue;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result);
    if (ec == std::errc() && ptr == (sv.data() + sv.size())) {
        return result;
    }
    return defaultValue;
}

inline bool parseHexByte(std::string_view sv, uint8_t& outByte) noexcept {
    if (sv.size() != 2) return false;
    uint8_t val = 0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + 2, val, 16);
    if (ec == std::errc() && ptr == (sv.data() + 2)) {
        outByte = val;
        return true;
    }
    return false;
}

inline std::vector<uint8_t> hexToBytes(std::string_view hex) {
    hex = trim(hex);
    std::vector<uint8_t> bytes;
    if (hex.length() % 2 != 0) return bytes;
    bytes.reserve(hex.length() / 2);
    for (size_t i = 0; i < hex.length(); i += 2) {
        uint8_t b = 0;
        if (!parseHexByte(hex.substr(i, 2), b)) {
            bytes.clear(); // corrupted hex stream, fail safe
            return bytes;
        }
        bytes.push_back(b);
    }
    return bytes;
}

} // namespace praccy::utils

int main() {
    using namespace praccy::utils;
    assert(parseInteger<int>("  42  ") == 42);
    assert(parseInteger<int>(" -10 ") == -10);
    assert(parseInteger<int>(" +50 ") == 50);
    assert(parseInteger<int>("abc", 99) == 99);
    assert(parseInteger<int>("123xyz", 99) == 99);
    assert(parseInteger<size_t>("100", 0) == 100);

    assert(std::abs(parseFloat(" 3.1415 ", 0.0f) - 3.1415f) < 1e-4f);
    assert(std::abs(parseFloat(" -0.5 ", 0.0f) - (-0.5f)) < 1e-5f);
    assert(std::abs(parseFloat(" +2.5 ", 0.0f) - 2.5f) < 1e-5f);
    assert(parseFloat("not_a_float", 1.23f) == 1.23f);

    auto bytes = hexToBytes("010AFF");
    assert(bytes.size() == 3);
    assert(bytes[0] == 0x01 && bytes[1] == 0x0A && bytes[2] == 0xFF);

    auto badBytes1 = hexToBytes("010");
    assert(badBytes1.empty());

    auto badBytes2 = hexToBytes("010AZZ");
    assert(badBytes2.empty());

    std::cout << "All parse_utils unit checks PASSED!\n";
    return 0;
}

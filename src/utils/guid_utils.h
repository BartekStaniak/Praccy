#pragma once

#include <string>
#include <string_view>
#include <cctype>
#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>

namespace praccy::utils {

enum class DawGuidFormat {
    SteinbergFUID,   // Microsoft mixed-endian GUID or raw Steinberg FUID
    AbletonBigEndian,// Raw 16-byte big-endian hex string
    DawprojectUuid   // RFC 4122 hyphenated string
};

class GuidUtils {
public:
    // Normalizes foreign representations to lowercase 32-char hex string
    static std::string normalizeToCanonicalGuid(std::string_view input, DawGuidFormat format = DawGuidFormat::SteinbergFUID) {
        std::string hex;
        hex.reserve(32);

        for (char c : input) {
            if (c == '{' || c == '}' || c == '-' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                continue;
            }
            if (std::isxdigit(static_cast<unsigned char>(c))) {
                hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        }

        if (hex.size() != 32) {
            return {};
        }

        if (format == DawGuidFormat::SteinbergFUID && input.find('-') != std::string_view::npos) {
            // Windows mixed-endian GUID {Data1(4)-Data2(2)-Data3(2)-Data4(2)-Data4(6)}
            // Swap byte endianness for Data1, Data2, Data3 to produce canonical big-endian byte sequence
            std::string canonical = hex;
            // Swap Data1 (first 4 bytes = 8 hex chars: 0..7)
            swapBytes(canonical, 0, 6);
            swapBytes(canonical, 2, 4);
            // Swap Data2 (next 2 bytes = 4 hex chars: 8..11)
            swapBytes(canonical, 8, 10);
            // Swap Data3 (next 2 bytes = 4 hex chars: 12..15)
            swapBytes(canonical, 12, 14);
            return canonical;
        }

        return hex;
    }

    static std::string toSteinbergGuidString(std::string_view canonicalGuid) {
        std::string clean = normalizeToCanonicalGuid(canonicalGuid, DawGuidFormat::AbletonBigEndian);
        if (clean.size() != 32) return {};

        // Convert big-endian canonical to Windows mixed-endian GUID format
        std::string mixed = clean;
        swapBytes(mixed, 0, 6);
        swapBytes(mixed, 2, 4);
        swapBytes(mixed, 8, 10);
        swapBytes(mixed, 12, 14);

        std::string out = "{";
        out += mixed.substr(0, 8);
        out += "-";
        out += mixed.substr(8, 4);
        out += "-";
        out += mixed.substr(12, 4);
        out += "-";
        out += mixed.substr(16, 4);
        out += "-";
        out += mixed.substr(20, 12);
        out += "}";
        for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
    }

    static std::string toAbletonGuidString(std::string_view canonicalGuid) {
        std::string clean = normalizeToCanonicalGuid(canonicalGuid, DawGuidFormat::AbletonBigEndian);
        for (char& c : clean) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return clean;
    }

    static std::string toDawprojectUuidString(std::string_view canonicalGuid) {
        std::string clean = normalizeToCanonicalGuid(canonicalGuid, DawGuidFormat::AbletonBigEndian);
        if (clean.size() != 32) return {};

        std::string out;
        out.reserve(36);
        out += clean.substr(0, 8);
        out += "-";
        out += clean.substr(8, 4);
        out += "-";
        out += clean.substr(12, 4);
        out += "-";
        out += clean.substr(16, 4);
        out += "-";
        out += clean.substr(20, 12);
        return out;
    }

private:
    static void swapBytes(std::string& s, size_t posA, size_t posB) {
        if (posA + 1 < s.size() && posB + 1 < s.size()) {
            std::swap(s[posA], s[posB]);
            std::swap(s[posA + 1], s[posB + 1]);
        }
    }
};

} // namespace praccy::utils

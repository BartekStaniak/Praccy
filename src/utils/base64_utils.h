#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <limits>

namespace praccy::utils {

class Base64 {
public:
    static std::string encode(const std::vector<uint8_t>& data) {
        return encode(data.data(), data.size());
    }

    static std::string encode(const uint8_t* data, size_t length) {
        if (!data || length == 0) return {};

        static constexpr char kTable[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

        // Prevent overflow during output size calculation
        if (length > (std::numeric_limits<size_t>::max() / 4) * 3) {
            return {};
        }

        const size_t outLen = 4 * ((length + 2) / 3);
        std::string result;
        result.resize(outLen);

        size_t rIdx = 0;
        for (size_t i = 0; i < length; i += 3) {
            const uint32_t b0 = data[i];
            const uint32_t b1 = (i + 1 < length) ? data[i + 1] : 0;
            const uint32_t b2 = (i + 2 < length) ? data[i + 2] : 0;
            const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;

            result[rIdx++] = kTable[(triple >> 18) & 0x3F];
            result[rIdx++] = kTable[(triple >> 12) & 0x3F];
            result[rIdx++] = (i + 1 < length) ? kTable[(triple >> 6) & 0x3F] : '=';
            result[rIdx++] = (i + 2 < length) ? kTable[triple & 0x3F] : '=';
        }

        return result;
    }

    static bool decode(std::string_view base64Str, std::vector<uint8_t>& outBytes) {
        outBytes.clear();
        if (base64Str.empty()) return true;

        static constexpr int8_t kDecodeTable[256] = {
            -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, // 0-15
            -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, // 16-31
            -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,62, -1,-1,-1,63, // 32-47 ('+', '/')
            52,53,54,55, 56,57,58,59, 60,61,-1,-1, -1, 0,-1,-1, // 48-63 ('0'-'9', '=')
            -1, 0, 1, 2,  3, 4, 5, 6,  7, 8, 9,10, 11,12,13,14, // 64-79 ('A'-'O')
            15,16,17,18, 19,20,21,22, 23,24,25,-1, -1,-1,-1,-1, // 80-95 ('P'-'Z')
            -1,26,27,28, 29,30,31,32, 33,34,35,36, 37,38,39,40, // 96-111 ('a'-'o')
            41,42,43,44, 45,46,47,48, 49,50,51,-1, -1,-1,-1,-1, // 112-127 ('p'-'z')
            -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
            -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
            -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
            -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
            -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
            -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
            -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
            -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1
        };

        // Pre-count valid non-whitespace chars to reserve memory safely
        size_t nonWsCount = 0;
        for (char ch : base64Str) {
            const unsigned char uc = static_cast<unsigned char>(ch);
            if (uc > ' ') {
                nonWsCount++;
            }
        }

        if (nonWsCount % 4 != 0) {
            return false; // Invalid base64 block alignment
        }

        outBytes.reserve((nonWsCount / 4) * 3);

        uint32_t quartet = 0;
        int bitsCollected = 0;
        int padCount = 0;

        for (char ch : base64Str) {
            const unsigned char uc = static_cast<unsigned char>(ch);
            if (uc <= ' ') {
                continue; // Ignore whitespace without allocations
            }

            if (ch == '=') {
                padCount++;
                quartet <<= 6;
                bitsCollected += 6;
            } else {
                if (padCount > 0) {
                    // Non-padding char after padding is invalid
                    outBytes.clear();
                    return false;
                }
                const int8_t val = kDecodeTable[uc];
                if (val < 0) {
                    outBytes.clear();
                    return false; // Invalid base64 character
                }
                quartet = (quartet << 6) | static_cast<uint32_t>(val);
                bitsCollected += 6;
            }

            if (bitsCollected == 24) {
                if (padCount == 0) {
                    outBytes.push_back(static_cast<uint8_t>((quartet >> 16) & 0xFF));
                    outBytes.push_back(static_cast<uint8_t>((quartet >> 8) & 0xFF));
                    outBytes.push_back(static_cast<uint8_t>(quartet & 0xFF));
                } else if (padCount == 1) {
                    outBytes.push_back(static_cast<uint8_t>((quartet >> 16) & 0xFF));
                    outBytes.push_back(static_cast<uint8_t>((quartet >> 8) & 0xFF));
                } else if (padCount == 2) {
                    outBytes.push_back(static_cast<uint8_t>((quartet >> 16) & 0xFF));
                } else {
                    outBytes.clear();
                    return false; // More than 2 padding chars in a block
                }
                quartet = 0;
                bitsCollected = 0;
                padCount = 0;
            }
        }

        return true;
    }
};

} // namespace praccy::utils

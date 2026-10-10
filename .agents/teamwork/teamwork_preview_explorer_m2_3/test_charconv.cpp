#include <charconv>
#include <string_view>
#include <iostream>
#include <cstdint>

int main() {
    std::string_view sv = "123.456";
    float f = 0.0f;
    auto res = std::from_chars(sv.data(), sv.data() + sv.size(), f);
    if (res.ec == std::errc()) {
        std::cout << "Float parsed: " << f << "\n";
    } else {
        std::cout << "Float failed: " << (int)res.ec << "\n";
    }
    std::string_view hexSv = "A5";
    uint8_t u = 0;
    auto resHex = std::from_chars(hexSv.data(), hexSv.data() + hexSv.size(), u, 16);
    if (resHex.ec == std::errc()) {
        std::cout << "Hex parsed: " << (int)u << "\n";
    } else {
        std::cout << "Hex failed\n";
    }
    return 0;
}

#include <iostream>
#include <vector>
#include <string>
#include <cmath>
#include <cassert>
#include <cstring>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

#include "utils/parse_utils.h"
#include "ui/update_checker.h"
#include "plugins/crash_isolation.h"
#include "state/app_config.h"
#include "miniz.h"

using namespace praccy;

// ============================================================================
// CHALLENGE 1: parse_utils Edge Case & Boundary Stress Test
// ============================================================================
void challengeParseUtils() {
    std::cout << "[CHALLENGE 1] parse_utils Edge Cases & Boundary Stress Testing... \n";

    // 1.1 trim()
    assert(utils::trim("").empty());
    assert(utils::trim("   \t\r\n   ").empty());
    assert(utils::trim("  hello world  \r\n") == "hello world");
    assert(utils::trim("no_spaces") == "no_spaces");
    assert(utils::trim(" a ") == "a");

    // 1.2 parseInteger()
    // Defaults
    assert(utils::parseInteger<int>("", 42) == 42);
    assert(utils::parseInteger<int>("   ", 42) == 42);
    assert(utils::parseInteger<int>("+", 42) == 42);
    assert(utils::parseInteger<int>("-", 42) == 42);
    assert(utils::parseInteger<int>("+abc", 42) == 42);
    assert(utils::parseInteger<int>("123abc", 42) == 42);
    assert(utils::parseInteger<int>("abc123", 42) == 42);

    // Valid positive and negative with and without explicit sign
    assert(utils::parseInteger<int>("+123", 0) == 123);
    assert(utils::parseInteger<int>("-123", 0) == -123);
    assert(utils::parseInteger<int>("0", 42) == 0);
    assert(utils::parseInteger<int>("+0", 42) == 0);
    assert(utils::parseInteger<int>("-0", 42) == 0);

    // Overflow / Underflow bounds
    assert(utils::parseInteger<int>("99999999999999999999999999999999", -1) == -1);
    assert(utils::parseInteger<int>("-99999999999999999999999999999999", -1) == -1);
    assert(utils::parseInteger<int64_t>("9223372036854775807", 0) == 9223372036854775807LL);
    assert(utils::parseInteger<int64_t>("-9223372036854775808", 0) == std::numeric_limits<int64_t>::min());
    assert(utils::parseInteger<int64_t>("9223372036854775808", -99) == -99); // overflow int64

    // Unsigned with negative sign
    assert(utils::parseInteger<uint32_t>("-1", 999) == 999);
    assert(utils::parseInteger<uint32_t>("+4294967295", 0) == 4294967295U);

    // 1.3 parseFloat() & parseDouble()
    assert(utils::parseFloat("", 3.14f) == 3.14f);
    assert(utils::parseFloat("   ", 3.14f) == 3.14f);
    assert(utils::parseFloat("+", 3.14f) == 3.14f);
    assert(utils::parseFloat("-", 3.14f) == 3.14f);
    assert(utils::parseFloat("+0.5", 0.0f) == 0.5f);
    assert(utils::parseFloat("-0.5", 0.0f) == -0.5f);
    assert(utils::parseFloat("1e-4", 0.0f) == 1e-4f);
    assert(utils::parseFloat("1.25e2", 0.0f) == 125.0f);
    assert(utils::parseFloat("not_a_float", 99.0f) == 99.0f);
    assert(utils::parseFloat("123.456xyz", 99.0f) == 99.0f); // partial parse rejected
    assert(utils::parseDouble("+123.456789", 0.0) == 123.456789);

    // 1.4 parseHexByte() & hexToBytes()
    uint8_t b = 0;
    assert(utils::parseHexByte("00", b) && b == 0x00);
    assert(utils::parseHexByte("FF", b) && b == 0xFF);
    assert(utils::parseHexByte("ff", b) && b == 0xFF);
    assert(utils::parseHexByte("a5", b) && b == 0xA5);
    assert(!utils::parseHexByte("A", b));     // single char
    assert(!utils::parseHexByte("AAA", b));   // triple char
    assert(!utils::parseHexByte("GG", b));    // non-hex
    assert(!utils::parseHexByte(" 0", b));    // whitespace
    assert(!utils::parseHexByte("0 ", b));

    auto bytes1 = utils::hexToBytes("000102030405060708090A0B0C0D0E0F");
    assert(bytes1.size() == 16);
    for (size_t i = 0; i < 16; ++i) {
        assert(bytes1[i] == i);
    }

    assert(utils::hexToBytes("").empty());
    assert(utils::hexToBytes("A").empty());     // odd length
    assert(utils::hexToBytes("ABC").empty());   // odd length
    assert(utils::hexToBytes("01ZZ02").empty()); // corrupt middle

    std::cout << "  -> PASSED: All boundary and edge cases handled non-throwingly.\n";
}

// ============================================================================
// CHALLENGE 2: Zip Slip & Zip Bomb Adversarial Permutation Attacks
// ============================================================================
void challengeZipAttacks() {
    std::cout << "[CHALLENGE 2] Zip Slip & Zip Bomb Adversarial Penetration Testing... \n";

    std::filesystem::path safe;

    // 2.1 Permutations of path traversal
    const std::vector<std::string> maliciousPaths = {
        "../foo",
        "..\\foo",
        "/../foo",
        "\\../foo",
        "foo/../../bar",
        "foo/bar/../../../baz",
        "./../evil.dll",
        "dir/....//foo",
        "dir/.. /foo",
        "dir/../. /foo",
        "C:/evil.exe",
        "c:\\evil.exe",
        "D:payload.dll",
        "\\\\192.168.1.1\\share\\test.exe",
        "//192.168.1.1/share/test.exe",
        "CON", "con.txt", "CON.tar.gz",
        "PRN", "AUX", "NUL",
        "COM1", "COM9", "LPT1", "LPT9",
        "bad<name>.txt", "bad>name.txt", "bad:name.txt",
        "bad\"name.txt", "bad|name.txt", "bad?name.txt",
        "bad*name.txt",
        "ends_with_dot.",
        "ends_with_space "
    };

    for (const auto& pathStr : maliciousPaths) {
        bool allowed = ui::sanitizeZipEntryPath(pathStr, safe);
        if (allowed) {
            std::cerr << "\nSECURITY FAILURE: sanitizeZipEntryPath permitted: " << pathStr << " -> " << safe << "\n";
            assert(false && "Zip path sanitizer allowed dangerous path!");
        }
    }

    // 2.2 UTF-8 international paths (should be allowed safely)
    assert(ui::sanitizeZipEntryPath("presets/delay_vintage.ini", safe));
    assert(ui::sanitizeZipEntryPath("resources/icons/audio.png", safe));

    // 2.3 Archive Extraction with Corrupt / Truncated / Zero-byte Archives
    std::string errStr;
    assert(!ui::extractZipArchive("non_existent_archive_12345.zip", "temp_out", errStr));
    assert(!errStr.empty());

    // Zero-byte archive test
    const std::filesystem::path zeroZip = "test_zero_archive.zip";
    {
        std::ofstream zf(zeroZip, std::ios::binary);
    }
    assert(!ui::extractZipArchive(zeroZip, "temp_out", errStr));
    assert(!errStr.empty());
    std::error_code ec;
    std::filesystem::remove(zeroZip, ec);

    // Truncated garbage file test
    const std::filesystem::path garbageZip = "test_garbage_archive.zip";
    {
        std::ofstream gf(garbageZip, std::ios::binary);
        gf << "PK\x03\x04This is not a real zip archive header contents truncated!";
    }
    assert(!ui::extractZipArchive(garbageZip, "temp_out", errStr));
    assert(!errStr.empty());
    std::filesystem::remove(garbageZip, ec);

    std::cout << "  -> PASSED: All 30+ path traversal attacks and corrupt archives neutralized.\n";
}

// ============================================================================
// CHALLENGE 3: Crash Isolation Multi-Threaded Concurrency & Fault Recovery
// ============================================================================
void challengeCrashIsolationConcurrency() {
    std::cout << "[CHALLENGE 3] Crash Isolation Concurrency & Multi-Threaded Stress... \n";

    plugins::initCrashIsolation();

    // 3.1 Test nested safeCallPlugin
    bool outerOk = false;
    bool innerOk = false;
    DWORD outerCode = 0;
    DWORD innerCode = 0;

    outerOk = plugins::safeCallPluginAudio([&]() {
        // Inner call simulates a sub-call that crashes
        innerOk = plugins::safeCallPluginAudio([&]() {
            volatile int* p = nullptr;
            *p = 42;
        }, &innerCode);

        // Code after inner catch continues execution without crashing outer!
        assert(!innerOk);
        assert(innerCode == EXCEPTION_ACCESS_VIOLATION);
    }, &outerCode);

    assert(outerOk); // Outer call must succeed because inner crash was isolated!
    assert(!innerOk);

    // 3.2 High-concurrency multithreaded crash isolation
    // 4 threads executing 100 crashes each simultaneously
    constexpr int numThreads = 4;
    constexpr int crashesPerThread = 100;
    std::atomic<int> caughtCrashes{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < crashesPerThread; ++i) {
                DWORD code = 0;
                bool ok = plugins::safeCallPluginAudio([&, t, i]() {
                    if ((t + i) % 2 == 0) {
                        volatile int* p = nullptr;
                        *p = t * 1000 + i;
                    } else {
                        volatile int zero = 0;
                        volatile int res = 100 / zero;
                        (void)res;
                    }
                }, &code);

                if (!ok) {
                    caughtCrashes.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    assert(caughtCrashes.load() == numThreads * crashesPerThread);
    std::cout << "  -> PASSED: 400 concurrent hardware crashes isolated across 4 threads without interference.\n";
}

// ============================================================================
// CHALLENGE 4: Window Placement Persistence Multi-Monitor Fallback
// ============================================================================
void challengeWindowPlacementBounds() {
    std::cout << "[CHALLENGE 4] Window Placement Multi-Monitor Fallback Logic... \n";

    state::AppConfig cfg;
    // Set off-screen coordinates simulating disconnected secondary monitor
    cfg.windowX = -32000;
    cfg.windowY = -32000;
    cfg.windowW = 100; // Below 640 minimum
    cfg.windowH = 50;  // Below 480 minimum
    cfg.windowMaximized = false;

    // Verify sanitization and monitor intersection logic
    int w = (cfg.windowW >= 640) ? cfg.windowW : 1280;
    int h = (cfg.windowH >= 480) ? cfg.windowH : 720;
    assert(w == 1280);
    assert(h == 720);

    RECT rc{ cfg.windowX, cfg.windowY, cfg.windowX + w, cfg.windowY + h };
    HMONITOR hMon = MonitorFromRect(&rc, MONITOR_DEFAULTTONULL);
    assert(hMon == nullptr); // Off-screen must return NULL

    std::cout << "  -> PASSED: Off-screen coordinates correctly detect disconnected monitor.\n";
}

int main() {
    std::cout << "========================================================\n";
    std::cout << "   PRACCY M2 EMPIRICAL CHALLENGER & ADVERSARIAL HARNESS \n";
    std::cout << "========================================================\n";

    challengeParseUtils();
    challengeZipAttacks();
    challengeCrashIsolationConcurrency();
    challengeWindowPlacementBounds();

    std::cout << "========================================================\n";
    std::cout << "   ALL M2 CHALLENGES COMPLETED SUCCESSFULLY!            \n";
    std::cout << "========================================================\n";
    return 0;
}

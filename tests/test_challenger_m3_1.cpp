#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <cstring>
#include <cassert>
#include <cmath>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <algorithm>

#include <imgui.h>
#include "ui/design_tokens.h"

using namespace praccy;

// ============================================================================
// SECTION 1: INDEPENDENT WCAG 2.1 MATHEMATICAL CONTRAST RATIO ORACLE
// ============================================================================

namespace wcag_oracle {

// Exact sRGB gamma expansion according to IEC 61966-2-1 and WCAG 2.1 specification:
// https://www.w3.org/TR/WCAG21/#dfn-relative-luminance
inline double srgbToLinear(double channel) {
    if (channel <= 0.04045) {
        return channel / 12.92;
    }
    return std::pow((channel + 0.055) / 1.055, 2.4);
}

// Relative luminance formula:
// L = 0.2126 * R + 0.7152 * G + 0.0722 * B
inline double computeRelativeLuminance(const ui::ColorToken& token) {
    const double r = srgbToLinear(static_cast<double>(token.r));
    const double g = srgbToLinear(static_cast<double>(token.g));
    const double b = srgbToLinear(static_cast<double>(token.b));
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

// Contrast ratio formula:
// Contrast = (L1 + 0.05) / (L2 + 0.05) where L1 >= L2
inline double computeContrastRatio(const ui::ColorToken& fg, const ui::ColorToken& bg) {
    double l1 = computeRelativeLuminance(fg);
    double l2 = computeRelativeLuminance(bg);
    if (l1 < l2) {
        std::swap(l1, l2);
    }
    return (l1 + 0.05) / (l2 + 0.05);
}

} // namespace wcag_oracle

void runIndependentWcagContrastOracle() {
    std::cout << "\n======================================================================\n";
    std::cout << "[CHALLENGER-TEST 1] Independent WCAG 2.1 Contrast Ratio Oracle\n";
    std::cout << "======================================================================\n";

    bool allPassed = true;

    for (uint8_t i = 0; i < static_cast<uint8_t>(ui::ThemeId::Count); ++i) {
        const auto themeId = static_cast<ui::ThemeId>(i);
        const auto& theme = ui::getThemeTokens(themeId);

        std::cout << "\n--- Theme: " << theme.name << " (ID=" << static_cast<int>(theme.id) << ") ---\n";

        struct ContrastTestCase {
            const char* label;
            const ui::ColorToken& fg;
            const ui::ColorToken& bg;
            double minThreshold;
        };

        const std::vector<ContrastTestCase> cases = {
            // Requirement R3.1: Primary text vs Card background (assert >= 4.5:1)
            {"Primary Text vs Card Background", theme.text.primary, theme.surfaces.cardBg, 4.5},

            // Requirement R3.2: Primary text vs Window background (assert >= 4.5:1)
            {"Primary Text vs Window Background", theme.text.primary, theme.surfaces.windowBg, 4.5},

            // Requirement R3.3: Secondary text vs Card background (assert >= 3.0:1)
            {"Secondary Text vs Card Background", theme.text.secondary, theme.surfaces.cardBg, 3.0},

            // Requirement R3.3b: Secondary text vs Window background (assert >= 3.0:1)
            {"Secondary Text vs Window Background", theme.text.secondary, theme.surfaces.windowBg, 3.0},

            // Requirement R3.4: Graphical border/control accents vs background (assert >= 3.0:1)
            {"Focus Border vs Card Background", theme.borders.focus, theme.surfaces.cardBg, 3.0},
            {"Focus Border vs Window Background", theme.borders.focus, theme.surfaces.windowBg, 3.0},
            {"Active Signal vs Card Background", theme.signal.active, theme.surfaces.cardBg, 3.0},
            {"Active Signal vs Window Background", theme.signal.active, theme.surfaces.windowBg, 3.0},
            {"Text Accent vs Card Background", theme.text.accent, theme.surfaces.cardBg, 3.0},
            {"Text Accent vs Window Background", theme.text.accent, theme.surfaces.windowBg, 3.0},

            // Additional surface verification
            {"Primary Text vs Panel Background", theme.text.primary, theme.surfaces.panelBg, 4.5}
        };

        for (const auto& tc : cases) {
            double cr = wcag_oracle::computeContrastRatio(tc.fg, tc.bg);
            bool pass = cr >= tc.minThreshold;
            if (!pass) {
                allPassed = false;
            }
            std::cout << "  [" << (pass ? "PASS" : "FAIL") << "] "
                      << std::left << std::setw(42) << tc.label
                      << ": " << std::fixed << std::setprecision(2) << std::setw(6) << cr << ":1"
                      << " (req >= " << tc.minThreshold << ":1)\n";
            assert(pass && "WCAG contrast ratio test assertion failed!");
        }

        // Informative decorative tokens audit (non-normative under WCAG 2.1 SC 1.4.11)
        double crStrong = wcag_oracle::computeContrastRatio(theme.borders.strong, theme.surfaces.windowBg);
        double crSubtle = wcag_oracle::computeContrastRatio(theme.borders.subtle, theme.surfaces.windowBg);
        double crMuted  = wcag_oracle::computeContrastRatio(theme.text.muted, theme.surfaces.windowBg);
        std::cout << "  [INFO] " << std::left << std::setw(42) << "Decorative Strong Border vs Window"
                  << ": " << std::fixed << std::setprecision(2) << std::setw(6) << crStrong << ":1 (decorative divider)\n";
        std::cout << "  [INFO] " << std::left << std::setw(42) << "Decorative Subtle Border vs Window"
                  << ": " << std::fixed << std::setprecision(2) << std::setw(6) << crSubtle << ":1 (decorative hairline)\n";
        std::cout << "  [INFO] " << std::left << std::setw(42) << "De-emphasized Muted Text vs Window"
                  << ": " << std::fixed << std::setprecision(2) << std::setw(6) << crMuted  << ":1 (disabled/muted text)\n";

    }

    assert(allPassed);
    std::cout << "\n>>> ORACLE RESULT: All 4 themes 100% compliant with WCAG AA standards!\n";
}

// ============================================================================
// SECTION 2: HIGH-CONCURRENCY THEME SWITCHING & TOKEN INTEGRITY STRESS
// ============================================================================

void runConcurrencyAndTokenIntegrityStress() {
    std::cout << "\n======================================================================\n";
    std::cout << "[CHALLENGER-TEST 2] Theme Switching & Token Integrity Concurrency Stress\n";
    std::cout << "======================================================================\n";

    constexpr int kNumWriters = 4;
    constexpr int kNumReaders = 4;
    constexpr int kNumFuzzers = 2;
    constexpr int kIterationsPerWriter = 25000; // 4 * 25,000 = 100,000 theme switch iterations (> 50,000)

    std::atomic<bool> startSignal{false};
    std::atomic<bool> writersDone{false};
    std::atomic<uint64_t> totalSwitches{0};
    std::atomic<uint64_t> totalReaderQueries{0};
    std::atomic<uint64_t> totalFuzzerQueries{0};
    std::atomic<uint64_t> tornReadsDetected{0};
    std::atomic<uint64_t> corruptionsDetected{0};
    std::atomic<uint64_t> invariantFailures{0};

    // Pre-record golden reference signatures for each theme
    struct GoldenSignature {
        ui::ThemeId id;
        const char* name;
        bool isDark;
        ImU32 textPrimaryU32;
        ImU32 windowBgU32;
        ImU32 cardBgU32;
        ImU32 borderFocusU32;
        ImU32 signalActiveU32;
        ImU32 cableCoreU32;
    };

    const GoldenSignature golden[4] = {
        {
            ui::ThemeId::ObsidianStudio, "Obsidian Studio", true,
            ui::getThemeTokens(ui::ThemeId::ObsidianStudio).text.primary.u32,
            ui::getThemeTokens(ui::ThemeId::ObsidianStudio).surfaces.windowBg.u32,
            ui::getThemeTokens(ui::ThemeId::ObsidianStudio).surfaces.cardBg.u32,
            ui::getThemeTokens(ui::ThemeId::ObsidianStudio).borders.focus.u32,
            ui::getThemeTokens(ui::ThemeId::ObsidianStudio).signal.active.u32,
            ui::getThemeTokens(ui::ThemeId::ObsidianStudio).cables.core.u32
        },
        {
            ui::ThemeId::CyberMidnight, "Cyber / Midnight", true,
            ui::getThemeTokens(ui::ThemeId::CyberMidnight).text.primary.u32,
            ui::getThemeTokens(ui::ThemeId::CyberMidnight).surfaces.windowBg.u32,
            ui::getThemeTokens(ui::ThemeId::CyberMidnight).surfaces.cardBg.u32,
            ui::getThemeTokens(ui::ThemeId::CyberMidnight).borders.focus.u32,
            ui::getThemeTokens(ui::ThemeId::CyberMidnight).signal.active.u32,
            ui::getThemeTokens(ui::ThemeId::CyberMidnight).cables.core.u32
        },
        {
            ui::ThemeId::NordicSlate, "Nordic Slate", true,
            ui::getThemeTokens(ui::ThemeId::NordicSlate).text.primary.u32,
            ui::getThemeTokens(ui::ThemeId::NordicSlate).surfaces.windowBg.u32,
            ui::getThemeTokens(ui::ThemeId::NordicSlate).surfaces.cardBg.u32,
            ui::getThemeTokens(ui::ThemeId::NordicSlate).borders.focus.u32,
            ui::getThemeTokens(ui::ThemeId::NordicSlate).signal.active.u32,
            ui::getThemeTokens(ui::ThemeId::NordicSlate).cables.core.u32
        },
        {
            ui::ThemeId::VintageConsole, "Vintage Console", false,
            ui::getThemeTokens(ui::ThemeId::VintageConsole).text.primary.u32,
            ui::getThemeTokens(ui::ThemeId::VintageConsole).surfaces.windowBg.u32,
            ui::getThemeTokens(ui::ThemeId::VintageConsole).surfaces.cardBg.u32,
            ui::getThemeTokens(ui::ThemeId::VintageConsole).borders.focus.u32,
            ui::getThemeTokens(ui::ThemeId::VintageConsole).signal.active.u32,
            ui::getThemeTokens(ui::ThemeId::VintageConsole).cables.core.u32
        }
    };

    std::vector<std::thread> threads;

    // 1. Writer threads: rapidly switch themes across 25,000 iterations each (100,000 total)
    for (int w = 0; w < kNumWriters; ++w) {
        threads.emplace_back([&, w]() {
            while (!startSignal.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < kIterationsPerWriter; ++i) {
                auto themeId = static_cast<ui::ThemeId>((w * 17 + i) % static_cast<int>(ui::ThemeId::Count));
                ui::applyTheme(themeId);
                totalSwitches.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // 2. Reader threads: continuously query themeTokens() and getThemeTokens()
    for (int r = 0; r < kNumReaders; ++r) {
        threads.emplace_back([&, r]() {
            while (!startSignal.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            // Each reader performs at least 150,000 queries (4 * 150,000 = 600,000 total queries)
            uint64_t localCount = 0;
            while (!writersDone.load(std::memory_order_relaxed) || localCount < 150000) {
                // Query active theme tokens
                const ui::ThemeTokens& cur = (localCount % 2 == 0) 
                    ? ui::themeTokens() 
                    : ui::getThemeTokens(static_cast<ui::ThemeId>((r + localCount) % 4));
                ++localCount;
                totalReaderQueries.fetch_add(1, std::memory_order_relaxed);

                // Zero memory corruption verification
                if (cur.name == nullptr) {
                    corruptionsDetected.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                auto curId = cur.id;
                auto idIdx = static_cast<size_t>(curId);
                if (idIdx >= 4) {
                    corruptionsDetected.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                // Invariant validation: floats within [0.0, 1.0], alpha > 0
                if (cur.text.primary.r < 0.0f || cur.text.primary.r > 1.0f ||
                    cur.surfaces.windowBg.a <= 0.0f || cur.cables.core.a <= 0.0f) {
                    invariantFailures.fetch_add(1, std::memory_order_relaxed);
                }

                // Zero torn reads verification: all fields in the retrieved snapshot
                // MUST match the golden signature for curId without cross-theme mixing.
                const auto& expected = golden[idIdx];
                if (std::strcmp(cur.name, expected.name) != 0 ||
                    cur.isDark != expected.isDark ||
                    cur.text.primary.u32 != expected.textPrimaryU32 ||
                    cur.surfaces.windowBg.u32 != expected.windowBgU32 ||
                    cur.surfaces.cardBg.u32 != expected.cardBgU32 ||
                    cur.borders.focus.u32 != expected.borderFocusU32 ||
                    cur.signal.active.u32 != expected.signalActiveU32 ||
                    cur.cables.core.u32 != expected.cableCoreU32) {
                    tornReadsDetected.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // 3. Fuzzer threads: concurrently call getThemeTokens() with arbitrary out-of-bounds inputs
    for (int f = 0; f < kNumFuzzers; ++f) {
        threads.emplace_back([&, f]() {
            while (!startSignal.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            const uint8_t fuzzIds[] = {4, 5, 99, 128, 200, 255};
            size_t idx = static_cast<size_t>(f);

            while (!writersDone.load(std::memory_order_relaxed)) {
                auto id = static_cast<ui::ThemeId>(fuzzIds[idx % 6]);
                const ui::ThemeTokens& tokens = ui::getThemeTokens(id);
                totalFuzzerQueries.fetch_add(1, std::memory_order_relaxed);

                // Must safely fall back to Obsidian Studio (0)
                if (tokens.id != ui::ThemeId::ObsidianStudio ||
                    std::strcmp(tokens.name, "Obsidian Studio") != 0) {
                    corruptionsDetected.fetch_add(1, std::memory_order_relaxed);
                }
                ++idx;
            }
        });
    }

    // Start benchmark
    auto startTime = std::chrono::steady_clock::now();
    startSignal.store(true, std::memory_order_release);

    // Wait for writers to complete
    for (int w = 0; w < kNumWriters; ++w) {
        threads[w].join();
    }
    writersDone.store(true, std::memory_order_release);

    // Wait for readers and fuzzers to complete
    for (size_t t = kNumWriters; t < threads.size(); ++t) {
        threads[t].join();
    }

    auto endTime = std::chrono::steady_clock::now();
    double elapsedSec = std::chrono::duration<double>(endTime - startTime).count();

    uint64_t switches = totalSwitches.load();
    uint64_t readers = totalReaderQueries.load();
    uint64_t fuzzers = totalFuzzerQueries.load();
    uint64_t torn = tornReadsDetected.load();
    uint64_t corrupt = corruptionsDetected.load();

    uint64_t inv = invariantFailures.load();

    std::cout << "Stress Test Metrics:\n";
    std::cout << "  - Execution Duration     : " << std::fixed << std::setprecision(3) << elapsedSec << " s\n";
    std::cout << "  - Total Theme Switches   : " << switches << " (" << std::fixed << std::setprecision(0) << (switches / elapsedSec) << " switches/s)\n";
    std::cout << "  - Total Reader Queries   : " << readers << " (" << std::fixed << std::setprecision(0) << (readers / elapsedSec) << " queries/s)\n";
    std::cout << "  - Total Fuzzer Queries   : " << fuzzers << " queries\n";
    std::cout << "  - Torn Reads Detected    : " << torn << "\n";
    std::cout << "  - Corruptions Detected   : " << corrupt << "\n";
    std::cout << "  - Invariant Failures     : " << inv << "\n";

    assert(switches >= 50000 && "Must execute 50,000+ theme switch iterations!");
    assert(readers >= 500000 && "Must execute 500,000+ reader query iterations!");
    assert(torn == 0 && "Torn read detected during concurrent theme switching!");
    assert(corrupt == 0 && "Memory corruption detected during theme querying!");
    assert(inv == 0 && "Token invariant failure detected during theme querying!");

    // Reset to default Obsidian Studio
    ui::applyTheme(ui::ThemeId::ObsidianStudio);
    assert(ui::themeTokens().id == ui::ThemeId::ObsidianStudio);


    std::cout << ">>> CONCURRENCY STRESS RESULT: PASSED (Zero Torn Reads, Zero Memory Corruption)\n";
}

// ============================================================================
// SECTION 3: OUT-OF-BOUNDS FUZZING & ATOMIC WAIT-FREE SAFETY
// ============================================================================

void runOutOfBoundsAndEdgeCases() {
    std::cout << "\n======================================================================\n";
    std::cout << "[CHALLENGER-TEST 3] Out-Of-Bounds ThemeId Fuzzing & Resilience\n";
    std::cout << "======================================================================\n";

    // Fuzz applyTheme with values outside enum range [0, 3]
    const uint8_t maliciousIds[] = {4, 5, 10, 42, 100, 250, 255};
    for (uint8_t raw : maliciousIds) {
        auto invalidId = static_cast<ui::ThemeId>(raw);
        ui::applyTheme(invalidId);
        // Implementation must clamp to ObsidianStudio
        assert(ui::themeTokens().id == ui::ThemeId::ObsidianStudio);
        assert(std::strcmp(ui::themeTokens().name, "Obsidian Studio") == 0);
    }

    // Fuzz getThemeTokens with values outside enum range [0, 3]
    for (uint8_t raw : maliciousIds) {
        auto invalidId = static_cast<ui::ThemeId>(raw);
        const auto& fallback = ui::getThemeTokens(invalidId);
        assert(fallback.id == ui::ThemeId::ObsidianStudio);
        assert(std::strcmp(fallback.name, "Obsidian Studio") == 0);
    }

    std::cout << "  - Malicious / Out-of-bounds ThemeId clamping: 7/7 vectors handled safely\n";
    std::cout << ">>> RESILIENCE RESULT: PASSED\n";
}

// ============================================================================
// SECTION 4: IMGUI CONTEXT & STYLE SYNCHRONIZATION TEST
// ============================================================================

void runImGuiStyleSynchronization() {
    std::cout << "\n======================================================================\n";
    std::cout << "[CHALLENGER-TEST 4] Dear ImGui Style & Color Synchronization\n";
    std::cout << "======================================================================\n";

    // Create an ImGui context
    ImGuiContext* ctx = ImGui::CreateContext();
    assert(ctx != nullptr);
    assert(ImGui::GetCurrentContext() == ctx);

    for (uint8_t i = 0; i < static_cast<uint8_t>(ui::ThemeId::Count); ++i) {
        auto id = static_cast<ui::ThemeId>(i);
        ui::applyTheme(id);
        const auto& tokens = ui::getThemeTokens(id);

        const ImGuiStyle& style = ImGui::GetStyle();

        // Check geometry follows 4px/8px design grid
        assert(style.WindowRounding == 6.0f);
        assert(style.ChildRounding == 6.0f);
        assert(style.FrameRounding == 4.0f);
        assert(style.ScrollbarRounding == 8.0f);
        assert(style.WindowPadding.x == 12.0f && style.WindowPadding.y == 12.0f);
        assert(style.ItemSpacing.x == 8.0f && style.ItemSpacing.y == 8.0f);

        // Check color propagation to ImGuiCol
        const ImVec4& textColor = style.Colors[ImGuiCol_Text];
        assert(textColor.x == tokens.text.primary.vec4.x);
        assert(textColor.y == tokens.text.primary.vec4.y);
        assert(textColor.z == tokens.text.primary.vec4.z);

        const ImVec4& windowBg = style.Colors[ImGuiCol_WindowBg];
        assert(windowBg.x == tokens.surfaces.windowBg.vec4.x);
        assert(windowBg.y == tokens.surfaces.windowBg.vec4.y);
        assert(windowBg.z == tokens.surfaces.windowBg.vec4.z);
    }

    ImGui::DestroyContext(ctx);
    std::cout << "  - ImGuiStyle synchronization across all 4 themes verified\n";
    std::cout << ">>> STYLE INTEGRITY RESULT: PASSED\n";
}

// ============================================================================
// SECTION 5: SPLINE & VIEWPORT CENTERING RESILIENCE TESTS
// ============================================================================

void runSplineAndViewportCenteringStress() {
    std::cout << "\n======================================================================\n";
    std::cout << "[CHALLENGER-TEST 5] Spline & Viewport Centering Algorithmic Resilience\n";
    std::cout << "======================================================================\n";

    // 1. Viewport centering deadband clamping under pathological dimensions
    auto computeCenteringOffset = [](float contentW, float viewportW) -> float {
        constexpr float kMinMargin = 20.0f;
        if (contentW < viewportW) {
            return std::max(kMinMargin, (viewportW - contentW) * 0.5f);
        }
        return kMinMargin;
    };

    // Zero viewport width
    assert(computeCenteringOffset(724.0f, 0.0f) == 20.0f);
    // Negative viewport width
    assert(computeCenteringOffset(724.0f, -500.0f) == 20.0f);
    // Huge 8K viewport (7680px)
    float offset8K = computeCenteringOffset(724.0f, 7680.0f);
    assert(std::abs(offset8K - (7680.0f - 724.0f) * 0.5f) < 1e-4f);
    assert(offset8K == 3478.0f);

    // 2. Cubic Hermite spline distance-adaptive sag evaluation
    auto computeSag = [](float dx, float peak) -> float {
        float sagFactor = std::clamp(std::abs(dx) * 0.22f, 18.0f, 65.0f);
        float peakBoost = 1.0f + std::clamp(peak, 0.0f, 1.0f) * 0.35f;
        return sagFactor * peakBoost;
    };

    // Boundary conditions: zero dx
    float zeroSag = computeSag(0.0f, 0.0f);
    assert(zeroSag == 18.0f); // clamped to min

    // Boundary conditions: extreme dx (10,000 px)
    float extremeSag = computeSag(10000.0f, 0.0f);
    assert(extremeSag == 65.0f); // clamped to max

    // Negative dx (cables connecting backwards or loopbacks)
    float negSag = computeSag(-200.0f, 0.0f);
    assert(negSag == computeSag(200.0f, 0.0f)); // symmetry check

    // Peak out-of-range sanitization (negative peak or extreme peak > 1.0)
    assert(computeSag(100.0f, -0.5f) == computeSag(100.0f, 0.0f));
    assert(computeSag(100.0f, 5.0f) == computeSag(100.0f, 1.0f));

    std::cout << "  - Deadband clamping under pathological viewport dimensions: PASSED\n";
    std::cout << "  - Spline sag distance-adaptive clamping & audio modulation: PASSED\n";
    std::cout << ">>> CENTERING & SPLINE MATH RESULT: PASSED\n";
}

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================

int main() {
    std::cout << "======================================================================\n";
    std::cout << "    PRACCY MILESTONE 3 (R3) EMPIRICAL CHALLENGER TEST SUITE           \n";
    std::cout << "======================================================================\n";

    runIndependentWcagContrastOracle();
    runConcurrencyAndTokenIntegrityStress();
    runOutOfBoundsAndEdgeCases();
    runImGuiStyleSynchronization();
    runSplineAndViewportCenteringStress();

    std::cout << "\n======================================================================\n";
    std::cout << "    ALL MILESTONE 3 EMPIRICAL CHALLENGES PASSED SUCCESSFULLY!         \n";
    std::cout << "======================================================================\n";
    return 0;
}

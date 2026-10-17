#pragma once

#include <imgui.h>
#include <cmath>
#include <string>
#include <algorithm>
#include <cstdio>
#include <functional>
#include "../../tools/tuner.h"
#include "../../tools/tuner_settings.h"
#include "../design_tokens.h"

extern ImFont* g_fontUI;
extern ImFont* g_fontMono;

namespace praccy::ui {

/**
 * @brief Crisp IEC 5009 power button glyph (circular arc with vertical line in top opening).
 */
inline void DrawPowerGlyph(ImDrawList* dl, ImVec2 center, float radius, ImU32 color, float thickness = 1.6f) {
    constexpr float kDegToRad = IM_PI / 180.0f;
    // Circular arc with 70-degree top gap: from -55 deg to 235 deg
    dl->PathArcTo(center, radius, -55.0f * kDegToRad, 235.0f * kDegToRad, 20);
    dl->PathStroke(color, 0, thickness);
    // Vertical stem down from top gap into circle center
    dl->AddLine(ImVec2(center.x, center.y - radius - 1.5f),
                ImVec2(center.x, center.y + 0.5f),
                color, thickness);
}

/**
 * @brief Crisp 6-tooth mechanical gear / cog glyph for settings button.
 */
inline void DrawGearGlyph(ImDrawList* dl, ImVec2 center, float radius, ImU32 color) {
    // 6 solid gear teeth radiating outward
    constexpr int kTeeth = 6;
    const float rInner = radius * 0.48f;
    const float rOuter = radius + 0.6f;
    const float toothW = radius * 0.44f;
    for (int i = 0; i < kTeeth; ++i) {
        float a = static_cast<float>(i) * (IM_PI / 3.0f);
        float cosA = std::cos(a);
        float sinA = std::sin(a);
        dl->AddLine(ImVec2(center.x + rInner * cosA, center.y + rInner * sinA),
                    ImVec2(center.x + rOuter * cosA, center.y + rOuter * sinA),
                    color, toothW);
    }
    // Solid annular gear body fusing teeth together
    const float ringR = radius * 0.52f;
    const float ringThick = radius * 0.38f;
    dl->AddCircle(center, ringR, color, 16, ringThick);
    // Crisp central axle hole
    dl->AddCircle(center, radius * 0.28f, color, 12, 1.1f);
}

/**
 * @brief High-resolution circular strobe tuner widget.
 * Features rotating strobe teeth (freezing solid within +/-1 cent),
 * calibrated +/-50 cent needle deviation arc gauge, and central note lock display.
 */
inline void DrawStrobeTuner(const tools::TunerResult& result,
                            ImVec2 center,
                            float radius,
                            ImDrawList* dl = nullptr,
                            bool isEnabled = true) {
    if (!dl) {
        dl = ImGui::GetWindowDrawList();
    }
    const auto& tokens = themeTokens();
    const ImGuiIO& io = ImGui::GetIO();
    const float dt = std::clamp(io.DeltaTime, 0.0f, 0.1f);

    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID phaseKey = ImGui::GetID("##praccy_strobe_phase");
    float s_strobePhase = storage ? storage->GetFloat(phaseKey, 0.0f) : 0.0f;
    const bool inTuneExact = isEnabled && result.confidence && (std::abs(result.centDeviation) <= 1.0f);
    const bool inTuneBroad = isEnabled && result.confidence && (std::abs(result.centDeviation) <= 3.0f);

    if (isEnabled && result.confidence) {
        if (inTuneExact) {
            // Frozen solid when pitch is within +/-1 cent
        } else {
            // Glides counter-clockwise when flat (<0), clockwise when sharp (>0)
            // Speed scaled by cent deviation
            s_strobePhase += result.centDeviation * 0.10f * dt * (2.0f * IM_PI);
            while (s_strobePhase > 2.0f * IM_PI) s_strobePhase -= 2.0f * IM_PI;
            while (s_strobePhase < 0.0f) s_strobePhase += 2.0f * IM_PI;
        }
    } else if (isEnabled) {
        // Idle gentle rotation
        s_strobePhase += 0.04f * dt * (2.0f * IM_PI);
        while (s_strobePhase > 2.0f * IM_PI) s_strobePhase -= 2.0f * IM_PI;
    }
    if (storage) {
        storage->SetFloat(phaseKey, s_strobePhase);
    }

    // 1. Outer Ring Housing & Background Track
    dl->AddCircleFilled(center, radius, tokens.surfaces.frameBg.u32, 48);
    dl->AddCircle(center, radius, tokens.borders.subtle.u32, 48, 1.0f);

    // 2. Rotating Strobe Band Ring
    const float innerRadius = radius * 0.62f;
    const float outerStrobeR = radius - 3.0f;
    constexpr int kNumTeeth = 24;

    ImU32 toothCol = tokens.borders.separator.u32;
    if (isEnabled && result.confidence) {
        if (inTuneExact || inTuneBroad) {
            toothCol = tokens.signal.tunerInTune.u32;
        } else if (result.centDeviation < 0.0f) {
            toothCol = tokens.signal.tunerFlat.u32;
        } else {
            toothCol = tokens.signal.tunerSharp.u32;
        }
    }

    for (int i = 0; i < kNumTeeth; ++i) {
        const float angle = s_strobePhase + static_cast<float>(i) * (2.0f * IM_PI / static_cast<float>(kNumTeeth));
        const float cosA = std::cos(angle);
        const float sinA = std::sin(angle);

        const ImVec2 p1(center.x + innerRadius * cosA, center.y + innerRadius * sinA);
        const ImVec2 p2(center.x + outerStrobeR * cosA, center.y + outerStrobeR * sinA);
        dl->AddLine(p1, p2, toothCol, 2.0f);
    }

    // 3. Calibrated +/-50 Cent Needle Deviation Arc Gauge (Top 120-degree sweep)
    constexpr float kDegToRad = IM_PI / 180.0f;
    const float gaugeArcR = radius + 2.0f;

    // Draw arc track
    dl->PathArcTo(center, gaugeArcR, 210.0f * kDegToRad, 330.0f * kDegToRad, 24);
    dl->PathStroke(tokens.borders.subtle.u32, 0, 1.0f);

    // Reference calibration ticks: -50, -25, 0, +25, +50 cents
    const float tickValues[] = { -50.0f, -25.0f, 0.0f, 25.0f, 50.0f };
    for (float tickCents : tickValues) {
        const float tickAngle = (270.0f + (tickCents / 50.0f) * 60.0f) * kDegToRad;
        const float tCos = std::cos(tickAngle);
        const float tSin = std::sin(tickAngle);

        const float isCenter = (std::abs(tickCents) < 0.1f);
        const float rA = gaugeArcR;
        const float rB = gaugeArcR + (isCenter ? 5.0f : 3.0f);

        const ImU32 tickCol = (isCenter && isEnabled) ? tokens.signal.tunerInTune.u32 : tokens.borders.subtle.u32;
        const float tickThick = isCenter ? 2.0f : 1.0f;
        dl->AddLine(ImVec2(center.x + rA * tCos, center.y + rA * tSin),
                    ImVec2(center.x + rB * tCos, center.y + rB * tSin),
                    tickCol, tickThick);
    }

    // Calibrated needle deviation pointer
    if (isEnabled && result.confidence) {
        const float clampedDeviation = std::clamp(result.centDeviation, -50.0f, 50.0f);
        const float needleAngle = (270.0f + (clampedDeviation / 50.0f) * 60.0f) * kDegToRad;
        const float nCos = std::cos(needleAngle);
        const float nSin = std::sin(needleAngle);

        const ImU32 needleColor = (inTuneExact || inTuneBroad) ? tokens.signal.tunerInTune.u32
                                : (result.centDeviation < 0.0f ? tokens.signal.tunerFlat.u32
                                                              : tokens.signal.tunerSharp.u32);

        const ImVec2 needleBase(center.x + (innerRadius - 2.0f) * nCos, center.y + (innerRadius - 2.0f) * nSin);
        const ImVec2 needleTip(center.x + (gaugeArcR + 5.0f) * nCos, center.y + (gaugeArcR + 5.0f) * nSin);
        dl->AddLine(needleBase, needleTip, needleColor, 2.0f);

        // Small illuminated needle tip head
        dl->AddCircleFilled(needleTip, 2.5f, needleColor, 12);
    }

    // 4. Central Hub & Large Note Display
    const float hubR = innerRadius - 2.0f;
    const ImU32 hubBg = tokens.surfaces.cardBg.u32;
    const ImU32 hubBorder = (isEnabled && (inTuneExact || inTuneBroad)) ? tokens.signal.tunerInTune.u32 : tokens.borders.subtle.u32;
    const float hubBorderThick = (isEnabled && (inTuneExact || inTuneBroad)) ? 2.0f : 1.0f;

    dl->AddCircleFilled(center, hubR, hubBg, 32);
    dl->AddCircle(center, hubR, hubBorder, 32, hubBorderThick);

    const char* noteStr = (!isEnabled) ? "--" : (result.confidence && !result.noteName.empty() ? result.noteName.c_str() : "--");
    const ImVec2 noteSize = ImGui::CalcTextSize(noteStr);
    const ImU32 noteColor = (!isEnabled) ? tokens.text.muted.u32
                          : ((inTuneExact || inTuneBroad) ? tokens.signal.tunerInTune.u32 : tokens.text.primary.u32);

    dl->AddText(ImVec2(center.x - noteSize.x * 0.5f, center.y - noteSize.y * 0.5f), noteColor, noteStr);
}

/**
 * @brief High-level Strobe Tuner module layout for ribbon & docked views.
 * Combines circular strobe dial, settings button, on/off power toggle button,
 * tabular monospace frequency/cents readouts, and deviation track bar.
 */
inline void DrawStrobeTunerModule(const tools::TunerResult& result,
                                  float availW,
                                  float availH,
                                  tools::InstrumentTuner* tuner = nullptr,
                                  std::function<void()> onOpenSettings = nullptr) {
    const auto& tokens = themeTokens();
    const ImVec2 startPos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const bool isTunerEnabled = tuner ? tuner->isEnabled() : true;

    const float dialRadius = std::clamp((availH - 22.0f) * 0.5f, 22.0f, 36.0f);
    const float dialMarginX = dialRadius + 8.0f;
    const ImVec2 dialCenter(startPos.x + dialMarginX, startPos.y + availH * 0.5f);

    // 1. Draw Circular Strobe Dial
    DrawStrobeTuner(result, dialCenter, dialRadius, dl, isTunerEnabled);

    // 2. Right-hand details column
    const float textStartX = startPos.x + dialMarginX + dialRadius + 14.0f;
    const float textAvailW = std::max(60.0f, availW - (textStartX - startPos.x));

    // Header Row: Title, Settings Button, and On/Off Toggle Button
    float curY = startPos.y + 6.0f;

    // Header buttons layout (standard button size):
    // [ Settings Button ⚙ ]  [ On/Off Power Button ⏻ ]
    const float btnH = Metrics::buttonHeight();
    const float btnW = btnH;
    const float btnGap = 4.0f;
    const float powerBtnX = textStartX + textAvailW - btnW - 2.0f;
    const float settingsBtnX = powerBtnX - btnW - btnGap;

    const float titleTextY = curY + (btnH - ImGui::GetFontSize()) * 0.5f;
    dl->AddText(ImVec2(textStartX, titleTextY), tokens.text.muted.u32, "TUNER");

    // Optional compact tuning subtitle badge if room allows
    const float titleW = ImGui::CalcTextSize("TUNER").x;
    const float subStartX = textStartX + titleW + 6.0f;
    const float subAvailW = settingsBtnX - subStartX - 4.0f;

    if (subAvailW > 45.0f && tuner) {
        const auto& cfg = tuner->config();
        char badgeBuf[48];
        const int a4Round = static_cast<int>(std::round(tuner->a4Reference()));
        if (!isTunerEnabled) {
            std::snprintf(badgeBuf, sizeof(badgeBuf), "OFF");
        } else if (a4Round != 440) {
            std::snprintf(badgeBuf, sizeof(badgeBuf), "%s • %dHz", cfg.tuningTypeName(), a4Round);
        } else {
            std::snprintf(badgeBuf, sizeof(badgeBuf), "%s", cfg.tuningTypeName());
        }
        ImVec2 badgeSize = ImGui::CalcTextSize(badgeBuf);
        if (badgeSize.x <= subAvailW) {
            const float badgeY = curY + (btnH - (badgeSize.y + 2.0f)) * 0.5f;
            dl->AddRectFilled(ImVec2(subStartX, badgeY),
                              ImVec2(subStartX + badgeSize.x + 6.0f, badgeY + badgeSize.y + 2.0f),
                              tokens.surfaces.cardBg.u32, 2.0f);
            dl->AddText(ImVec2(subStartX + 3.0f, badgeY + 1.0f),
                        isTunerEnabled ? tokens.text.secondary.u32 : tokens.text.muted.u32,
                        badgeBuf);
        }
    }

    // 1. Tuner Settings Button (immediately to the left of the power button)
    if (settingsBtnX > textStartX + titleW + 10.0f) {
        ImGui::SetCursorScreenPos(ImVec2(settingsBtnX, curY));
        if (ImGui::InvisibleButton("##TunerSettingsBtn", ImVec2(btnW, btnH))) {
            if (onOpenSettings) onOpenSettings();
        }
        const bool settingsHovered = ImGui::IsItemHovered();
        const bool settingsActive = ImGui::IsItemActive();
        if (settingsHovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("Tuner Settings\nConfigure instrument type, strings, tuning & pitch standard");
        }
        const ImU32 settingsBg = settingsActive ? tokens.surfaces.buttonBgActive.u32
                               : (settingsHovered ? tokens.surfaces.buttonBgHovered.u32 : tokens.surfaces.frameBg.u32);
        const ImU32 settingsBorder = settingsHovered ? tokens.borders.focus.u32 : tokens.borders.subtle.u32;
        const ImU32 settingsIconCol = settingsHovered ? tokens.text.primary.u32 : tokens.text.secondary.u32;

        dl->AddRectFilled(ImVec2(settingsBtnX, curY),
                          ImVec2(settingsBtnX + btnW, curY + btnH),
                          settingsBg, 4.0f);
        dl->AddRect(ImVec2(settingsBtnX, curY),
                    ImVec2(settingsBtnX + btnW, curY + btnH),
                    settingsBorder, 4.0f, 0, 1.0f);
        DrawGearGlyph(dl, ImVec2(settingsBtnX + btnW * 0.5f, curY + btnH * 0.5f), 5.8f, settingsIconCol);
    }

    // 2. On/Off Toggle Button with Power Glyph (immediately to the right of settings button)
    if (powerBtnX > textStartX + titleW + 20.0f) {
        ImGui::SetCursorScreenPos(ImVec2(powerBtnX, curY));
        if (ImGui::InvisibleButton("##TunerPowerToggleBtn", ImVec2(btnW, btnH))) {
            if (tuner) {
                tuner->setEnabled(!tuner->isEnabled());
            }
        }
        const bool powerHovered = ImGui::IsItemHovered();
        const bool powerActive = ImGui::IsItemActive();
        if (powerHovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (isTunerEnabled) {
                ImGui::SetTooltip("Tuner: Active (Click to Mute / Disable)");
            } else {
                ImGui::SetTooltip("Tuner: Disabled (Click to Enable)");
            }
        }

        const ImU32 powerBg = powerActive ? tokens.surfaces.buttonBgActive.u32
                            : (powerHovered ? tokens.surfaces.buttonBgHovered.u32 : tokens.surfaces.frameBg.u32);
        const ImU32 powerBorder = isTunerEnabled ? tokens.signal.tunerInTune.u32 : tokens.borders.subtle.u32;
        const ImU32 powerIconCol = isTunerEnabled ? tokens.signal.tunerInTune.u32
                                : (powerHovered ? tokens.text.secondary.u32 : tokens.text.muted.u32);

        dl->AddRectFilled(ImVec2(powerBtnX, curY),
                          ImVec2(powerBtnX + btnW, curY + btnH),
                          powerBg, 4.0f);
        dl->AddRect(ImVec2(powerBtnX, curY),
                    ImVec2(powerBtnX + btnW, curY + btnH),
                    powerBorder, 4.0f, 0, 1.0f);
        DrawPowerGlyph(dl, ImVec2(powerBtnX + btnW * 0.5f, curY + btnH * 0.5f), 6.5f, powerIconCol, 2.0f);
    }

    curY += btnH + (availH >= 85.0f ? 8.0f : 5.0f);

    // Tabular Monospace Readout: Frequency & Cents
    char freqBuf[32];
    char centsBuf[32];
    const bool inTune = isTunerEnabled && result.confidence && (std::abs(result.centDeviation) <= 3.0f);

    if (isTunerEnabled && result.confidence) {
        std::snprintf(freqBuf, sizeof(freqBuf), "%.1f Hz", result.frequencyHz);
        std::snprintf(centsBuf, sizeof(centsBuf), "%+0.1f ct", result.centDeviation);
    } else if (!isTunerEnabled) {
        std::snprintf(freqBuf, sizeof(freqBuf), "OFF");
        std::snprintf(centsBuf, sizeof(centsBuf), "--.- ct");
    } else {
        std::snprintf(freqBuf, sizeof(freqBuf), "---.- Hz");
        std::snprintf(centsBuf, sizeof(centsBuf), "--.- ct");
    }

    if (g_fontMono) ImGui::PushFont(g_fontMono);
    dl->AddText(ImVec2(textStartX, curY), isTunerEnabled ? tokens.text.primary.u32 : tokens.text.muted.u32, freqBuf);

    const ImVec2 centsSize = ImGui::CalcTextSize(centsBuf);
    const float centsX = textStartX + textAvailW - centsSize.x - 4.0f;
    const ImU32 centsColor = (!isTunerEnabled) ? tokens.text.muted.u32
                           : (inTune ? tokens.signal.tunerInTune.u32
                           : (result.confidence ? (result.centDeviation < 0.0f ? tokens.signal.tunerFlat.u32 : tokens.signal.tunerSharp.u32)
                                                : tokens.text.muted.u32));
    if (centsX > textStartX + 50.0f) {
        dl->AddText(ImVec2(centsX, curY), centsColor, centsBuf);
    }
    if (g_fontMono) ImGui::PopFont();

    curY += ImGui::GetFontSize() + (availH >= 85.0f ? 8.0f : 5.0f);

    // Mini Horizontal Deviation Track Bar
    const float barW = std::max(40.0f, textAvailW - 4.0f);
    const float barH = (availH >= 85.0f ? 9.0f : 7.0f);
    const ImVec2 barMin(textStartX, curY);
    const ImVec2 barMax(textStartX + barW, curY + barH);

    dl->AddRectFilled(barMin, barMax, tokens.surfaces.frameBg.u32, 2.0f);
    dl->AddRect(barMin, barMax, tokens.borders.subtle.u32, 2.0f, 0, 1.0f);

    const float midX = textStartX + barW * 0.5f;
    // Center in-tune tick
    dl->AddLine(ImVec2(midX, barMin.y - 1.0f), ImVec2(midX, barMax.y + 1.0f),
                isTunerEnabled ? tokens.signal.tunerInTune.u32 : tokens.borders.subtle.u32, 1.5f);

    if (isTunerEnabled && result.confidence) {
        const float norm = std::clamp((result.centDeviation + 50.0f) / 100.0f, 0.0f, 1.0f);
        const float markerX = textStartX + norm * barW;
        dl->AddRectFilled(ImVec2(markerX - 1.5f, barMin.y), ImVec2(markerX + 1.5f, barMax.y), centsColor, 1.0f);
    }

    ImGui::Dummy(ImVec2(availW, availH));
}

} // namespace praccy::ui

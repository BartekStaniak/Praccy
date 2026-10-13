#pragma once

#include <imgui.h>
#include <cmath>
#include <string>
#include <algorithm>
#include <cstdio>
#include "../../tools/tuner.h"
#include "../design_tokens.h"

extern ImFont* g_fontUI;
extern ImFont* g_fontMono;

namespace praccy::ui {

/**
 * @brief High-resolution circular strobe tuner widget.
 * Features rotating strobe teeth (freezing solid within +/-1 cent),
 * calibrated +/-50 cent needle deviation arc gauge, and central note lock display.
 */
inline void DrawStrobeTuner(const tools::TunerResult& result,
                            ImVec2 center,
                            float radius,
                            ImDrawList* dl = nullptr) {
    if (!dl) {
        dl = ImGui::GetWindowDrawList();
    }
    const auto& tokens = themeTokens();
    const ImGuiIO& io = ImGui::GetIO();
    const float dt = std::clamp(io.DeltaTime, 0.0f, 0.1f);

    static float s_strobePhase = 0.0f;
    const bool inTuneExact = result.confidence && (std::abs(result.centDeviation) <= 1.0f);
    const bool inTuneBroad = result.confidence && (std::abs(result.centDeviation) <= 3.0f);

    if (result.confidence) {
        if (inTuneExact) {
            // Frozen solid when pitch is within +/-1 cent
        } else {
            // Glides counter-clockwise when flat (<0), clockwise when sharp (>0)
            // Speed scaled by cent deviation
            s_strobePhase += result.centDeviation * 0.10f * dt * (2.0f * IM_PI);
            while (s_strobePhase > 2.0f * IM_PI) s_strobePhase -= 2.0f * IM_PI;
            while (s_strobePhase < 0.0f) s_strobePhase += 2.0f * IM_PI;
        }
    } else {
        // Idle gentle rotation
        s_strobePhase += 0.04f * dt * (2.0f * IM_PI);
        while (s_strobePhase > 2.0f * IM_PI) s_strobePhase -= 2.0f * IM_PI;
    }

    // 1. Outer Ring Housing & Background Track
    dl->AddCircleFilled(center, radius, tokens.surfaces.frameBg.u32, 48);
    dl->AddCircle(center, radius, tokens.borders.subtle.u32, 48, 1.0f);

    // 2. Rotating Strobe Band Ring
    const float innerRadius = radius * 0.62f;
    const float outerStrobeR = radius - 3.0f;
    constexpr int kNumTeeth = 24;

    ImU32 toothCol = tokens.borders.separator.u32;
    if (result.confidence) {
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
    // -50 cents at 210 deg, 0 cents at 270 deg (12 o'clock), +50 cents at 330 deg
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

        const ImU32 tickCol = isCenter ? tokens.signal.tunerInTune.u32 : tokens.borders.subtle.u32;
        const float tickThick = isCenter ? 2.0f : 1.0f;
        dl->AddLine(ImVec2(center.x + rA * tCos, center.y + rA * tSin),
                    ImVec2(center.x + rB * tCos, center.y + rB * tSin),
                    tickCol, tickThick);
    }

    // Calibrated needle deviation pointer
    if (result.confidence) {
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
    const ImU32 hubBorder = (inTuneExact || inTuneBroad) ? tokens.signal.tunerInTune.u32 : tokens.borders.subtle.u32;
    const float hubBorderThick = (inTuneExact || inTuneBroad) ? 2.0f : 1.0f;

    dl->AddCircleFilled(center, hubR, hubBg, 32);
    dl->AddCircle(center, hubR, hubBorder, 32, hubBorderThick);

    const char* noteStr = result.confidence && !result.noteName.empty() ? result.noteName.c_str() : "--";
    const ImVec2 noteSize = ImGui::CalcTextSize(noteStr);
    const ImU32 noteColor = (inTuneExact || inTuneBroad) ? tokens.signal.tunerInTune.u32 : tokens.text.primary.u32;

    dl->AddText(ImVec2(center.x - noteSize.x * 0.5f, center.y - noteSize.y * 0.5f), noteColor, noteStr);
}

/**
 * @brief High-level Strobe Tuner module layout for ribbon & docked views.
 * Combines circular strobe dial, tabular monospace frequency/cents readouts, and status lock pill.
 */
inline void DrawStrobeTunerModule(const tools::TunerResult& result, float availW, float availH) {
    const auto& tokens = themeTokens();
    const ImVec2 startPos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float dialRadius = std::clamp((availH - 18.0f) * 0.5f, 22.0f, 32.0f);
    const float dialMarginX = dialRadius + 8.0f;
    const ImVec2 dialCenter(startPos.x + dialMarginX, startPos.y + availH * 0.5f);

    // 1. Draw Circular Strobe Dial
    DrawStrobeTuner(result, dialCenter, dialRadius, dl);

    // 2. Right-hand details column
    const float textStartX = startPos.x + dialMarginX + dialRadius + 14.0f;
    const float textAvailW = std::max(60.0f, availW - (textStartX - startPos.x));

    // Header Row: Title & Lock Status
    float curY = startPos.y + 4.0f;
    const bool inTune = result.confidence && (std::abs(result.centDeviation) <= 3.0f);
    const char* statusPill = "LISTENING";
    ImU32 statusColor = tokens.text.muted.u32;

    if (result.confidence) {
        if (std::abs(result.centDeviation) <= 1.0f) {
            statusPill = "LOCKED";
            statusColor = tokens.signal.tunerInTune.u32;
        } else if (inTune) {
            statusPill = "IN TUNE";
            statusColor = tokens.signal.tunerInTune.u32;
        } else if (result.centDeviation < 0.0f) {
            statusPill = "FLAT";
            statusColor = tokens.signal.tunerFlat.u32;
        } else {
            statusPill = "SHARP";
            statusColor = tokens.signal.tunerSharp.u32;
        }
    }

    dl->AddText(ImVec2(textStartX, curY), tokens.text.muted.u32, "TUNER");

    const ImVec2 statusSize = ImGui::CalcTextSize(statusPill);
    const float statusBadgeW = statusSize.x + 8.0f;
    const float statusBadgeH = statusSize.y + 2.0f;
    const float statusBadgeX = textStartX + textAvailW - statusBadgeW - 4.0f;

    if (statusBadgeX > textStartX + 44.0f) {
        dl->AddRectFilled(ImVec2(statusBadgeX, curY - 1.0f),
                          ImVec2(statusBadgeX + statusBadgeW, curY - 1.0f + statusBadgeH),
                          tokens.surfaces.frameBg.u32, 3.0f);
        dl->AddRect(ImVec2(statusBadgeX, curY - 1.0f),
                    ImVec2(statusBadgeX + statusBadgeW, curY - 1.0f + statusBadgeH),
                    statusColor, 3.0f, 0, 1.0f);
        dl->AddText(ImVec2(statusBadgeX + 4.0f, curY), statusColor, statusPill);
    }

    curY += 18.0f;

    // Tabular Monospace Readout: Frequency & Cents
    char freqBuf[32];
    char centsBuf[32];
    if (result.confidence) {
        std::snprintf(freqBuf, sizeof(freqBuf), "%.1f Hz", result.frequencyHz);
        std::snprintf(centsBuf, sizeof(centsBuf), "%+0.1f ct", result.centDeviation);
    } else {
        std::snprintf(freqBuf, sizeof(freqBuf), "---.- Hz");
        std::snprintf(centsBuf, sizeof(centsBuf), "--.- ct");
    }

    if (g_fontMono) ImGui::PushFont(g_fontMono);
    dl->AddText(ImVec2(textStartX, curY), tokens.text.primary.u32, freqBuf);

    const ImVec2 centsSize = ImGui::CalcTextSize(centsBuf);
    const float centsX = textStartX + textAvailW - centsSize.x - 4.0f;
    const ImU32 centsColor = inTune ? tokens.signal.tunerInTune.u32
                           : (result.confidence ? (result.centDeviation < 0.0f ? tokens.signal.tunerFlat.u32 : tokens.signal.tunerSharp.u32)
                                                : tokens.text.muted.u32);
    if (centsX > textStartX + 60.0f) {
        dl->AddText(ImVec2(centsX, curY), centsColor, centsBuf);
    }
    if (g_fontMono) ImGui::PopFont();

    curY += 20.0f;

    // Mini Horizontal Deviation Track Bar
    const float barW = std::max(40.0f, textAvailW - 4.0f);
    const float barH = 7.0f;
    const ImVec2 barMin(textStartX, curY);
    const ImVec2 barMax(textStartX + barW, curY + barH);

    dl->AddRectFilled(barMin, barMax, tokens.surfaces.frameBg.u32, 2.0f);
    dl->AddRect(barMin, barMax, tokens.borders.subtle.u32, 2.0f, 0, 1.0f);

    const float midX = textStartX + barW * 0.5f;
    // Center in-tune tick
    dl->AddLine(ImVec2(midX, barMin.y - 1.0f), ImVec2(midX, barMax.y + 1.0f), tokens.signal.tunerInTune.u32, 1.5f);

    if (result.confidence) {
        const float norm = std::clamp((result.centDeviation + 50.0f) / 100.0f, 0.0f, 1.0f);
        const float markerX = textStartX + norm * barW;
        dl->AddRectFilled(ImVec2(markerX - 1.5f, barMin.y), ImVec2(markerX + 1.5f, barMax.y), centsColor, 1.0f);
    }

    ImGui::Dummy(ImVec2(availW, availH));
}

} // namespace praccy::ui

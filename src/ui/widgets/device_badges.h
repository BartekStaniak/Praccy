#pragma once

#include <imgui.h>
#include <string>
#include <algorithm>
#include <cmath>
#include "../design_tokens.h"

extern ImFont* g_fontUI;
extern ImFont* g_fontMono;

namespace praccy::ui {

enum class DeviceCategory {
    OverdriveStompbox,
    TubeAmpHead,
    StudioCompressor,
    TapeDelay,
    PlateReverb,
    Modulation,
    Equalizer,
    GenericProcessor
};

inline DeviceCategory detectDeviceCategory(const std::string& name, const std::string& vendor) {
    std::string lower = name + " " + vendor;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Check metering/reference/utility tools first so "MetricAB" never triggers "cab"
    if (lower.find("metricab") != std::string::npos || lower.find("metric ab") != std::string::npos ||
        lower.find("meter") != std::string::npos || lower.find("scope") != std::string::npos ||
        lower.find("spectrum") != std::string::npos || lower.find("loudness") != std::string::npos) {
        return DeviceCategory::GenericProcessor;
    }

    bool hasCompKeyword = (lower.find("1176") != std::string::npos || lower.find("la2a") != std::string::npos ||
                           lower.find("comp") != std::string::npos || lower.find("limit") != std::string::npos ||
                           lower.find("gate") != std::string::npos || lower.find("level") != std::string::npos ||
                           lower.find("opticom") != std::string::npos || lower.find("dynamic") != std::string::npos);

    bool hasAmpKeyword = (lower.find("amp") != std::string::npos || lower.find("archetype") != std::string::npos ||
                          lower.find("neural dsp") != std::string::npos || lower.find("head") != std::string::npos ||
                          lower.find("cab") != std::string::npos || lower.find("marshall") != std::string::npos ||
                          lower.find("fender") != std::string::npos || lower.find("mesa") != std::string::npos ||
                          lower.find("recto") != std::string::npos || lower.find("soldano") != std::string::npos ||
                          lower.find("lead") != std::string::npos || lower.find("crunch") != std::string::npos ||
                          lower.find("ampeg") != std::string::npos || lower.find("svt") != std::string::npos ||
                          lower.find("ir") != std::string::npos);

    if (hasAmpKeyword) {
        if (hasCompKeyword) return DeviceCategory::StudioCompressor;
        return DeviceCategory::TubeAmpHead;
    }

    if (lower.find("drive") != std::string::npos || lower.find("overdrive") != std::string::npos ||
        lower.find("dist") != std::string::npos || lower.find("fuzz") != std::string::npos ||
        lower.find("boost") != std::string::npos || lower.find("screamer") != std::string::npos ||
        lower.find("muff") != std::string::npos || lower.find("rat") != std::string::npos ||
        lower.find("klon") != std::string::npos || lower.find("od") != std::string::npos) {
        return DeviceCategory::OverdriveStompbox;
    }

    if (lower.find("eq") != std::string::npos || lower.find("filter") != std::string::npos ||
        lower.find("curve") != std::string::npos || lower.find("graphic") != std::string::npos ||
        lower.find("param") != std::string::npos) {
        return DeviceCategory::Equalizer;
    }

    if (lower.find("delay") != std::string::npos || lower.find("echo") != std::string::npos ||
        lower.find("tape") != std::string::npos || lower.find("space") != std::string::npos ||
        lower.find("repeat") != std::string::npos || lower.find("bbd") != std::string::npos) {
        return DeviceCategory::TapeDelay;
    }

    if (lower.find("verb") != std::string::npos || lower.find("reverb") != std::string::npos ||
        lower.find("hall") != std::string::npos || lower.find("room") != std::string::npos ||
        lower.find("plate") != std::string::npos || lower.find("spring") != std::string::npos ||
        lower.find("shimmer") != std::string::npos) {
        return DeviceCategory::PlateReverb;
    }

    if (lower.find("chorus") != std::string::npos || lower.find("flang") != std::string::npos ||
        lower.find("phas") != std::string::npos || lower.find("trem") != std::string::npos ||
        lower.find("vibe") != std::string::npos || lower.find("rotary") != std::string::npos ||
        lower.find("mod") != std::string::npos) {
        return DeviceCategory::Modulation;
    }

    if (hasCompKeyword) {
        return DeviceCategory::StudioCompressor;
    }

    return DeviceCategory::GenericProcessor;
}

inline const char* getCategoryTitle(DeviceCategory cat) {
    switch (cat) {
        case DeviceCategory::OverdriveStompbox: return "STOMPBOX OD";
        case DeviceCategory::TubeAmpHead:       return "TUBE AMP";
        case DeviceCategory::StudioCompressor:  return "COMPRESSOR";
        case DeviceCategory::TapeDelay:         return "TAPE DELAY";
        case DeviceCategory::PlateReverb:       return "PLATE REVERB";
        case DeviceCategory::Modulation:        return "MODULATION";
        case DeviceCategory::Equalizer:         return "PRECISION EQ";
        case DeviceCategory::GenericProcessor:  return "STUDIO FX";
    }
    return "DEVICE";
}

/**
 * @brief Renders a stylized vector hardware category faceplate badge.
 * Replaces unreliable GDI BitBlt desktop window scrapers with sharp, pro-audio hardware artwork.
 */
inline void DrawDeviceFaceplateBadge(
    ImDrawList* dl,
    ImVec2 pos,
    ImVec2 size,
    DeviceCategory cat,
    [[maybe_unused]] const std::string& pluginName,
    const std::string& vendorName,
    bool isBypassed,
    bool isHovered,
    bool isOpen
) {
    const auto& tokens = themeTokens();
    const float W = size.x;
    const float H = size.y;
    const ImVec2 maxPos(pos.x + W, pos.y + H);

    // 1. Chassis faceplate background
    ImU32 chassisBg = isBypassed ? tokens.surfaces.cardBgBypassed.u32 : tokens.surfaces.frameBg.u32;
    dl->AddRectFilled(pos, maxPos, chassisBg, 5.0f);

    // Chassis corner screws
    const float screwMargin = 5.0f;
    const float screwR = 2.0f;
    const ImVec2 corners[4] = {
        ImVec2(pos.x + screwMargin, pos.y + screwMargin),
        ImVec2(maxPos.x - screwMargin, pos.y + screwMargin),
        ImVec2(pos.x + screwMargin, maxPos.y - screwMargin),
        ImVec2(maxPos.x - screwMargin, maxPos.y - screwMargin)
    };
    for (const auto& c : corners) {
        dl->AddCircleFilled(c, screwR, tokens.borders.subtle.u32);
        dl->AddLine(ImVec2(c.x - 1.2f, c.y), ImVec2(c.x + 1.2f, c.y), tokens.borders.subtle.u32, 1.0f);
    }

    // 2. Hardware vector artwork on the left/center
    const float artX = pos.x + 16.0f;
    const float artY = pos.y + H * 0.5f;

    switch (cat) {
        case DeviceCategory::OverdriveStompbox: {
            // Stompbox enclosure outline
            ImVec2 boxMin(artX, artY - 18.0f);
            ImVec2 boxMax(artX + 32.0f, artY + 18.0f);
            dl->AddRectFilled(boxMin, boxMax, tokens.surfaces.buttonBg.u32, 3.0f);
            dl->AddRect(boxMin, boxMax, tokens.borders.cardHovered.u32, 3.0f, 0, 1.2f);

            // Footswitch nut & plunger
            dl->AddCircleFilled(ImVec2(artX + 16.0f, artY + 8.0f), 5.5f, tokens.borders.subtle.u32);
            dl->AddCircleFilled(ImVec2(artX + 16.0f, artY + 8.0f), 3.5f, tokens.surfaces.buttonBgHovered.u32);

            // Amber jewel LED
            ImU32 ledCol = isBypassed ? tokens.text.muted.u32 : tokens.signal.active.u32;
            dl->AddCircleFilled(ImVec2(artX + 16.0f, artY - 9.0f), 3.0f, ledCol);
            if (!isBypassed) {
                dl->AddCircle(ImVec2(artX + 16.0f, artY - 9.0f), 5.0f, tokens.signal.active.u32, 0, 1.0f);
            }
            break;
        }

        case DeviceCategory::TubeAmpHead: {
            // Woven grill cloth frame
            ImVec2 grillMin(artX, artY - 17.0f);
            ImVec2 grillMax(artX + 38.0f, artY + 17.0f);
            dl->AddRectFilled(grillMin, grillMax, tokens.surfaces.buttonBg.u32, 2.0f);
            dl->AddRect(grillMin, grillMax, tokens.borders.subtle.u32, 2.0f);

            // Vacuum tube filaments (warm amber glow)
            for (int t = 0; t < 3; ++t) {
                float tx = artX + 8.0f + static_cast<float>(t) * 11.0f;
                ImVec2 tTop(tx, artY - 8.0f);
                ImVec2 tBot(tx, artY + 8.0f);
                dl->AddLine(tTop, tBot, isBypassed ? tokens.text.muted.u32 : tokens.text.accent.u32, 2.2f);
                if (!isBypassed) {
                    dl->AddCircleFilled(ImVec2(tx, artY), 3.5f, tokens.text.accent.withAlpha(0.35f).u32);
                }
            }
            break;
        }

        case DeviceCategory::StudioCompressor: {
            // Vintage VU meter dial
            ImVec2 vuCenter(artX + 18.0f, artY + 14.0f);
            float vuR = 24.0f;
            dl->AddRectFilled(ImVec2(artX, artY - 16.0f), ImVec2(artX + 36.0f, artY + 16.0f), tokens.surfaces.buttonBg.u32, 3.0f);
            dl->AddRect(ImVec2(artX, artY - 16.0f), ImVec2(artX + 36.0f, artY + 16.0f), tokens.borders.subtle.u32, 3.0f);

            // VU scale arc
            constexpr float vuStart = 3.665f; // ~210 deg
            constexpr float vuEnd   = 5.759f; // ~330 deg
            dl->PathArcTo(vuCenter, vuR, vuStart, vuEnd, 16);
            dl->PathStroke(tokens.text.secondary.u32, 0, 1.5f);

            // Needle pointer (parked around -3dB when active)
            float needleAngle = isBypassed ? vuStart : (vuStart + 0.65f * (vuEnd - vuStart));
            ImVec2 nTip(vuCenter.x + std::cos(needleAngle) * (vuR - 3.0f), vuCenter.y + std::sin(needleAngle) * (vuR - 3.0f));
            dl->AddLine(vuCenter, nTip, isBypassed ? tokens.text.muted.u32 : tokens.signal.faulted.u32, 1.5f);
            break;
        }

        case DeviceCategory::TapeDelay: {
            // Dual tape reels
            float rR = 9.0f;
            ImVec2 r1(artX + 10.0f, artY);
            ImVec2 r2(artX + 28.0f, artY);

            dl->AddCircleFilled(r1, rR, tokens.surfaces.buttonBg.u32);
            dl->AddCircle(r1, rR, tokens.borders.cardHovered.u32, 0, 1.2f);
            dl->AddCircleFilled(r1, 3.0f, tokens.borders.subtle.u32);

            dl->AddCircleFilled(r2, rR, tokens.surfaces.buttonBg.u32);
            dl->AddCircle(r2, rR, tokens.borders.cardHovered.u32, 0, 1.2f);
            dl->AddCircleFilled(r2, 3.0f, tokens.borders.subtle.u32);

            // Magnetic tape path between reels
            dl->AddLine(ImVec2(r1.x, r1.y + rR), ImVec2(r2.x, r2.y + rR), isBypassed ? tokens.text.muted.u32 : tokens.signal.active.u32, 1.5f);
            break;
        }

        case DeviceCategory::PlateReverb: {
            // Suspended steel plate with acoustic wave ripples
            ImVec2 pMin(artX, artY - 14.0f);
            ImVec2 pMax(artX + 36.0f, artY + 14.0f);
            dl->AddRect(pMin, pMax, tokens.borders.cardHovered.u32, 1.0f, 0, 1.5f);

            // Driver center coil
            ImVec2 pCenter(artX + 18.0f, artY);
            dl->AddCircleFilled(pCenter, 3.5f, tokens.signal.active.u32);

            // Ripple arcs
            if (!isBypassed) {
                dl->AddCircle(pCenter, 8.0f, tokens.signal.active.withAlpha(0.45f).u32, 0, 1.0f);
                dl->AddCircle(pCenter, 13.0f, tokens.signal.active.withAlpha(0.25f).u32, 0, 1.0f);
            }
            break;
        }

        case DeviceCategory::Modulation: {
            // Dual phase-offset sine waves
            ImVec2 pts0[12];
            ImVec2 pts1[12];
            for (int i = 0; i < 12; ++i) {
                float frac = static_cast<float>(i) / 11.0f;
                float x = artX + frac * 36.0f;
                pts0[i] = ImVec2(x, artY + std::sin(frac * 6.2831853f) * 9.0f);
                pts1[i] = ImVec2(x, artY + std::cos(frac * 6.2831853f) * 9.0f);
            }
            dl->AddPolyline(pts0, 12, isBypassed ? tokens.text.muted.u32 : tokens.signal.branchA.u32, 0, 1.5f);
            dl->AddPolyline(pts1, 12, isBypassed ? tokens.text.muted.u32 : tokens.signal.branchB.u32, 0, 1.5f);
            break;
        }

        case DeviceCategory::Equalizer: {
            // Parametric EQ curve with bell peak
            ImVec2 eqPts[10];
            for (int i = 0; i < 10; ++i) {
                float frac = static_cast<float>(i) / 9.0f;
                float x = artX + frac * 36.0f;
                float bell = std::exp(-std::pow((frac - 0.5f) * 4.0f, 2.0f));
                eqPts[i] = ImVec2(x, artY + 6.0f - bell * 16.0f);
            }
            dl->AddPolyline(eqPts, 10, isBypassed ? tokens.text.muted.u32 : tokens.signal.active.u32, 0, 1.8f);
            dl->AddLine(ImVec2(artX, artY + 6.0f), ImVec2(artX + 36.0f, artY + 6.0f), tokens.borders.subtle.u32, 1.0f);
            break;
        }

        case DeviceCategory::GenericProcessor:
        default: {
            // Clean rack unit meter bars
            for (int bar = 0; bar < 4; ++bar) {
                float bx = artX + static_cast<float>(bar) * 9.0f;
                float bh = 8.0f + static_cast<float>(bar) * 6.0f;
                dl->AddRectFilled(ImVec2(bx, artY + 12.0f - bh), ImVec2(bx + 6.0f, artY + 12.0f),
                                  isBypassed ? tokens.text.muted.u32 : tokens.signal.active.u32, 1.0f);
            }
            break;
        }
    }

    // 3. Category & Vendor Badges
    const float labelStartX = artX + 46.0f;
    const char* catTitle = getCategoryTitle(cat);
    dl->AddText(ImVec2(labelStartX, pos.y + 10.0f),
                isBypassed ? tokens.text.muted.u32 : tokens.text.primary.u32,
                catTitle);

    std::string vendor = vendorName.empty() ? "Praccy Audio" : vendorName;
    if (vendor.size() > 18) {
        vendor = vendor.substr(0, 16) + "..";
    }
    dl->AddText(ImVec2(labelStartX, pos.y + 26.0f), tokens.text.secondary.u32, vendor.c_str());

    // 4. Status Tag (Open / Prompt)
    if (isOpen) {
        const char* openTag = "● OPEN";
        ImVec2 oSz = ImGui::CalcTextSize(openTag);
        ImVec2 oPos(pos.x + W - oSz.x - 8.0f, pos.y + 6.0f);
        dl->AddRectFilled(ImVec2(oPos.x - 3.0f, oPos.y - 1.0f),
                          ImVec2(oPos.x + oSz.x + 3.0f, oPos.y + oSz.y + 1.0f),
                          tokens.signal.active.withAlpha(0.25f).u32, 3.0f);
        dl->AddText(oPos, tokens.signal.active.u32, openTag);
    }

    // 5. Hover overlay with action prompt
    if (isHovered) {
        dl->AddRectFilled(pos, maxPos, tokens.surfaces.modalOverlay.u32, 5.0f);
        const char* expandPrompt = isOpen ? "FOCUS WINDOW ↗" : "EXPAND GUI ↗";
        ImVec2 pSz = ImGui::CalcTextSize(expandPrompt);
        ImVec2 pPos(pos.x + (W - pSz.x) * 0.5f, pos.y + (H - pSz.y) * 0.5f);
        dl->AddRectFilled(ImVec2(pPos.x - 8.0f, pPos.y - 3.0f),
                          ImVec2(pPos.x + pSz.x + 8.0f, pPos.y + pSz.y + 3.0f),
                          tokens.surfaces.popupBg.u32, 4.0f);
        dl->AddText(pPos, tokens.text.primary.u32, expandPrompt);
    }
}

} // namespace praccy::ui

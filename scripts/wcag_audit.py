#!/usr/bin/env python3
"""
Independent WCAG 2.1 Contrast Ratio Oracle for Praccy v2.0
Directly parses src/ui/design_tokens.h and computes exact sRGB gamma decompression
and relative luminance:
L = 0.2126 * R + 0.7152 * G + 0.0722 * B
Contrast Ratio = (L1 + 0.05) / (L2 + 0.05)
"""

import sys
import os
import re

def srgb_to_linear(c_srgb: float) -> float:
    if c_srgb <= 0.04045:
        return c_srgb / 12.92
    return ((c_srgb + 0.055) / 1.055) ** 2.4

def relative_luminance(r: int, g: int, b: int) -> float:
    r_lin = srgb_to_linear(r / 255.0)
    g_lin = srgb_to_linear(g / 255.0)
    b_lin = srgb_to_linear(b / 255.0)
    return 0.2126 * r_lin + 0.7152 * g_lin + 0.0722 * b_lin

def contrast_ratio(rgb1, rgb2) -> float:
    l1 = relative_luminance(*rgb1)
    l2 = relative_luminance(*rgb2)
    if l1 < l2:
        l1, l2 = l2, l1
    return (l1 + 0.05) / (l2 + 0.05)

SURFACE_FIELDS = [
    "windowBg", "panelBg", "cardBg", "cardBgHovered", "cardBgActive", "headerBg",
    "cardBgFaulted", "frameBg", "frameBgHovered", "frameBgActive", "popupBg",
    "modalOverlay", "buttonBg", "buttonBgHovered", "buttonBgActive", "slotBg",
    "slotBgHovered", "slotBgActive", "tabBg", "tabBgHovered", "tabBgActive",
    "meterTrack", "scrollbarBg", "scrollbarGrab", "scrollbarGrabActive", "dropZoneBg",
    "branchABg", "branchABgHeader", "branchBBg", "branchBBgHeader", "toastBg",
    "sceneActiveEmerald"
]

BORDER_FIELDS = [
    "subtle", "strong", "focus", "card", "cardHovered", "cardGlowActive",
    "cardBypassed", "cardFaulted", "cardGlowOpen", "accentGreen", "separator",
    "shadow", "branchABorder", "branchBBorder", "headerDivider", "mapModeViolet"
]

TEXT_FIELDS = [
    "primary", "secondary", "muted", "accent", "inverse",
    "badgeVst3", "badgeClap", "badgeBuiltin", "success", "warning", "error", "monoValues"
]

SIGNAL_FIELDS = [
    "active", "bypassed", "faulted", "recording", "overdubbing", "playing",
    "stopped", "levelGood", "levelNominal", "levelWarning", "levelClip",
    "tunerInTune", "tunerSharp", "tunerFlat", "meterSignal", "meterWarning"
]

def parse_design_tokens(header_path: str):
    if not os.path.exists(header_path):
        raise FileNotFoundError(f"Cannot find {header_path}")
    
    with open(header_path, "r", encoding="utf-8") as f:
        content = f.read()

    # Find kThemes definition array
    match = re.search(r"inline const ThemeTokens kThemes\[static_cast<size_t>\(ThemeId::Count\)\]\s*=\s*\{([\s\S]*?)\n\};", content)
    if not match:
        raise ValueError("Could not locate kThemes in design_tokens.h")
    
    body = match.group(1)
    # Split into theme blocks by theme comments or ThemeId
    theme_matches = list(re.finditer(r'ThemeId::(\w+),\s*"([^"]+)",\s*(true|false),', body))
    if not theme_matches:
        raise ValueError("Could not locate individual themes in kThemes")
    
    parsed_themes = {}
    for i, tmatch in enumerate(theme_matches):
        theme_id_name = tmatch.group(1)
        theme_display_name = tmatch.group(2)
        start_idx = tmatch.end()
        end_idx = theme_matches[i + 1].start() if i + 1 < len(theme_matches) else len(body)
        theme_body = body[start_idx:end_idx]

        # Extract all ColorToken(r, g, b, [a])
        all_tokens = re.findall(r'ColorToken\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)(?:\s*,\s*(\d+))?\s*\)', theme_body)
        rgb_list = [(int(r), int(g), int(b)) for r, g, b, _ in all_tokens]

        idx = 0
        surfaces = {}
        for name in SURFACE_FIELDS:
            surfaces[name] = rgb_list[idx]
            idx += 1

        borders = {}
        for name in BORDER_FIELDS:
            borders[name] = rgb_list[idx]
            idx += 1

        text = {}
        for name in TEXT_FIELDS:
            text[name] = rgb_list[idx]
            idx += 1

        signal = {}
        for name in SIGNAL_FIELDS:
            signal[name] = rgb_list[idx]
            idx += 1

        parsed_themes[theme_display_name] = {
            "surfaces": surfaces,
            "borders": borders,
            "text": text,
            "signal": signal,
        }

    return parsed_themes

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.dirname(script_dir)
    header_path = os.path.join(repo_root, "src", "ui", "design_tokens.h")

    print("=" * 80)
    print(f"WCAG 2.1 AA DIRECT AUDIT: {header_path}")
    print("=" * 80)

    try:
        themes = parse_design_tokens(header_path)
    except Exception as e:
        print(f"ERROR: Failed to parse design_tokens.h: {e}")
        sys.exit(1)

    all_passed = True
    for theme_name, tokens in themes.items():
        print(f"\n--- Theme: {theme_name} ---")
        surfaces = tokens["surfaces"]
        text = tokens["text"]
        borders = tokens["borders"]
        signal = tokens["signal"]

        tests = [
            # Requirement 1: Primary text vs Card / Window / Panel background (>= 4.5:1)
            ("Primary text vs Card background", text["primary"], surfaces["cardBg"], 4.5),
            ("Primary text vs Window background", text["primary"], surfaces["windowBg"], 4.5),
            ("Primary text vs Panel background", text["primary"], surfaces["panelBg"], 4.5),

            # Requirement 2: Secondary text vs Card / Window / Panel background (>= 4.5:1)
            ("Secondary text vs Card background", text["secondary"], surfaces["cardBg"], 4.5),
            ("Secondary text vs Window background", text["secondary"], surfaces["windowBg"], 4.5),
            ("Secondary text vs Panel background", text["secondary"], surfaces["panelBg"], 4.5),

            # Requirement 3: Muted text vs Card / Window / Panel background (>= 4.5:1)
            ("Muted text vs Card background", text["muted"], surfaces["cardBg"], 4.5),
            ("Muted text vs Window background", text["muted"], surfaces["windowBg"], 4.5),
            ("Muted text vs Panel background", text["muted"], surfaces["panelBg"], 4.5),

            # Requirement 4: Primary text vs sceneActiveEmerald (>= 4.5:1)
            ("Primary text vs Active Scene Emerald", text["primary"], surfaces["sceneActiveEmerald"], 4.5),

            # Graphical UI controls (>= 3.0:1)
            ("Focus border vs Card background", borders["focus"], surfaces["cardBg"], 3.0),
            ("Focus border vs Window background", borders["focus"], surfaces["windowBg"], 3.0),
            ("Active signal vs Card background", signal["active"], surfaces["cardBg"], 3.0),
            ("Active signal vs Window background", signal["active"], surfaces["windowBg"], 3.0),
            ("Text accent vs Card background", text["accent"], surfaces["cardBg"], 3.0),
            ("Text accent vs Window background", text["accent"], surfaces["windowBg"], 3.0),
        ]

        for desc, fg, bg, threshold in tests:
            cr = contrast_ratio(fg, bg)
            passed = cr >= threshold
            status = "PASS" if passed else "FAIL"
            if not passed:
                all_passed = False
            print(f"  [{status}] {desc:<45}: {cr:6.2f}:1  (min {threshold:.1f}:1)")

    print("\n" + "=" * 80)
    if all_passed:
        print("OVERALL ORACLE RESULT: ALL REQUIRED TOKEN PAIRS SATISFY WCAG 2.1 AA REQUIREMENTS!")
        print("=" * 80)
        sys.exit(0)
    else:
        print("OVERALL ORACLE RESULT: CONTRAST DEFICIENCIES FOUND!")
        print("=" * 80)
        sys.exit(1)

if __name__ == "__main__":
    main()

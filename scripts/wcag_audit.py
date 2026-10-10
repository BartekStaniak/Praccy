#!/usr/bin/env python3
"""
Independent WCAG 2.1 Contrast Ratio Oracle for Praccy v2.0
Computes exact sRGB gamma decompression and relative luminance:
L = 0.2126 * R + 0.7152 * G + 0.0722 * B
Contrast Ratio = (L1 + 0.05) / (L2 + 0.05)
"""

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

themes = {
    "Obsidian Studio": {
        "surfaces": {
            "windowBg": (18, 19, 22),
            "panelBg": (22, 24, 28),
            "cardBg": (28, 31, 38),
            "frameBg": (34, 38, 46),
            "buttonBg": (42, 47, 58),
            "tabBg": (26, 29, 35),
            "modalOverlay": (8, 10, 12),
        },
        "borders": {
            "subtle": (46, 52, 64),
            "strong": (70, 78, 96),
            "focus": (255, 179, 38),
            "card": (50, 56, 70),
            "cardHovered": (85, 140, 220),
            "cardGlowActive": (255, 179, 38),
        },
        "text": {
            "primary": (242, 244, 248),
            "secondary": (168, 175, 188),
            "muted": (116, 124, 138),
            "accent": (255, 179, 38),
            "inverse": (18, 19, 22),
            "success": (52, 199, 89),
            "warning": (255, 179, 38),
            "error": (235, 64, 52),
        },
        "signal": {
            "active": (52, 199, 89),
            "bypassed": (84, 92, 106),
            "faulted": (235, 64, 52),
            "recording": (245, 50, 50),
            "overdubbing": (255, 160, 35),
            "playing": (52, 199, 89),
        }
    },
    "Cyber / Midnight": {
        "surfaces": {
            "windowBg": (10, 14, 23),
            "panelBg": (13, 18, 30),
            "cardBg": (18, 25, 42),
            "frameBg": (24, 34, 56),
            "buttonBg": (30, 44, 72),
            "tabBg": (18, 24, 38),
            "modalOverlay": (5, 7, 14),
        },
        "borders": {
            "subtle": (36, 54, 88),
            "strong": (60, 90, 145),
            "focus": (0, 240, 255),
            "card": (40, 65, 105),
            "cardHovered": (0, 240, 255),
            "cardGlowActive": (0, 240, 255),
        },
        "text": {
            "primary": (240, 248, 255),
            "secondary": (155, 185, 220),
            "muted": (100, 125, 155),
            "accent": (0, 240, 255),
            "inverse": (10, 14, 23),
            "success": (0, 230, 160),
            "warning": (255, 185, 30),
            "error": (255, 42, 85),
        },
        "signal": {
            "active": (0, 240, 255),
            "bypassed": (60, 75, 98),
            "faulted": (255, 42, 85),
            "recording": (255, 30, 80),
            "overdubbing": (255, 160, 20),
            "playing": (0, 230, 160),
        }
    },
    "Nordic Slate": {
        "surfaces": {
            "windowBg": (22, 27, 34),
            "panelBg": (27, 34, 42),
            "cardBg": (34, 43, 54),
            "frameBg": (40, 50, 64),
            "buttonBg": (48, 62, 78),
            "tabBg": (30, 38, 48),
            "modalOverlay": (12, 16, 20),
        },
        "borders": {
            "subtle": (54, 68, 86),
            "strong": (78, 98, 122),
            "focus": (88, 184, 216),
            "card": (58, 74, 92),
            "cardHovered": (88, 184, 216),
            "cardGlowActive": (88, 184, 216),
        },
        "text": {
            "primary": (244, 248, 250),
            "secondary": (165, 185, 198),
            "muted": (115, 132, 145),
            "accent": (88, 184, 216),
            "inverse": (22, 27, 34),
            "success": (69, 194, 154),
            "warning": (230, 175, 80),
            "error": (224, 82, 82),
        },
        "signal": {
            "active": (69, 194, 154),
            "bypassed": (75, 90, 102),
            "faulted": (224, 82, 82),
            "recording": (230, 75, 75),
            "overdubbing": (226, 167, 98),
            "playing": (69, 194, 154),
        }
    },
    "Vintage Console": {
        "surfaces": {
            "windowBg": (236, 230, 218),
            "panelBg": (242, 237, 226),
            "cardBg": (248, 245, 238),
            "frameBg": (226, 218, 202),
            "buttonBg": (218, 208, 190),
            "tabBg": (228, 220, 204),
            "modalOverlay": (40, 34, 28),
        },
        "borders": {
            "subtle": (196, 184, 164),
            "strong": (156, 142, 120),
            "focus": (184, 75, 24),
            "card": (185, 172, 152),
            "cardHovered": (184, 75, 24),
            "cardGlowActive": (184, 75, 24),
        },
        "text": {
            "primary": (36, 28, 22),
            "secondary": (95, 80, 68),
            "muted": (145, 130, 115),
            "accent": (184, 75, 24),
            "inverse": (248, 245, 238),
            "success": (38, 135, 68),
            "warning": (184, 75, 24),
            "error": (195, 45, 35),
        },
        "signal": {
            "active": (38, 135, 68),
            "bypassed": (150, 140, 128),
            "faulted": (195, 45, 35),
            "recording": (200, 40, 30),
            "overdubbing": (195, 95, 20),
            "playing": (38, 135, 68),
        }
    }
}

def main():
    print("=" * 80)
    print("INDEPENDENT WCAG 2.1 CONTRAST RATIO AUDIT (ALL 4 THEMES)")
    print("=" * 80)
    
    all_passed = True
    for theme_name, tokens in themes.items():
        print(f"\n--- Theme: {theme_name} ---")
        surfaces = tokens["surfaces"]
        text = tokens["text"]
        borders = tokens["borders"]
        signal = tokens["signal"]
        
        # Test required pairs
        tests = [
            # Requirement 1: Primary text vs Card background (>= 4.5:1)
            ("Primary text vs Card background", text["primary"], surfaces["cardBg"], 4.5),
            # Requirement 2: Primary text vs Window background (>= 4.5:1)
            ("Primary text vs Window background", text["primary"], surfaces["windowBg"], 4.5),
            # Requirement 3: Secondary text vs Card background (>= 3.0:1)
            ("Secondary text vs Card background", text["secondary"], surfaces["cardBg"], 3.0),
            # Requirement 3b: Secondary text vs Window background (>= 3.0:1)
            ("Secondary text vs Window background", text["secondary"], surfaces["windowBg"], 3.0),
            # Requirement 4: Graphical border/control accents vs background (>= 3.0:1)
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
        print("OVERALL ORACLE RESULT: ALL REQUIRED TOKEN PAIRS SATISFY WCAG 2.1 REQUIREMENTS!")
    else:
        print("OVERALL ORACLE RESULT: CONTRAST DEFICIENCIES FOUND!")
    print("=" * 80)

if __name__ == "__main__":
    main()

def srgb_to_lin(c):
    c_s = c / 255.0
    return c_s / 12.92 if c_s <= 0.04045 else ((c_s + 0.055) / 1.055) ** 2.4

def lum(rgb):
    return 0.2126 * srgb_to_lin(rgb[0]) + 0.7152 * srgb_to_lin(rgb[1]) + 0.0722 * srgb_to_lin(rgb[2])

def cr(rgb1, rgb2):
    l1 = lum(rgb1)
    l2 = lum(rgb2)
    lighter = max(l1, l2)
    darker = min(l1, l2)
    return (lighter + 0.05) / (darker + 0.05)

themes = {
    "ObsidianStudio": {
        "name": "Obsidian Studio",
        "description": "Charcoal/black with studio amber/warm accents",
        "windowBg": (18, 19, 22),       # #121316
        "panelBg": (22, 24, 28),        # #16181C
        "cardBg": (28, 31, 38),         # #1C1F26
        "cardHover": (36, 40, 50),      # #242832
        "frameBg": (34, 38, 46),        # #22262E
        "buttonBg": (42, 47, 58),       # #2A2F3A
        "textPri": (242, 244, 248),     # #F2F4F8
        "textSec": (168, 175, 188),     # #A8AFBC
        "textMut": (116, 124, 138),     # #747C8A
        "accent": (255, 179, 38),       # #FFB326 Studio Amber
        "border": (54, 60, 74),         # #363C4A
        "signalActive": (52, 199, 89),  # #34C759 Active Green
        "signalByp": (84, 92, 106),     # #545C6A Bypassed Slate
        "signalFault": (235, 64, 52),   # #EB4034 Fault Ruby
        "cableCore": (255, 185, 45),    # #FFB92D Warm Gold Core
        "cableSleeve": (50, 42, 30),    # #322A1E Sleeve
    },
    "CyberMidnight": {
        "name": "Cyber / Midnight",
        "description": "Neon cyan/magenta on midnight navy",
        "windowBg": (10, 14, 23),       # #0A0E17
        "panelBg": (13, 18, 30),        # #0D121E
        "cardBg": (18, 25, 42),         # #12192A
        "cardHover": (25, 36, 60),      # #19243C
        "frameBg": (24, 34, 56),        # #182238
        "buttonBg": (30, 44, 72),       # #1E2C48
        "textPri": (240, 248, 255),     # #F0F8FF Alice Frost
        "textSec": (155, 185, 220),     # #9BB9DC Neon Frost
        "textMut": (100, 125, 155),     # #647D9B Muted Navy
        "accent": (0, 240, 255),        # #00F0FF Electric Neon Cyan
        "border": (40, 65, 105),        # #284169
        "signalActive": (0, 230, 160),  # #00E6A0 Cyber Mint
        "signalByp": (60, 75, 98),      # #3C4B62
        "signalFault": (255, 42, 85),   # #FF2A55 Vivid Crimson
        "cableCore": (0, 240, 255),     # #00F0FF Cyan Core
        "cableSleeve": (15, 45, 70),    # #0F2D46
    },
    "NordicSlate": {
        "name": "Nordic Slate",
        "description": "Muted slate blue, soft frost white, scandinavian minimal",
        "windowBg": (22, 27, 34),       # #161B22
        "panelBg": (27, 34, 42),        # #1B222A
        "cardBg": (34, 43, 54),         # #222B36
        "cardHover": (44, 56, 70),      # #2C3846
        "frameBg": (40, 50, 64),        # #283240
        "buttonBg": (48, 62, 78),       # #303E4E
        "textPri": (244, 248, 250),     # #F4F8FA Soft Frost White
        "textSec": (165, 185, 198),     # #A5B9C6 Glacial Mist
        "textMut": (115, 132, 145),     # #738491 Nordic Stone
        "accent": (88, 184, 216),       # #58B8D8 Glacial Ice Blue
        "border": (58, 74, 92),         # #3A4A5C
        "signalActive": (69, 194, 154), # #45C29A Frost Teal
        "signalByp": (75, 90, 102),     # #4B5A66
        "signalFault": (224, 82, 82),   # #E05252 Nordic Coral
        "cableCore": (95, 195, 225),    # #5FC3E1
        "cableSleeve": (28, 48, 60),    # #1C303C
    },
    "VintageConsole": {
        "name": "Vintage Console",
        "description": "Warm analog parchment/cream, bakelite knob accents, vintage tape saturation hues",
        "windowBg": (236, 230, 218),    # #ECE6DA Warm Parchment
        "panelBg": (242, 237, 226),     # #F2EDE2 Muted Cream
        "cardBg": (248, 245, 238),      # #F8F5EE Studio Ivory Card
        "cardHover": (240, 234, 220),   # #F0EADC
        "frameBg": (226, 218, 202),     # #E2DACA Inset Parchment
        "buttonBg": (218, 208, 190),    # #DAD0BE Bakelite Cream
        "textPri": (36, 28, 22),        # #241C16 Deep Bakelite Espresso
        "textSec": (95, 80, 68),        # #5F5044 Aged Tobacco
        "textMut": (145, 130, 115),     # #918273 Warm Faded Sepia
        "accent": (184, 75, 24),        # #B84B18 Vintage Tape Saturation Rust/Amber
        "border": (185, 172, 152),      # #B9AC98 Console Trim Bronze
        "signalActive": (38, 135, 68),  # #268744 Analog Meter Green
        "signalByp": (150, 140, 128),   # #968C80
        "signalFault": (195, 45, 35),   # #C32D23 Console Alert Red
        "cableCore": (180, 80, 30),     # #B4501E Tape Saturation Core
        "cableSleeve": (210, 195, 175),  # #D2C3AF Warm Parchment Sleeve
    }
}

for tid, t in themes.items():
    print(f"==================================================")
    print(f"THEME: {t['name']} ({tid})")
    print(f"Description: {t['description']}")
    print(f"==================================================")
    print(f"Luminance WindowBg: {lum(t['windowBg']):.5f}")
    print(f"Luminance CardBg:   {lum(t['cardBg']):.5f}")
    print(f"Luminance TextPri:  {lum(t['textPri']):.5f}")
    print(f"Luminance TextSec:  {lum(t['textSec']):.5f}")
    print(f"Luminance TextMut:  {lum(t['textMut']):.5f}")
    print(f"Luminance Accent:   {lum(t['accent']):.5f}")
    print()

    # Contrast ratios
    cr_pri_card = cr(t['textPri'], t['cardBg'])
    cr_pri_win  = cr(t['textPri'], t['windowBg'])
    cr_sec_card = cr(t['textSec'], t['cardBg'])
    cr_mut_card = cr(t['textMut'], t['cardBg'])
    cr_acc_card = cr(t['accent'], t['cardBg'])
    cr_sig_act  = cr(t['signalActive'], t['cardBg'])
    cr_sig_flt  = cr(t['signalFault'], t['cardBg'])
    cr_bdr_card = cr(t['border'], t['cardBg'])

    print(f"Normal Text (WCAG AA >= 4.5:1, AAA >= 7.0:1):")
    print(f"  Primary Text vs CardBg:   {cr_pri_card:6.2f}:1  {'PASS AA & AAA' if cr_pri_card >= 7.0 else ('PASS AA' if cr_pri_card >= 4.5 else 'FAIL')}")
    print(f"  Primary Text vs WindowBg: {cr_pri_win:6.2f}:1  {'PASS AA & AAA' if cr_pri_win >= 7.0 else ('PASS AA' if cr_pri_win >= 4.5 else 'FAIL')}")
    print(f"  Secondary Text vs CardBg: {cr_sec_card:6.2f}:1  {'PASS AA & AAA' if cr_sec_card >= 7.0 else ('PASS AA' if cr_sec_card >= 4.5 else 'FAIL')}")
    print(f"  Muted Text vs CardBg:     {cr_mut_card:6.2f}:1  {'PASS AA' if cr_mut_card >= 4.5 else ('PASS UI/Large' if cr_mut_card >= 3.0 else 'Sub-3.0 (decorative/disabled)')}")
    print()
    print(f"UI Components & Graphics (WCAG AA >= 3.0:1):")
    print(f"  Accent vs CardBg:         {cr_acc_card:6.2f}:1  {'PASS UI' if cr_acc_card >= 3.0 else 'FAIL'}")
    print(f"  Signal Active vs CardBg:  {cr_sig_act:6.2f}:1  {'PASS UI' if cr_sig_act >= 3.0 else 'FAIL'}")
    print(f"  Signal Fault vs CardBg:   {cr_sig_flt:6.2f}:1  {'PASS UI' if cr_sig_flt >= 3.0 else 'FAIL'}")
    print(f"  Border vs CardBg:         {cr_bdr_card:6.2f}:1  (Structural delineation)")
    print()

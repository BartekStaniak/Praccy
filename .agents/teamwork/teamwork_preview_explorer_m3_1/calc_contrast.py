def srgb_to_lin(c):
    c_s = c / 255.0
    return c_s / 12.92 if c_s <= 0.04045 else ((c_s + 0.055) / 1.055) ** 2.4

def relative_luminance(r, g, b):
    r_lin = srgb_to_lin(r)
    g_lin = srgb_to_lin(g)
    b_lin = srgb_to_lin(b)
    return 0.2126 * r_lin + 0.7152 * g_lin + 0.0722 * b_lin

def contrast_ratio(rgb1, rgb2):
    l1 = relative_luminance(*rgb1)
    l2 = relative_luminance(*rgb2)
    lighter = max(l1, l2)
    darker = min(l1, l2)
    return (lighter + 0.05) / (darker + 0.05)

# Test Obsidian Studio
obsidian_bg = (18, 19, 22)       # #121316 WindowBg
obsidian_card = (24, 26, 31)     # #181A1F CardBg
obsidian_frame = (32, 35, 42)    # #20232A FrameBg
obsidian_text_pri = (242, 244, 248) # #F2F4F8 Primary text
obsidian_text_sec = (175, 182, 195) # #AFB6C3 Secondary text
obsidian_text_mut = (125, 133, 148) # #7D8594 Muted text
obsidian_amber = (255, 179, 38)     # #FFB326 Studio Amber accent
obsidian_signal_ok = (52, 199, 89)  # #34C759
obsidian_signal_err = (235, 64, 52) # #EB4034

print("=== OBSIDIAN STUDIO ===")
print("Text Pri vs Card:", contrast_ratio(obsidian_text_pri, obsidian_card))
print("Text Pri vs Window:", contrast_ratio(obsidian_text_pri, obsidian_bg))
print("Text Sec vs Card:", contrast_ratio(obsidian_text_sec, obsidian_card))
print("Amber Accent vs Card (UI):", contrast_ratio(obsidian_amber, obsidian_card))
print("Signal OK vs Card (UI):", contrast_ratio(obsidian_signal_ok, obsidian_card))
print("Signal Err vs Card (UI):", contrast_ratio(obsidian_signal_err, obsidian_card))

# Test Cyber / Midnight
cyber_bg = (10, 14, 23)          # #0A0E17 WindowBg
cyber_card = (15, 22, 38)        # #0F1626 CardBg
cyber_frame = (22, 32, 53)       # #162035 FrameBg
cyber_text_pri = (240, 248, 255) # #F0F8FF Primary text (Alice Blue)
cyber_text_sec = (150, 185, 220) # #96B9DC Secondary text
cyber_cyan = (0, 240, 255)       # #00F0FF Neon Cyan
cyber_magenta = (255, 0, 128)    # #FF0080 Neon Magenta
cyber_signal_ok = (0, 230, 150)
cyber_signal_err = (255, 42, 85)

print("\n=== CYBER MIDNIGHT ===")
print("Text Pri vs Card:", contrast_ratio(cyber_text_pri, cyber_card))
print("Text Pri vs Window:", contrast_ratio(cyber_text_pri, cyber_bg))
print("Text Sec vs Card:", contrast_ratio(cyber_text_sec, cyber_card))
print("Cyan Accent vs Card (UI):", contrast_ratio(cyber_cyan, cyber_card))
print("Magenta Accent vs Card (UI):", contrast_ratio(cyber_magenta, cyber_card))

# Test Nordic Slate
nordic_bg = (24, 30, 36)         # #181E24 WindowBg
nordic_card = (32, 39, 48)       # #202730 CardBg
nordic_frame = (42, 51, 62)      # #2A333E FrameBg
nordic_text_pri = (244, 248, 250) # #F4F8FA Frost White
nordic_text_sec = (165, 182, 195) # #A5B6C3 Glacial Mist
nordic_ice_blue = (88, 184, 216)  # #58B8D8 Glacial Ice Blue
nordic_signal_ok = (69, 194, 154) # #45C29A Frost Teal
nordic_signal_err = (224, 82, 82) # #E05252 Nordic Coral

print("\n=== NORDIC SLATE ===")
print("Text Pri vs Card:", contrast_ratio(nordic_text_pri, nordic_card))
print("Text Pri vs Window:", contrast_ratio(nordic_text_pri, nordic_bg))
print("Text Sec vs Card:", contrast_ratio(nordic_text_sec, nordic_card))
print("Ice Blue Accent vs Card (UI):", contrast_ratio(nordic_ice_blue, nordic_card))

# Test Vintage Console - evaluate both Warm Console Dark and Analog Parchment Light
vintage_dark_bg = (30, 26, 22)      # #1E1A16 WindowBg (Dark Console)
vintage_dark_card = (42, 36, 30)    # #2A241E CardBg (Dark Console)
vintage_dark_frame = (56, 48, 40)   # #383028 FrameBg
vintage_dark_text_pri = (248, 240, 224) # #F8F0E0 Warm Parchment Cream
vintage_dark_text_sec = (196, 182, 160) # #C4B6A0 Muted Parchment
vintage_dark_amber = (228, 140, 48) # #E48C30 Tape Saturation Amber
vintage_dark_bakelite = (184, 75, 41) # #B84B29 Bakelite Rust

print("\n=== VINTAGE CONSOLE (Dark Studio Console) ===")
print("Text Pri vs Card:", contrast_ratio(vintage_dark_text_pri, vintage_dark_card))
print("Text Pri vs Window:", contrast_ratio(vintage_dark_text_pri, vintage_dark_bg))
print("Text Sec vs Card:", contrast_ratio(vintage_dark_text_sec, vintage_dark_card))
print("Amber vs Card (UI):", contrast_ratio(vintage_dark_amber, vintage_dark_card))

# Vintage Console as Parchment Light theme:
vintage_light_bg = (238, 232, 220)   # #EEE8DC Warm Parchment
vintage_light_card = (248, 244, 235) # #F8F4EB Cream Surface
vintage_light_frame = (225, 217, 202) # #E1D9CA Inset frame
vintage_light_text_pri = (38, 30, 24) # #261E18 Deep Bakelite Espresso
vintage_light_text_sec = (92, 78, 66) # #5C4E42 Muted Tobacco
vintage_light_amber = (180, 85, 20)  # #B45514 Warm Tape Amber
vintage_light_bakelite = (140, 40, 25)

print("\n=== VINTAGE CONSOLE (Analog Parchment Light) ===")
print("Text Pri vs Card:", contrast_ratio(vintage_light_text_pri, vintage_light_card))
print("Text Pri vs Window:", contrast_ratio(vintage_light_text_pri, vintage_light_bg))
print("Text Sec vs Card:", contrast_ratio(vintage_light_text_sec, vintage_light_card))
print("Amber vs Card (UI):", contrast_ratio(vintage_light_amber, vintage_light_card))

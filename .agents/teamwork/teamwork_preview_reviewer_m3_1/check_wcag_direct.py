import math

def rel_lum(r, g, b):
    def lin(c):
        c = c / 255.0
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)

def cr(c1, c2):
    l1 = rel_lum(*c1)
    l2 = rel_lum(*c2)
    if l1 < l2:
        l1, l2 = l2, l1
    return (l1 + 0.05) / (l2 + 0.05)

themes = {
    'Obsidian Studio': {
        'windowBg': (18, 19, 22), 'panelBg': (22, 24, 28), 'cardBg': (28, 31, 38),
        'textPrimary': (242, 244, 248), 'textSecondary': (168, 175, 188),
        'focus': (255, 179, 38), 'active': (52, 199, 89)
    },
    'Cyber / Midnight': {
        'windowBg': (10, 14, 23), 'panelBg': (13, 18, 30), 'cardBg': (18, 25, 42),
        'textPrimary': (240, 248, 255), 'textSecondary': (155, 185, 220),
        'focus': (0, 240, 255), 'active': (0, 240, 255)
    },
    'Nordic Slate': {
        'windowBg': (22, 27, 34), 'panelBg': (27, 34, 42), 'cardBg': (34, 43, 54),
        'textPrimary': (244, 248, 250), 'textSecondary': (165, 185, 198),
        'focus': (88, 184, 216), 'active': (69, 194, 154)
    },
    'Vintage Console': {
        'windowBg': (236, 230, 218), 'panelBg': (242, 237, 226), 'cardBg': (248, 245, 238),
        'textPrimary': (36, 28, 22), 'textSecondary': (95, 80, 68),
        'focus': (184, 75, 24), 'active': (38, 135, 68)
    }
}

for name, t in themes.items():
    print(f"=== {name} ===")
    cr_win = cr(t['textPrimary'], t['windowBg'])
    cr_pnl = cr(t['textPrimary'], t['panelBg'])
    cr_crd = cr(t['textPrimary'], t['cardBg'])
    cr_sec = cr(t['textSecondary'], t['windowBg'])
    cr_foc = cr(t['focus'], t['cardBg'])
    cr_act = cr(t['active'], t['windowBg'])
    print(f"  textPrimary vs windowBg: {cr_win:.2f}:1 (AA >= 4.5: {cr_win >= 4.5})")
    print(f"  textPrimary vs panelBg:  {cr_pnl:.2f}:1 (AA >= 4.5: {cr_pnl >= 4.5})")
    print(f"  textPrimary vs cardBg:   {cr_crd:.2f}:1 (AA >= 4.5: {cr_crd >= 4.5})")
    print(f"  textSecondary vs winBg:  {cr_sec:.2f}:1 (UI >= 3.0: {cr_sec >= 3.0})")
    print(f"  focus vs cardBg:         {cr_foc:.2f}:1 (UI >= 3.0: {cr_foc >= 3.0})")
    print(f"  active vs windowBg:      {cr_act:.2f}:1 (UI >= 3.0: {cr_act >= 3.0})")

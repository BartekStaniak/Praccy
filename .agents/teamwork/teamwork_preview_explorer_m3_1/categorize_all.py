import re

with open("f:/Projects/Praccy/src/ui/rack_view.cpp", "r", encoding="utf-8") as f:
    lines = f.readlines()

pattern = re.compile(r'IM_COL32\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+|\w+)\s*\)')

categories = {
    "Card Surfaces": [],
    "Card Borders & Glows": [],
    "Buttons & Badges": [],
    "Cables & Sockets": [],
    "Meters & Signal States": [],
    "Text & Typography": [],
    "Chassis & Containers": [],
    "Knobs & Controls": [],
    "Other": []
}

for idx, line in enumerate(lines, 1):
    matches = pattern.findall(line)
    if not matches:
        continue
    line_str = line.strip()
    # Categorization heuristic
    if any(k in line_str for k in ["renderBadgePill", "PushStyleColor(ImGuiCol_Button", "CenteredButton", "roundBtn"]):
        categories["Buttons & Badges"].append((idx, line_str))
    elif any(k in line_str for k in ["AddBezierCubic", "AddLine", "wire", "cable", "socket", "fork", "port", "combLeft"]):
        categories["Cables & Sockets"].append((idx, line_str))
    elif any(k in line_str for k in ["meterColor", "meter", "ledColor", "isFaulted", "bypassed", "isActive", "dotCenter"]):
        categories["Meters & Signal States"].append((idx, line_str))
    elif any(k in line_str for k in ["AddText", "tagColor", "PushStyleColor(ImGuiCol_Text", "textPos", "vendorText", "crashTitle"]):
        categories["Text & Typography"].append((idx, line_str))
    elif any(k in line_str for k in ["cardBg", "frameBg", "knobBg", "envY", "chassis"]):
        categories["Card Surfaces"].append((idx, line_str))
    elif any(k in line_str for k in ["borderCol", "frameBorder", "border"]):
        categories["Card Borders & Glows"].append((idx, line_str))
    elif any(k in line_str for k in ["knob", "dial", "needle"]):
        categories["Knobs & Controls"].append((idx, line_str))
    elif any(k in line_str for k in ["RectFilled", "AddRect"]):
        categories["Chassis & Containers"].append((idx, line_str))
    else:
        categories["Other"].append((idx, line_str))

for cat, items in categories.items():
    print(f"=== {cat}: {len(items)} items ===")
    for idx, l in items[:5]:
        print(f"  [{idx}] {l}")

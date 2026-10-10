import re

with open("f:/Projects/Praccy/src/ui/rack_view.cpp", "r", encoding="utf-8") as f:
    lines = f.readlines()

pattern = re.compile(r'IM_COL32\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+|\w+)\s*\)')

all_items = []
for idx, line in enumerate(lines, 1):
    matches = pattern.finditer(line)
    for m in matches:
        all_items.append((idx, m.group(0), line.strip()))

with open("f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m3_1/all_170_items.txt", "w", encoding="utf-8") as out:
    for idx, col, full_line in all_items:
        out.write(f"{idx:4d} | {col:32s} | {full_line}\n")

print(f"Dumped {len(all_items)} items to all_170_items.txt")

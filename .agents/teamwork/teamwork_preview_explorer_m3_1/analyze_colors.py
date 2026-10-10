import re

with open("f:/Projects/Praccy/src/ui/rack_view.cpp", "r", encoding="utf-8") as f:
    lines = f.readlines()

occurrences = []
pattern = re.compile(r'IM_COL32\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+|\w+)\s*\)')

for idx, line in enumerate(lines, 1):
    matches = pattern.findall(line)
    if matches:
        for m in matches:
            occurrences.append((idx, line.strip(), m))

print(f"Total IM_COL32 occurrences in rack_view.cpp: {len(occurrences)}")

# Group occurrences by functionality/pattern
by_color = {}
for idx, line, m in occurrences:
    rgba = f"IM_COL32({m[0]}, {m[1]}, {m[2]}, {m[3]})"
    if rgba not in by_color:
        by_color[rgba] = []
    by_color[rgba].append((idx, line))

print(f"Unique RGBA definitions: {len(by_color)}")
for rgba, locs in sorted(by_color.items(), key=lambda x: -len(x[1])):
    print(f"\n{rgba} - Count: {len(locs)}")
    for l_idx, l_str in locs[:3]: # show up to 3 samples
        print(f"   Line {l_idx}: {l_str}")

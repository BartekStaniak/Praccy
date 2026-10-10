import re

with open("f:/Projects/Praccy/src/ui/rack_view.cpp", "r", encoding="utf-8") as f:
    lines = f.readlines()

# Find function definitions
func_pattern = re.compile(r'^(?:void|bool|ImU32|float|int|static void|static bool|static ImU32)\s+([A-Za-z0-9_:]+)\s*\(')
im_pattern = re.compile(r'IM_COL32\s*\(')

current_func = "global"
func_counts = {}

for idx, line in enumerate(lines, 1):
    m_func = func_pattern.match(line)
    if m_func:
        current_func = m_func.group(1)
        if current_func not in func_counts:
            func_counts[current_func] = 0
    if im_pattern.search(line):
        func_counts[current_func] = func_counts.get(current_func, 0) + 1

for func, count in sorted(func_counts.items(), key=lambda x: -x[1]):
    if count > 0:
        print(f"{func:40s}: {count:3d} lines")

#!/usr/bin/env python3
"""
scripts/check_hardcoded_colors.py
Praccy Static Hardcoded Color Scanner (Milestone 3 / Feature 15).

Scans all C/C++ source files under src/ui/ (and any subdirectories)
to ensure zero undeclared raw IM_COL32(...) literals exist outside
of token definition files (design_tokens.h / design_tokens.cpp).

Exit code:
  0: Clean, zero violations found.
  1: Violations detected, prints filename, line number, and offending snippet.
  2: Command-line or path error.
"""

import sys
import os
import re
import argparse
from pathlib import Path

ALLOWED_FILES = {
    "design_tokens.h",
    "design_tokens.hpp",
    "design_tokens.cpp"
}

VALID_EXTENSIONS = {".h", ".hpp", ".c", ".cpp", ".cc"}
PATTERN_IM_COL32 = re.compile(r'\bIM_COL32\s*\(')
PATTERN_IM_VEC4 = re.compile(r'\bImVec4\s*\(')

def strip_line_comments(line: str) -> str:
    comment_idx = line.find('//')
    if comment_idx != -1:
        return line[:comment_idx]
    return line

def scan_file(file_path: Path):
    violations = []
    in_block_comment = False

    try:
        with open(file_path, "r", encoding="utf-8", errors="replace") as f:
            for line_no, raw_line in enumerate(f, 1):
                line = raw_line.strip()

                if in_block_comment:
                    end_idx = line.find("*/")
                    if end_idx != -1:
                        in_block_comment = False
                        line = line[end_idx + 2:].strip()
                    else:
                        continue

                start_idx = line.find("/*")
                if start_idx != -1:
                    end_idx = line.find("*/", start_idx + 2)
                    if end_idx != -1:
                        line = line[:start_idx] + line[end_idx + 2:]
                    else:
                        in_block_comment = True
                        line = line[:start_idx]

                code_line = strip_line_comments(line).strip()
                if not code_line:
                    continue

                if PATTERN_IM_COL32.search(code_line) or PATTERN_IM_VEC4.search(code_line):
                    violations.append((line_no, raw_line.rstrip()))
    except Exception as e:
        print(f"Error reading {file_path}: {e}", file=sys.stderr)

    return violations

def main():
    parser = argparse.ArgumentParser(
        description="Verify zero undeclared IM_COL32 literals in Praccy UI codebase."
    )
    parser.add_argument(
        "--path",
        default="src/ui",
        help="Path to UI directory to scan (default: src/ui relative to cwd)"
    )
    args = parser.parse_args()

    root_dir = Path(args.path).resolve()
    if not root_dir.exists():
        print(f"Error: Target path does not exist: {root_dir}", file=sys.stderr)
        sys.exit(2)

    total_violations = 0
    files_with_violations = 0

    for current_root, _, files in os.walk(root_dir):
        for f in files:
            file_path = Path(current_root) / f
            if file_path.suffix.lower() not in VALID_EXTENSIONS:
                continue

            if file_path.name in ALLOWED_FILES:
                continue

            violations = scan_file(file_path)
            if violations:
                files_with_violations += 1
                total_violations += len(violations)
                rel_path = os.path.relpath(file_path, Path.cwd())
                for line_no, snippet in violations:
                    print(f"[COLOR-LINT] {rel_path}:{line_no}: {snippet.strip()}")

    print("-" * 72)
    if total_violations == 0:
        print(f"SUCCESS: Clean! 0 hardcoded IM_COL32 / ImVec4 literals found in {root_dir}.")
        print("Design tokens fully enforced across all UI translation units.")
        sys.exit(0)
    else:
        print(f"FAILED: Found {total_violations} raw IM_COL32 / ImVec4 call(s) across {files_with_violations} file(s).")
        print("All UI colors must be defined via semantic tokens in src/ui/design_tokens.h.")
        sys.exit(1)

if __name__ == "__main__":
    main()

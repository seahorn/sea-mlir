#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Check publishable source files for machine-local and unpublished provenance.

Run from any directory: python3 utils/privacy-check.py [--root DIR] [--terms FILE].
Optional project-specific regexes come from FILE or ROOT/.privacy-terms.
Use one regex per line; blank lines and lines starting with # are ignored.
Keep that file untracked. Missing default files are allowed.
Scans tracked files (including staged additions), using working-tree contents.
Outside a Git checkout, scans the directory tree without Git metadata.
Build outputs, connected corpora and Git metadata are not publication inputs.
This is a guard against known classes of accidental disclosure, not a secret
scanner. New findings should add a precise pattern and a regression case.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

# Detection vocabulary is intentionally explicit. This declaration block is
# excluded from generic rules, but local terms are checked everywhere.
# BEGIN DETECTION VOCABULARY
PATTERNS = {
    "local-path": r"(?<![\w%/])(?:~?/(?:ag|tmp|home|Users)/)[^\s\"'<>`]*",
    "private-notes": r"\b(?:journal/|loose-ends/|handoff[ -]notes?|notebook)\b",
    "review-provenance": r"\b(?:review\s+(?:round\s+)?(?:R)?\d+|round\s+\d+)\b",
    "review-label": r"(?-i:\bR\d{1,3}(?:-\d+)?\b)",
    "plan-label": r"\b(?:plan\s+(?:[A-Z]\.?\d+(?:\.\d+)*|\d+(?:\.\d+)*|step\b)|phase\s+\d+)\b",
    "decision-date": r"\b20\d{2}-[01]\d-[0-3]\d\b",
}
# END DETECTION VOCABULARY
RULES = {name: re.compile(pattern, re.I) for name, pattern in PATTERNS.items()}


def load_terms(path):
    """Read optional local regexes; invalid patterns fail instead of being skipped."""
    return [
        re.compile(line.strip(), re.I)
        for line in path.read_text().splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]


def findings(path, text, local_terms=()):
    """Return (line number, category, matched text), with narrow public exceptions."""
    in_ai_section = False
    in_vocabulary = False
    for number, line in enumerate(text.splitlines(), 1):
        if path == "utils/privacy-check.py":
            if line == "# BEGIN DETECTION VOCABULARY":
                in_vocabulary = True
                continue
            if line == "# END DETECTION VOCABULARY":
                in_vocabulary = False
                continue
        if path == "README.md" and line.startswith("## "):
            in_ai_section = line == "## 🐬 Built with AI agents"
        # The contributor instruction filename is public, not an attribution.
        scan = line.replace("CLAUDE.md", "CONTRIBUTOR_FILE")
        for category, regex in list(RULES.items()) + [
            ("local-term", r) for r in local_terms
        ]:
            if in_vocabulary and category != "local-term":
                continue
            # The LLVM pin's commit date (utils/llvm-drift.py reads it).
            if category == "decision-date" and path == "LLVM_REVISION":
                continue
            if category == "review-label" and not (
                path.endswith(".md") or line.lstrip().startswith(("//", "#", "/*", "*"))
            ):
                continue
            # Local names may appear in public credits and scholarly blockquotes.
            # Generic path/provenance rules still apply in those locations.
            if category == "local-term" and (
                path == "AUTHORS"
                or (path == "README.md" and in_ai_section)
                or (path == "docs/reference/seam.md" and line.startswith("> "))
            ):
                continue
            for match in regex.finditer(scan):
                yield number, category, match.group()


def source_files(root):
    result = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "--is-inside-work-tree"],
        capture_output=True,
        text=True,
    )
    if result.returncode:
        if "not a git repository" not in result.stderr:
            raise RuntimeError(result.stderr.strip())
        return sorted(
            str(path.relative_to(root))
            for path in root.rglob("*")
            if ".git" not in path.relative_to(root).parts
            and (path.is_file() or path.is_symlink())
        )
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "--cached", "-z"],
        check=True,
        capture_output=True,
    )
    return sorted(set(result.stdout.decode().split("\0")) - {""})


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parent.parent
    )
    parser.add_argument(
        "--terms", type=Path, help="local regex file (default: ROOT/.privacy-terms)"
    )
    args = parser.parse_args()
    terms_path = args.terms or args.root / ".privacy-terms"
    try:
        local_terms = (
            load_terms(terms_path) if args.terms or terms_path.exists() else []
        )
    except (OSError, re.error) as error:
        parser.error(f"cannot load terms: {error}")
    count = 0
    for name in source_files(args.root):
        path = args.root / name
        if not path.is_file() and not path.is_symlink():
            continue
        # Check the link text rather than following links outside the tree.
        if path.is_symlink():
            text = str(path.readlink())
        else:
            data = path.read_bytes()
            if b"\0" in data:
                continue
            text = data.decode("utf-8", errors="replace")
        for number, category, match in findings(name, text, local_terms):
            print(f"{name}:{number}: {category}: {match}")
            count += 1
    print(f"privacy: {count} finding(s)")
    return int(count != 0)


if __name__ == "__main__":
    sys.exit(main())

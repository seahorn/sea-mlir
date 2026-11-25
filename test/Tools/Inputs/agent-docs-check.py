#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Every docs/... path an --agent guide cites exists under ROOT.

usage: agent-docs-check.py ROOT GUIDE...

Paths are `docs/` followed by path characters; an anchor (#...) and
trailing punctuation are stripped. A .md path must be a file. The DOCS / DOCS-WEB lines (the docs
root, include/sea/Support/AgentGuide.h) are skipped: their docs/... is a
pattern, not a page. Prints `agent-docs.missing: <guide>:<line>: <path>`
per dangling path and `agent-docs.checked: <n> paths` at the end; exit 1
if any is missing.
"""

import os
import re
import sys

PATH = re.compile(r"(?<![\w./-])docs/[\w./#-]*")


def main():
    root, guides = sys.argv[1], sys.argv[2:]
    checked = missing = 0
    for g in guides:
        with open(g, encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                if line.startswith(("DOCS:", "DOCS-WEB:")):
                    continue
                for m in PATH.finditer(line):
                    p = m.group(0).split("#", 1)[0].rstrip(".,;:)")
                    checked += 1
                    full = os.path.join(root, p)
                    ok = (
                        os.path.isfile(full)
                        if p.endswith(".md")
                        else os.path.exists(full)
                    )
                    if not ok:
                        missing += 1
                        print(f"agent-docs.missing: {g}:{n}: {p}")
    print(f"agent-docs.checked: {checked} paths")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())

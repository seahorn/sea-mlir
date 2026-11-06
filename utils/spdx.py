#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""spdx: check or add the SPDX header of every code and test file.

    utils/spdx.py --check [--root DIR] [FILE...]
    utils/spdx.py --fix   [--root DIR] [FILE...]

The header is two comment lines in the file's comment syntax:

    // SPDX-FileCopyrightText: 2026 The sea-mlir Authors
    // SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

Code files carry it at the top (after a `#!` line); test inputs under
test/ (.mlir, .test, .c, .ll, .cfg) carry it at the end, so that the line
numbers their checks name do not move.

Files: `git ls-files` (tracked) plus untracked, not ignored files that a
rule of RULES matches. Files that match an annotation of REUSE.toml are
licensed there and skipped; so is LICENSES/. A tracked file that neither a
rule nor REUSE.toml covers is reported: add a rule here or a path to
REUSE.toml.

--check prints `<file>: <problem>` per bad file and exits 1 if there is
one; --fix adds the header or replaces a wrong one (any SPDX tag line in
the file's comment syntax is removed first) and prints the files changed.
"""

import argparse
import os
import re
import subprocess
import sys

# Built from parts, so that REUSE tools do not read these strings as the
# tags of this file.
TAG_COPYRIGHT = "SPDX-" + "FileCopyrightText"
TAG_LICENSE = "SPDX-" + "License-Identifier"
COPYRIGHT = "2026 The sea-mlir Authors"
LICENSE = "Apache-2.0 WITH LLVM-exception"

# (glob, comment, placement), first match wins. comment: a line prefix,
# ("/*", "*/") for a block comment, or "test" (the comment of a lit .test
# file: `//` if it uses `//` lines, else `#`). Globs: `*` stays within a
# path component, `**` crosses them.
TEST_END = ("test/**", "unittests/**")
RULES = [
    # test inputs: at the end
    *[(f"{d}.{e}", "//", "end") for d in TEST_END for e in ("mlir", "c")],
    *[(f"{d}.ll", ";", "end") for d in TEST_END],
    *[(f"{d}.test", "test", "end") for d in TEST_END],
    *[(f"{d}.cfg", "#", "end") for d in TEST_END],
    # code: at the top
    ("**.cpp", "//", "top"),
    ("**.c", "//", "top"),
    ("**.h", "//", "top"),
    ("**.hh", "//", "top"),
    ("**.def", "//", "top"),
    ("**.td", "//", "top"),
    ("**.mlir", "//", "top"),
    ("**.js", "//", "top"),
    ("**.ll", ";", "top"),
    ("**.css", ("/*", "*/"), "top"),
    ("**.py", "#", "top"),
    ("**.py.in", "#", "top"),
    ("**.sh", "#", "top"),
    ("**.cmake", "#", "top"),
    ("**CMakeLists.txt", "#", "top"),
    ("**requirements.txt", "#", "top"),
    ("**.cfg", "#", "top"),
    ("**.toml", "#", "top"),
    ("justfile", "#", "top"),
    ("utils/hooks/*", "#", "top"),
    ("**Dockerfile", "#", "top"),
    ("**.dockerignore", "#", "top"),
    ("**.gitignore", "#", "top"),
    ("**.clang-format", "#", "top"),
]


def glob_re(pat):
    """A REUSE path glob as a regex: `**` any, `*` / `?` within a component."""
    out, i = [], 0
    while i < len(pat):
        if pat.startswith("**", i):
            out.append(".*")
            i += 2
        elif pat[i] == "*":
            out.append("[^/]*")
            i += 1
        elif pat[i] == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(pat[i]))
            i += 1
    return re.compile("".join(out) + r"\Z")


RULE_RES = [(glob_re(g), c, p) for g, c, p in RULES]


def reuse_globs(root):
    path = os.path.join(root, "REUSE.toml")
    if not os.path.exists(path):
        return []
    try:
        import tomllib
    except ImportError:
        sys.exit("spdx: reading REUSE.toml needs Python 3.11+ (tomllib)")
    with open(path, "rb") as f:
        data = tomllib.load(f)
    globs = []
    for ann in data.get("annotations", []):
        p = ann.get("path", [])
        globs += [p] if isinstance(p, str) else p
    return [glob_re(g) for g in globs]


def rule_for(path):
    for rx, comment, place in RULE_RES:
        if rx.match(path):
            return comment, place
    return None


def git_files(root, others):
    args = ["git", "-C", root, "ls-files", "-z"]
    if others:
        args += ["--others", "--exclude-standard"]
    out = subprocess.run(args, check=True, capture_output=True).stdout
    return [p for p in out.decode().split("\0") if p]


def header_lines(comment):
    if isinstance(comment, tuple):
        o, c = comment
        return [
            f"{o} {TAG_COPYRIGHT}: {COPYRIGHT} {c}",
            f"{o} {TAG_LICENSE}: {LICENSE} {c}",
        ]
    return [
        f"{comment} {TAG_COPYRIGHT}: {COPYRIGHT}",
        f"{comment} {TAG_LICENSE}: {LICENSE}",
    ]


def test_comment(lines):
    return "//" if any(l.startswith("//") for l in lines) else "#"


def tag_line_re(comment):
    o = comment[0] if isinstance(comment, tuple) else comment
    return re.compile(
        r"\s*" + re.escape(o) + r"\s*SPDX-(FileCopyrightText|License-Identifier):"
    )


def analyze(text, comment, place):
    """(problem or None, lines of the file with the header fixed)."""
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()  # final newline
    if comment == "test":
        comment = test_comment(lines)
    hdr = header_lines(comment)
    tag = tag_line_re(comment)
    start = 1 if lines and lines[0].startswith("#!") else 0

    # where the header is now
    if place == "top":
        ok = lines[start : start + 2] == hdr
    else:
        ok = lines[-2:] == hdr
    ntags = sum(1 for l in lines if tag.match(l))
    if ok and ntags == 2:
        return None, None
    problem = "wrong header" if ntags else "missing header"
    if ntags and not ok:
        problem = "wrong or misplaced header"
    elif ok:
        problem = "extra SPDX lines"

    # fix: drop every tag line (and the blank line that separated it), add
    body = []
    i = 0
    while i < len(lines):
        if tag.match(lines[i]):
            i += 1
            while i < len(lines) and lines[i] == "" and (not body or body[-1] == ""):
                i += 1
            continue
        body.append(lines[i])
        i += 1
    while body and body[-1] == "":
        body.pop()
    if place == "top":
        rest = body[start:]
        sep = [""] if rest and rest[0] != "" else []
        fixed = body[:start] + hdr + sep + rest
    else:
        fixed = body + ([""] if body else []) + hdr
    return problem, fixed


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--fix", action="store_true")
    ap.add_argument(
        "--root",
        default=os.path.join(os.path.dirname(__file__), ".."),
        help="the source tree (default: this script's ..)",
    )
    ap.add_argument("files", nargs="*", help="only these (paths relative to the root)")
    a = ap.parse_args()
    root = os.path.abspath(a.root)

    covered = reuse_globs(root)
    if a.files:
        files = [(os.path.relpath(os.path.abspath(f), root), True) for f in a.files]
    else:
        files = [(p, True) for p in git_files(root, False)]
        files += [(p, False) for p in git_files(root, True)]

    bad = 0
    for path, tracked in sorted(set(files)):
        full = os.path.join(root, path)
        if not os.path.isfile(full) or os.path.islink(full):
            continue
        if path.startswith("LICENSES/") or any(rx.match(path) for rx in covered):
            continue
        rule = rule_for(path)
        if rule is None:
            if tracked:
                print(
                    f"{path}: no SPDX rule (add one to utils/spdx.py or the path to REUSE.toml)"
                )
                bad += 1
            continue
        with open(full, encoding="utf-8", newline="") as f:
            text = f.read()
        problem, fixed = analyze(text, *rule)
        if problem is None:
            continue
        if a.fix:
            with open(full, "w", encoding="utf-8", newline="") as f:
                f.write("\n".join(fixed) + "\n")
            print(f"{path}: fixed ({problem})")
        else:
            print(f"{path}: {problem}")
            bad += 1
    if a.check:
        print(f"spdx: {bad} file(s) to fix" + (" (utils/spdx.py --fix)" if bad else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""llvm-drift: how far the LLVM pin (LLVM_REVISION) is behind LLVM main.

    utils/llvm-drift.py [--llvm DIR] [--ref REF] [--fetch] [--network]
                        [--list] [--gate]
    just llvm-drift [args]

Always prints the pin, its commit date and its age in days (from
LLVM_REVISION, no network). With an llvm-project checkout (--llvm DIR or
$LLVM_PROJECT) it also counts the commits of REF (default origin/main) after
the pin: all of them, those in mlir/, and those touching the MLIR parts
sea-mlir uses (USED_PATHS); --fetch first runs `git fetch origin main` in
that checkout; --list prints the latter commits. Without a checkout,
--network asks the GitHub API for the number of commits main is ahead
(no per-path counts).

Status: ok below WARN_DAYS days, warn up to FAIL_DAYS, fail beyond
(docs/Release.md, "LLVM"). --gate exits 1 on fail (release-check).
Output lines are `key: value`.
"""

import argparse
import datetime
import json
import os
import subprocess
import sys
import urllib.request

WARN_DAYS = 45
FAIL_DAYS = 90

# the MLIR parts sea-mlir builds on: a commit touching one of them may need
# a change here when the pin moves
_PARTS = [
    "IR",
    "Pass",
    "Parser",
    "AsmParser",
    "Support",
    "Interfaces",
    "Transforms",
    "Rewrite",
    "Reducer",
    "Tools",
    "Bytecode",
    "TableGen",
]
USED_PATHS = (
    [f"mlir/include/mlir/{p}" for p in _PARTS]
    + [f"mlir/lib/{p}" for p in _PARTS]
    + ["mlir/tools/mlir-tblgen"]
    + [
        f"mlir/{d}/Dialect/{x}"
        for d in ("include/mlir", "lib")
        for x in ("ControlFlow", "Func")
    ]
)


def read_pin(path):
    """(short hash, full hash or None, date or None) of LLVM_REVISION."""
    with open(path) as f:
        lines = [l.strip() for l in f if l.strip()]
    short, full, date = lines[0], None, None
    for l in lines[1:]:
        key, _, val = l.partition(":")
        val = val.strip()
        if key == "commit":
            full = val
        elif key == "date":
            date = datetime.date.fromisoformat(val)
    return short, full, date


def git(repo, *args):
    return subprocess.run(
        ["git", "-C", repo, *args], check=True, capture_output=True, text=True
    ).stdout.strip()


def count(repo, rng, paths=()):
    args = ["rev-list", "--count", rng]
    if paths:
        args += ["--", *paths]
    return int(git(repo, *args))


def main():
    src = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument(
        "--llvm",
        default=os.environ.get("LLVM_PROJECT"),
        help="an llvm-project checkout (default $LLVM_PROJECT)",
    )
    ap.add_argument(
        "--ref",
        default="origin/main",
        help="the ref compared with the pin (default origin/main)",
    )
    ap.add_argument(
        "--fetch",
        action="store_true",
        help="git fetch origin main in the checkout first",
    )
    ap.add_argument(
        "--network",
        action="store_true",
        help="no checkout: ask the GitHub API for the count",
    )
    ap.add_argument(
        "--list",
        action="store_true",
        help="list the commits touching the MLIR parts used",
    )
    ap.add_argument(
        "--gate",
        action="store_true",
        help=f"exit 1 when the pin is older than {FAIL_DAYS} days",
    )
    ap.add_argument(
        "--today",
        type=datetime.date.fromisoformat,
        default=datetime.date.today(),
        help=argparse.SUPPRESS,
    )
    a = ap.parse_args()

    short, full, date = read_pin(os.path.join(src, "LLVM_REVISION"))
    print(f"llvm.pin: {short}")
    if full:
        print(f"llvm.pin-commit: {full}")
    status = "unknown"
    if date:
        age = (a.today - date).days
        status = "ok" if age <= WARN_DAYS else "warn" if age <= FAIL_DAYS else "fail"
        print(f"llvm.pin-date: {date.isoformat()}")
        print(f"llvm.pin-age-days: {age}")
    else:
        print("llvm.pin-date: unknown (no `date:` line in LLVM_REVISION)")

    if a.llvm:
        pin = full or short
        try:
            if a.fetch:
                git(a.llvm, "fetch", "-q", "origin", "main")
            head = git(a.llvm, "log", "-1", "--format=%h %cs", a.ref)
            rng = f"{pin}..{a.ref}"
            print(f"llvm.main: {head} ({a.ref} in {a.llvm})")
            print(f"llvm.behind: {count(a.llvm, rng)}")
            print(f"llvm.behind-mlir: {count(a.llvm, rng, ['mlir'])}")
            print(f"llvm.behind-mlir-used: {count(a.llvm, rng, USED_PATHS)}")
            if a.list:
                out = git(a.llvm, "log", "--format=%h %cs %s", rng, "--", *USED_PATHS)
                for l in out.splitlines():
                    print(f"  {l}")
        except subprocess.CalledProcessError as e:
            print(f"llvm.main: error: {e.stderr.strip() or e}", file=sys.stderr)
            return 2
    elif a.network:
        if not full:
            print(
                "llvm.main: error: --network needs `commit:` in LLVM_REVISION",
                file=sys.stderr,
            )
            return 2
        url = f"https://api.github.com/repos/llvm/llvm-project/compare/{full}...main"
        try:
            req = urllib.request.Request(
                url, headers={"Accept": "application/vnd.github+json"}
            )
            with urllib.request.urlopen(req, timeout=30) as r:
                d = json.load(r)
            print(
                f"llvm.behind: {d['ahead_by']} (GitHub API; "
                "per-path counts need --llvm)"
            )
        except Exception as e:  # network, rate limit
            print(f"llvm.main: error: {e}", file=sys.stderr)
            return 2
    else:
        print("llvm.behind: unknown (--llvm DIR, $LLVM_PROJECT or --network)")

    print(
        f"llvm.pin-status: {status} (warn > {WARN_DAYS} days, fail > {FAIL_DAYS} days)"
    )
    return 1 if a.gate and status == "fail" else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""qa-coverage: how much of each validated pass the QA corpus exercises.

The QA corpus (test/QA: small programs per pass, each validated by
utils/cover-diff.py) is a good set only if it drives the code of the
passes it validates. This script measures it with clang source coverage:

1. A build of sea-vc / sea-run / sea-opt with -fprofile-instr-generate
   -fcoverage-mapping (--build, configured as in docs/agents/gates.md,
   "QA coverage"; the script configures and builds it when it has no
   CMakeCache.txt, from --cmake-args).
2. The lit tests of test/QA run in that build (lit: --lit, else the
   build's LLVM_EXTERNAL_LIT, else lit on PATH) with LLVM_PROFILE_FILE set
   (one raw profile per process, deleted once merged) and lit --param
   sea_log=canon.fold:2 / sea_log_file=<file>, which test/lit.cfg.py puts
   into the tests' environment (every fold that fires is an event); then,
   for comparison, all of check-sea, without the log.
3. llvm-profdata merges each set; llvm-cov reports, for each pass source
   (PASSES below), its line and branch coverage under the QA corpus and
   under check-sea, the branches the QA corpus never takes one way, and,
   for each canonicalization pattern, how often it ran (its
   matchAndRewrite) and how often it fired (canon.fold events): no-fire =
   ran - fired.

Output (line-oriented):
  file: <path> qa-lines=<covered>/<total> (<pct>%) qa-branches=<c>/<t> (<pct>%)
        check-lines=... check-branches=...
  uncovered: <path>:<line>:<col> <true|false> never taken  (QA corpus)
  pattern: <name> ran=<n> fired=<n> no-fire=<n> (QA corpus)
  fold: <kind> fired=<n>   (canon.fold events of the QA corpus)
--reuse reports from the merged profiles of an earlier run, which record
their commit (git describe --dirty): refused for another commit.
Status 0 when every step ran; 1 otherwise.
"""

import argparse
import collections
import glob
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# the sources of the passes the QA corpus validates
PASSES = [
    "lib/Dialect/Sea/SeaOps.cpp",
    "lib/Transforms/DropFreshMemHavoc.cpp",
    "lib/Transforms/GuardNullMemHavoc.cpp",
    "lib/Transforms/MergeAsserts.cpp",
    "lib/Transforms/SplitAtChecks.cpp",
    "lib/Transforms/ReachTarget.cpp",
    "lib/VC/Replay.cpp",
]
# canonicalization patterns (SeaOps.cpp): their matchAndRewrite, by the
# fold kinds each reports (canon.fold)
PATTERNS = {
    "SimplifyBr": ["br-merge"],
    "FoldConstAssume": ["assume-true", "assume-false"],
    "FoldConstAssert": ["assert-true", "assert-false"],
    "ForwardStoredValue": ["load-stored"],
}
TOOLS = ["sea-vc", "sea-run", "sea-opt"]
# a file whose passes are one part of it: also reported on the lines from
# the first marker to the second (SeaOps.cpp: the canonicalization patterns)
RANGES = {
    "lib/Dialect/Sea/SeaOps.cpp": (
        "struct SimplifyBr",
        "void BrOp::getCanonicalizationPatterns",
    ),
}


def line_range(path):
    """(first, last) line of RANGES[path] in the source, else None."""
    if path not in RANGES:
        return None
    start, end = RANGES[path]
    lines = open(os.path.join(ROOT, path)).read().splitlines()
    first = next(i for i, x in enumerate(lines, 1) if start in x)
    last = next(i for i, x in enumerate(lines, 1) if end in x and i > first)
    return first, last


def in_range(f, rng):
    """Lines and branches of one file's export within rng: (covered,
    total) of each."""
    lo, hi = rng
    cov = {}
    for seg in f.get("segments", []):
        ln, _, count, has_count = seg[0], seg[1], seg[2], seg[3]
        if lo <= ln <= hi and has_count:
            cov[ln] = cov.get(ln, 0) or count
    br = [b for b in f.get("branches", []) if lo <= b[0] <= hi]
    taken = sum((b[4] > 0) + (b[5] > 0) for b in br)
    return (sum(1 for c in cov.values() if c), len(cov)), (taken, 2 * len(br))


def frac(c):
    return f"{c[0]}/{c[1]} ({100.0 * c[0] / c[1] if c[1] else 0:.1f}%)"


def run(cmd, **kw):
    print("run: " + " ".join(cmd), file=sys.stderr, flush=True)
    return subprocess.run(cmd, **kw)


def head_id():
    """The commit the profiles are of (`git describe --always --dirty`)."""
    p = subprocess.run(
        ["git", "-C", ROOT, "describe", "--always", "--dirty", "--abbrev=40"],
        capture_output=True,
        text=True,
    )
    return p.stdout.strip() or "unknown"


def lit_profile(a, tests, prof_dir, log_file):
    """Runs lit on `tests` in the coverage build; the merged profile of
    its runs (prof_dir/merged.profdata; the raw profiles are deleted).
    log_file: the canon.fold events go there (lit --param sea_log /
    sea_log_file, which test/lit.cfg.py sets in the tests' environment)."""
    data = os.path.join(prof_dir, "merged.profdata")
    stamp = os.path.join(prof_dir, "commit.txt")
    if a.reuse:
        have = open(stamp).read().strip() if os.path.exists(stamp) else "none"
        if not os.path.exists(data) or have != head_id():
            sys.exit(
                f"qa-coverage: --reuse: {prof_dir} is of {have}, HEAD is "
                f"{head_id()}: run without --reuse"
            )
        return data
    shutil.rmtree(prof_dir, ignore_errors=True)
    os.makedirs(prof_dir)
    env = dict(os.environ)
    env["LLVM_PROFILE_FILE"] = os.path.join(prof_dir, "p-%p-%m.profraw")
    cmd = [a.lit, "-q", f"-j{a.jobs}"]
    if log_file:
        if os.path.exists(log_file):
            os.unlink(log_file)
        cmd += [
            "--param",
            "sea_log=canon.fold:2",
            "--param",
            f"sea_log_file={log_file}",
        ]
    r = run(cmd + tests, env=env)
    # a failing test is reported; the coverage of the rest still counts
    if r.returncode:
        print(f"warning: lit exit {r.returncode} on {' '.join(tests)}", file=sys.stderr)
    raws = glob.glob(os.path.join(prof_dir, "*.profraw"))
    if not raws:
        sys.exit(f"qa-coverage: no profiles in {prof_dir}")
    lst = os.path.join(prof_dir, "list.txt")
    with open(lst, "w") as f:
        f.write("\n".join(raws) + "\n")
    if run([a.profdata, "merge", "-sparse", "-f", lst, "-o", data]).returncode:
        sys.exit("qa-coverage: llvm-profdata failed")
    # one raw profile per process: tens of GB for check-sea
    for x in raws:
        os.unlink(x)
    os.unlink(lst)
    with open(stamp, "w") as f:
        f.write(head_id() + "\n")
    return data


def default_lit(build):
    """The lit CMake found (LLVM_EXTERNAL_LIT), else lit / llvm-lit on PATH."""
    cache = os.path.join(build, "CMakeCache.txt")
    if os.path.exists(cache):
        for line in open(cache):
            if line.startswith("LLVM_EXTERNAL_LIT:"):
                return line.split("=", 1)[1].strip()
    return shutil.which("lit") or shutil.which("llvm-lit") or "lit"


def export(a, data):
    """llvm-cov export JSON of the pass sources."""
    objs = [os.path.join(a.build, "bin", t) for t in TOOLS]
    cmd = [a.cov, "export", "-instr-profile", data, objs[0]]
    for o in objs[1:]:
        cmd += ["-object", o]
    cmd += [os.path.join(ROOT, p) for p in PASSES]
    p = run(cmd, capture_output=True, text=True)
    if p.returncode:
        sys.exit("qa-coverage: llvm-cov export failed: " + p.stderr[:300])
    return json.loads(p.stdout)


def by_file(js):
    out = {}
    for d in js["data"]:
        for f in d["files"]:
            out[os.path.relpath(f["filename"], ROOT)] = f
    return out


def pct(s):
    return f"{s['covered']}/{s['count']} ({s['percent']:.1f}%)"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--build", default=os.path.join(ROOT, "build-cov"))
    ap.add_argument(
        "--lit", help="lit (default: LLVM_EXTERNAL_LIT of the build, or lit on PATH)"
    )
    ap.add_argument(
        "--profdata", default=shutil.which("llvm-profdata-18") or "llvm-profdata"
    )
    ap.add_argument("--cov", default=shutil.which("llvm-cov-18") or "llvm-cov")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--out", help="work directory (default <build>/qa-coverage)")
    ap.add_argument("--no-check-sea", action="store_true", help="the QA corpus only")
    ap.add_argument(
        "--reuse",
        action="store_true",
        help="report from the profiles of an earlier run of this commit (no lit;"
        " refused when they are of another commit or a dirty tree)",
    )
    ap.add_argument(
        "cmake_args",
        nargs="*",
        help="after --: CMake arguments of a build to configure (no CMakeCache.txt)",
    )
    a = ap.parse_args()
    # lit runs the tests elsewhere: every path absolute
    a.build = os.path.abspath(a.build)
    out = os.path.abspath(a.out or os.path.join(a.build, "qa-coverage"))
    a.lit = a.lit or default_lit(a.build)
    os.makedirs(out, exist_ok=True)
    if not os.path.exists(os.path.join(a.build, "CMakeCache.txt")):
        flags = "-fprofile-instr-generate -fcoverage-mapping"
        cfg = [
            "cmake", "-S", ROOT, "-B", a.build, "-GNinja",
            f"-DCMAKE_C_FLAGS={flags}", f"-DCMAKE_CXX_FLAGS={flags}",
            "-DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate",
        ] + a.cmake_args  # fmt: skip
        if run(cfg).returncode:
            sys.exit("qa-coverage: cmake failed")
    if run(["ninja", "-C", a.build, "-j8"] + TOOLS).returncode:
        sys.exit("qa-coverage: build failed")
    test_dir = os.path.join(a.build, "test")
    log = os.path.join(out, "canon-fold.log")
    qa = lit_profile(a, [os.path.join(test_dir, "QA")], os.path.join(out, "qa"), log)
    files_qa = by_file(export(a, qa))
    files_all = {}
    if not a.no_check_sea:
        if run(["ninja", "-C", a.build, "-j8"]).returncode:
            sys.exit("qa-coverage: build failed")
        allp = lit_profile(a, [test_dir], os.path.join(out, "check-sea"), None)
        files_all = by_file(export(a, allp))
    for p in PASSES:
        f = files_qa.get(p)
        if not f:
            print(f"file: {p} qa=none")
            continue
        s = f["summary"]
        line = f"file: {p} qa-lines={pct(s['lines'])} qa-branches={pct(s['branches'])}"
        g = files_all.get(p)
        if g:
            t = g["summary"]
            line += (
                f" check-lines={pct(t['lines'])} check-branches={pct(t['branches'])}"
            )
        print(line)
        rng = line_range(p)
        if rng:
            ql, qb = in_range(f, rng)
            line = (
                f"range: {p}:{rng[0]}-{rng[1]} qa-lines={frac(ql)} "
                f"qa-branches={frac(qb)}"
            )
            if g:
                cl, cb = in_range(g, rng)
                line += f" check-lines={frac(cl)} check-branches={frac(cb)}"
            print(line)
    for p in PASSES:
        f = files_qa.get(p)
        if not f:
            continue
        g = files_all.get(p)
        # [line, col, end line, end col, true count, false count, file id,
        #  expanded file id, kind]
        seen = set()
        for b in f.get("branches", []):
            ln, col, tc, fc = b[0], b[1], b[4], b[5]
            for way, c in (("true", tc), ("false", fc)):
                if c == 0 and (ln, col, way) not in seen:
                    seen.add((ln, col, way))
                    also = ""
                    if g:
                        hit = any(
                            x[0] == ln
                            and x[1] == col
                            and (x[4] if way == "true" else x[5])
                            for x in g.get("branches", [])
                        )
                        also = " (check-sea: taken)" if hit else " (check-sea: never)"
                    print(f"uncovered: {p}:{ln}:{col} {way} never taken{also}")
    # the patterns: matchAndRewrite executions (function coverage)
    fired = collections.Counter()
    if os.path.exists(log):
        for line in open(log):
            # text events, or jsonl ones (a run with --format=jsonl)
            if line.startswith("{"):
                e = json.loads(line)
                if e.get("log") == "canon.fold":
                    fired[e.get("msg", "")] += 1
                continue
            m = re.match(r"log: canon\.fold (\S+)", line)
            if m:
                fired[m.group(1)] += 1
    js = export(a, qa)
    ran = collections.Counter()
    for d in js["data"]:
        for fn in d.get("functions", []):
            for name in PATTERNS:
                # mangled: ...<len><name>15matchAndRewrite...
                if (
                    f"{len(name)}{name}" in fn["name"]
                    and "matchAndRewrite" in fn["name"]
                ):
                    ran[name] = max(ran[name], fn["count"])
    for name, kinds in PATTERNS.items():
        f = sum(fired[k] for k in kinds)
        print(f"pattern: {name} ran={ran[name]} fired={f} no-fire={ran[name] - f}")
    for k in sorted(fired):
        print(f"fold: {k} fired={fired[k]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

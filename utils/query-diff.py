#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""query-diff: compare the queries two sea-vc binaries make, job by job.

The gate of a representation change (docs/design/PtrIndex.md 0a,
docs/design/PtrSort.md): a change that should not change the query is
checked to make the same one, or one that differs only in argument order,
or one that z3 proves equivalent. Verdict identity alone hides
differences; counts alone hide them too (the step (a) lesson).

For every .mlir of a corpus directory (or the files given), both binaries
run with the same arguments plus --smt2. Each job is classified:
  identical   byte-identical queries
  order       equal after sorting the arguments of commutative operators
              (bvadd bvmul and or =) and the top-level forms
  equiv       z3 proves (old query) <=> (new query) (unsat of distinct)
  DIFF        none of the above (z3 sat, unknown or timeout)
  error       a binary failed or printed no query
The A-INB report (seam.inb: accesses / conditions, on stderr with
--seam-stats) is compared too, when both print it.
"""

import argparse
import concurrent.futures as cf
import glob
import os
import re
import subprocess
import sys
import tempfile

AGENT = """\
query-diff: compare the SMT queries of two sea-vc binaries over a corpus.
WHY: the gate of a change that must not change queries (or only their
  argument order): classify every job identical / order / equiv / DIFF.
USAGE:
  utils/query-diff.py --old <sea-vc> --new <sea-vc> <corpus-dir | f.mlir...>
      [--args="--lower --widen-memory=8 --seam"] [--par 12]
      [--z3 <z3>] [--timeout 300] [--keep <dir>]
OUTPUT (stdout, one line per job, then totals):
  <job> <identical|order|equiv|DIFF|error> [inb old=<acc>/<cond> new=<acc>/<cond>]
  total: identical=<n> order=<n> equiv=<n> diff=<n> error=<n>
  inb: accesses <old> -> <new> conditions <old> -> <new>
STATUS: 0 if no DIFF and no error, 1 otherwise.
--keep <dir> keeps the two queries of every DIFF / error job there.
"""

COMM = {"bvadd", "bvmul", "and", "or", "="}


def tokens(s):
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c in "()":
            yield c
            i += 1
        elif c.isspace():
            i += 1
        elif c == "|":
            j = s.index("|", i + 1)
            yield s[i : j + 1]
            i = j + 1
        else:
            j = i
            while j < n and not s[j].isspace() and s[j] not in "()":
                j += 1
            yield s[i:j]
            i = j


def parse(ts):
    stack = [[]]
    for t in ts:
        if t == "(":
            stack.append([])
        elif t == ")":
            x = stack.pop()
            stack[-1].append(x)
        else:
            stack[-1].append(t)
    return stack[0]


def ser(x):
    return x if isinstance(x, str) else "(" + " ".join(ser(k) for k in x) + ")"


def canon(x):
    if isinstance(x, str):
        return x
    kids = [canon(k) for k in x]
    if kids and kids[0] in COMM:
        kids = [kids[0]] + sorted(kids[1:])
    return "(" + " ".join(kids) + ")"


def normalized(text):
    return "\n".join(sorted(canon(x) for x in parse(tokens(text))))


def equiv_script(old, new):
    """An SMT-LIB script, unsat iff the two queries are equivalent."""
    decls, asserts = {}, {"old": [], "new": []}
    for tag, text in (("old", old), ("new", new)):
        for f in parse(tokens(text)):
            if (
                isinstance(f, list)
                and f
                and f[0] in ("declare-fun", "declare-const", "define-fun")
            ):
                decls[f[1]] = ser(f)
            elif isinstance(f, list) and f and f[0] == "assert":
                asserts[tag].append(ser(f[1]))
    # no set-logic: the queries use const arrays and bvumul_noovfl, which
    # QF_AUFBV rejects (an error was read as DIFF)
    out = list(decls.values())
    for tag in ("old", "new"):
        out.append(
            f"(define-fun {tag.upper()}__q () Bool (and true {' '.join(asserts[tag])}))"
        )
    out += ["(assert (distinct OLD__q NEW__q))", "(check-sat)"]
    return "\n".join(out) + "\n"


INB = re.compile(r"seam\.inb: \S+ accesses=(\d+) conditions=(\d+)")


def run(binary, f, args, timeout):
    try:
        p = subprocess.run(
            [binary, f] + args + ["--smt2"],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return None, None
    m = INB.search(p.stderr)
    inb = (int(m.group(1)), int(m.group(2))) if m else None
    return (p.stdout if p.returncode == 0 and p.stdout else None), inb


def one(f, a):
    job = os.path.basename(f)[:-5]
    old, inb_o = run(a.old, f, a.args, a.timeout)
    new, inb_n = run(a.new, f, a.args, a.timeout)
    if old is None or new is None:
        cls = "error"
    elif old == new:
        cls = "identical"
    elif normalized(old) == normalized(new):
        cls = "order"
    else:
        with tempfile.NamedTemporaryFile("w", suffix=".smt2", delete=False) as t:
            t.write(equiv_script(old, new))
        try:
            z = subprocess.run(
                [a.z3, t.name], capture_output=True, text=True, timeout=a.timeout
            )
            cls = "equiv" if z.stdout.strip().startswith("unsat") else "DIFF"
        except subprocess.TimeoutExpired:
            cls = "DIFF"
        os.unlink(t.name)
    if a.keep and cls in ("DIFF", "error"):
        os.makedirs(a.keep, exist_ok=True)
        for tag, text in (("old", old), ("new", new)):
            if text:
                open(os.path.join(a.keep, f"{job}.{tag}.smt2"), "w").write(text)
    return job, cls, inb_o, inb_n


def main():
    if "--agent" in sys.argv:
        print(AGENT, end="")
        return 0
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--old", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("--args", default="--lower --widen-memory=8 --seam")
    ap.add_argument("--par", type=int, default=12)
    ap.add_argument("--z3", default=os.environ.get("SEA_Z3", "z3"))
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--keep")
    a = ap.parse_args()
    a.args = a.args.split()
    # the seam.inb counts (sea-vc refuses --seam-stats without a rewriter)
    if any(x == "--seam" or x.startswith("--simplify") for x in a.args):
        a.args.append("--seam-stats")
    files = []
    for i in a.inputs:
        files += (
            sorted(glob.glob(os.path.join(i, "*.mlir"))) if os.path.isdir(i) else [i]
        )
    counts = {k: 0 for k in ("identical", "order", "equiv", "DIFF", "error")}
    inb = [0, 0, 0, 0]
    with cf.ThreadPoolExecutor(a.par) as ex:
        for job, cls, io, iN in ex.map(lambda f: one(f, a), files):
            counts[cls] += 1
            line = f"{job} {cls}"
            if io and iN:
                line += f" inb old={io[0]}/{io[1]} new={iN[0]}/{iN[1]}"
                inb = [inb[0] + io[0], inb[1] + iN[0], inb[2] + io[1], inb[3] + iN[1]]
            print(line, flush=True)
    print("total: " + " ".join(f"{k.lower()}={v}" for k, v in counts.items()))
    print(f"inb: accesses {inb[0]} -> {inb[1]} conditions {inb[2]} -> {inb[3]}")
    return 0 if counts["DIFF"] == 0 and counts["error"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Reference equivalence of two encodings of the same program's VC.

Not an oracle: a reference check for the memory-operation encodings (plain
vs --seam today; the store-chain expansion vs the term encoding of
docs/design/SeaM-memops.md later). Run with --agent for a terse guide.

For each program of the .mlir file, dump the query (sea-vc --smt2) with two
option sets A and B and ask z3 whether they are equivalent over the
nondeterministic inputs.

WHAT IT ESTABLISHES (exactly). Let FA, FB be the conjunctions of the asserts
of the two queries (each is "side conditions and not property"; a sat model
is a counterexample). The constants of the queries split into
  shared:  named nd.* (nondet values, initial memories, memhavoc arrays: the
           inputs of the program, which both encodings name alike), declared
           in at least one query, with the same sort where in both. A nd.*
           that one query dropped (e.g. SeaM simplified a memory away) is
           still shared: the other formula just does not mention it, so
           it is an unconstrained input there;
  private: everything else (block Booleans, obj.<name> constants, ...);
           renamed apart per side, so equal names cannot alias.
Write S for the shared and PA, PB for the private constants. It checks, with
z3, both
  (1)  FA(S,PA)  and  not exists PB. FB(S,PB)      is unsat
  (2)  FB(S,PB)  and  not exists PA. FA(S,PA)      is unsat
(the "not exists" is a universal quantifier over the private constants,
asserted inside z3). Both unsat means: for EVERY assignment to the shared
constants, query A has a satisfying assignment of its private constants iff
query B has one. In words: the two encodings admit the same counterexample
inputs (a query that is unsat has none, and then both are unsat). That
covers the side conditions (assume, nondet bounds, expansion bounds) and the
property together.
It does NOT establish: that private constants agree, or that the two
encodings compute the same memory term for a given input (only that the
verdict on every input is the same). A nd.* with different sorts in the two
queries is treated as private on each side (reported as "sort-clash").
Result per program: equivalent | differ (a model of the shared constants,
which one of A/B accepts and the other cannot) | unknown (z3 gave up) |
error. Plain-vs-seam equivalence holds under A-INB (accesses stay in their
objects), which --seam assumes; a program that leaves its objects may
differ legitimately.
Exit status: 0 all equivalent, 1 some differ, 2 unknown or error.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

DEFAULT_Z3 = shutil.which("z3") or "z3"

AGENT = """memops-equiv.py (agent guide, terse)
WHAT: checks that two sea-vc option sets give equivalent queries on a program,
as formulas over the shared nondet constants (nd.*): for every assignment to
them, A has a satisfying assignment of its private constants iff B has.
Uses z3 (explicit binary; the PATH z3 may be too old). Exact guarantee: see
the docstring (python3 utils/memops-equiv.py -h).
USE:
  utils/memops-equiv.py prog.mlir                       # plain vs --seam, --lower
  utils/memops-equiv.py prog.mlir --a '--widen-memory=4' --b '--seam --widen-memory=4'
  utils/memops-equiv.py prog.mlir --program sat_hi --keep <directory>  # retain the .smt2 files
OPTIONS: --sea-vc PATH (default build/bin/sea-vc next to the repo), --z3 PATH,
  --common 'FLAGS' (both sides, default --lower), --a / --b 'FLAGS'
  (side A default none, B default --seam), --timeout SECONDS (z3, default 60).
OUTPUT: one line per program: equivalent | differ | unknown | error, then
  details. Exit 0 / 1 (differ) / 2 (unknown, error)."""


def sexps(text):
    """Top-level s-expressions of SMT-LIB text, as source strings."""
    out, depth, start, i, n = [], 0, None, 0, len(text)
    while i < n:
        c = text[i]
        if c == ";":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "|":  # quoted symbol
            j = text.index("|", i + 1)
            i = j + 1
            continue
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 1
            i = j + 1
            continue
        if c == "(":
            if depth == 0:
                start = i
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                out.append(text[start : i + 1])
        i += 1
    return out


SYM = r"[A-Za-z0-9~!@$%^&*_+\-=<>.?/]"


def parse_query(smt):
    """-> (decls {name: sort}, formula string)"""
    decls, asserts = {}, []
    for f in sexps(smt):
        m = re.match(r"\(declare-fun\s+(\S+)\s+\(\)\s+(.*)\)\s*$", f, re.S)
        if m:
            decls[m.group(1)] = " ".join(m.group(2).split())
            continue
        m = re.match(r"\(declare-const\s+(\S+)\s+(.*)\)\s*$", f, re.S)
        if m:
            decls[m.group(1)] = " ".join(m.group(2).split())
            continue
        if f.startswith("(assert"):
            asserts.append(f[len("(assert") : -1].strip())
        elif (
            f.startswith("(check-sat") or f.startswith("(set-") or f.startswith("(get-")
        ):
            pass
        else:
            raise ValueError("unsupported form in query: " + f[:60])
    if not asserts:
        raise ValueError("query has no assert")
    return decls, "(and " + " ".join(asserts) + ")"


def rename(formula, mapping):
    if not mapping:
        return formula
    pat = re.compile(
        r"(?<!%s)(%s)(?!%s)"
        % (
            SYM,
            "|".join(re.escape(k) for k in sorted(mapping, key=len, reverse=True)),
            SYM,
        )
    )
    return pat.sub(lambda m: mapping[m.group(1)], formula)


def dump(sea_vc, mlir, program, flags):
    cmd = [sea_vc, mlir, "--program=" + program, "--smt2"] + flags
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(
            "sea-vc failed (%s): %s" % (" ".join(cmd), r.stdout + r.stderr)
        )
    # keep the SMT-LIB part (sea-vc also prints timing lines)
    smt = "\n".join(
        l for l in r.stdout.splitlines() if not re.match(r"^[a-z.\-]+:\s", l)
    )
    return smt


def z3_check(z3, text, timeout, getvals):
    with tempfile.NamedTemporaryFile("w", suffix=".smt2", delete=False) as f:
        f.write(text)
        path = f.name
    try:
        r = subprocess.run(
            [z3, "-T:%d" % timeout, path], capture_output=True, text=True
        )
    finally:
        os.unlink(path)
    lines = r.stdout.strip().splitlines()
    if not lines:
        return "error", r.stderr.strip()
    verdict = lines[0].strip()
    if verdict not in ("sat", "unsat"):
        return "unknown", r.stdout
    return verdict, "\n".join(lines[1:])


def check_program(args, mlir, program):
    A = dump(args.sea_vc, mlir, program, args.common + args.a)
    B = dump(args.sea_vc, mlir, args.b_program or program, args.common + args.b)
    da, fa = parse_query(A)
    db, fb = parse_query(B)
    shared = sorted(
        n
        for n in set(da) | set(db)
        if n.startswith("nd.") and da.get(n, db.get(n)) == db.get(n, da.get(n))
    )
    onesided = sorted(n for n in set(da) ^ set(db) if n in shared)
    clash = sorted(
        n for n in set(da) & set(db) if n.startswith("nd.") and n not in shared
    )
    sh = set(shared)
    sortof = {n: da.get(n, db.get(n)) for n in shared}
    ma = {n: "A!" + n for n in da if n not in sh}
    mb = {n: "B!" + n for n in db if n not in sh}
    fa, fb = rename(fa, ma), rename(fb, mb)
    pa = [(ma[n], da[n]) for n in da if n not in sh]
    pb = [(mb[n], db[n]) for n in db if n not in sh]

    def side(F, other_priv, formula, other_formula, own_priv):
        decl = "".join("(declare-fun %s () %s)\n" % (n, sortof[n]) for n in shared)
        decl += "".join("(declare-fun %s () %s)\n" % p for p in own_priv)
        binders = " ".join("(%s %s)" % p for p in other_priv)
        neg = (
            ("(forall (%s) (not %s))" % (binders, other_formula))
            if other_priv
            else "(not %s)" % other_formula
        )
        gv = "(get-value (%s))" % " ".join(shared) if shared else ""
        return decl + "(assert %s)\n(assert %s)\n(check-sat)\n%s\n" % (formula, neg, gv)

    res = []
    for tag, text in (
        ("A and not exists PB. B", side("A", pb, fa, fb, pa)),
        ("B and not exists PA. A", side("B", pa, fb, fa, pb)),
    ):
        v, extra = z3_check(args.z3, text, args.timeout, True)
        res.append((tag, v, extra))
        if args.keep:
            os.makedirs(args.keep, exist_ok=True)
            with open(
                os.path.join(args.keep, "%s.%s.smt2" % (program, "ab"[len(res) - 1])),
                "w",
            ) as f:
                f.write(text)
    verdicts = [v for _, v, _ in res]
    # the standalone verdicts, for the report
    standalone = []
    for _nm, _d, F, priv in (("A", da, fa, pa), ("B", db, fb, pb)):
        decl = "".join(
            "(declare-fun %s () %s)\n" % (n, sortof[n]) for n in shared
        ) + "".join("(declare-fun %s () %s)\n" % p for p in priv)
        v, _ = z3_check(
            args.z3, decl + "(assert %s)\n(check-sat)\n" % F, args.timeout, False
        )
        standalone.append(v)
    if all(v == "unsat" for v in verdicts):
        status = "equivalent"
    elif "sat" in verdicts:
        status = "differ"
    else:
        status = "unknown"
    return (
        status,
        res,
        shared,
        onesided + ["sort-clash:" + c for c in clash],
        standalone,
    )


def main():
    if "--agent" in sys.argv:
        print(AGENT)
        return 0
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("mlir", nargs="+")
    ap.add_argument("--agent", action="store_true", help="print a terse guide and exit")
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument(
        "--sea-vc", default=os.path.join(here, "..", "build", "bin", "sea-vc")
    )
    ap.add_argument("--z3", default=DEFAULT_Z3)
    ap.add_argument("--common", default="--lower")
    ap.add_argument("--a", default="")
    ap.add_argument("--b", default="--seam")
    ap.add_argument(
        "--program",
        action="append",
        help="program name (default: every program of the file)",
    )
    ap.add_argument(
        "--b-program",
        help="program for side B (default: the same; a self-test: two programs must differ)",
    )
    ap.add_argument("--timeout", type=int, default=60)
    ap.add_argument("--keep", help="directory for the z3 inputs")
    args = ap.parse_args()
    args.common, args.a, args.b = args.common.split(), args.a.split(), args.b.split()
    worst = 0
    for mlir in args.mlir:
        progs = args.program or re.findall(
            r"sea\.program\s+@\"?([\w.$-]+)\"?", open(mlir).read()
        )
        for p in progs:
            try:
                status, res, shared, onesided, standalone = check_program(args, mlir, p)
            except Exception as e:  # noqa
                print(
                    "%s %s: error: %s"
                    % (os.path.basename(mlir), p, str(e).splitlines()[0][:300])
                )
                worst = max(worst, 2)
                continue
            print(
                "%s %s: %s (A %s, B %s; shared nd: %d%s)"
                % (
                    os.path.basename(mlir),
                    p,
                    status,
                    standalone[0],
                    standalone[1],
                    len(shared),
                    ("; dropped by one side: " + " ".join(onesided))
                    if onesided
                    else "",
                )
            )
            if status != "equivalent":
                for tag, v, extra in res:
                    if v != "unsat":
                        print(
                            "  [%s] %s\n    %s"
                            % (tag, v, extra.replace("\n", "\n    ")[:1500])
                        )
            worst = max(worst, {"equivalent": 0, "differ": 1}.get(status, 2))
    return worst


if __name__ == "__main__":
    sys.exit(main())

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
run with the same arguments plus --smt2. OLD and NEW below are the
conjunctions of the asserts of the two queries; symbols are matched by
name (a declaration of both queries is one symbol). Each job gets the
first class that applies, in this order:

  identical   byte-identical queries.
  order       equal after sorting the arguments of commutative operators
              (bvadd bvmul and or =) and the top-level forms.
  both-unsat  z3 proves OLD unsat and NEW unsat, each by its own check, or
              (reason=derived) one by its own check and the other, whose
              check was unknown, through a proof of OLD <=> NEW.
              Equisatisfiability only: says nothing about the change beyond
              the verdict (any two unsat queries are both-unsat).
  DIFF        OLD and NEW differ: one is sat and the other unsat, or z3
              finds an assignment of the symbols on which they differ
              (sat of (distinct OLD NEW)).
  equiv-sat   both are sat and z3 proves (distinct OLD NEW) unsat: the same
              assignments of all symbols satisfy them (a symbol of one query
              only is unconstrained by the other). Real equivalence.
              reason=derived: one side's sat check was unknown, its
              satisfiability follows from the other's and OLD <=> NEW.
  unknown     z3 decided neither: rlimit, timeout or incompleteness on a
              check (reason= names it). Not a DIFF and not a pass.
  error       a binary failed or printed no query, z3 failed, or the
              queries cannot be compared (a symbol with two sorts,
              an unsupported command).

Checks, cheapest first: the text checks; then sat(OLD) and sat(NEW), one
z3 run each (the common unsat-unsat case ends here, as does a verdict
change); then (distinct OLD NEW), unless the two verdicts decide. If the
equivalence check proves equivalence while one side is unknown, the
other side's verdict carries over (reason=derived).

--project-blocks: block guards (declarations ^bbN_k) are existentially
projected: the equivalence checked is (exists g. OLD) <=> (exists g'. NEW),
g and g' the guards of each query, which makes renumbered blocks
equivalent. The quantifier-free check by names runs first (it implies the
projected one); only when it finds a distinguishing assignment does a
quantified check run (equiv-sat by=exists when it proves equivalence).
Sat checks are unchanged: exists g. OLD is sat iff OLD is.

Skolem hints (designed, not implemented): a map from each guard of one
query to a term over the symbols of the other replaces the quantifier:
NEW(x, g') => OLD(x, s(g')) and OLD(x, g) => NEW(x, t(g)) are
quantifier-free. A wrong hint makes a check sat: it can cost a fallback
to the quantified check, never a false pass. See project_equiv().

Gate: both-unsat passes (it keeps the verdict), but its weakness is
shown: the evidence line counts the jobs by what they prove, and
--strict fails (exit 3) when some job is only both-unsat.

Every z3 run has a resource limit (--rlimit, deterministic) and a
wall-clock backstop (--timeout). z3 runs single-threaded by default
(threads=1, parallel.enable=false), so --par N uses at most N cores
for z3. The A-INB report (seam.inb: accesses /
conditions, on stderr with --seam-stats) is compared too, when both
print it.
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
  argument order): classify every job identical / order / both-unsat /
  equiv-sat / DIFF / unknown / error.
USAGE:
  utils/query-diff.py --old <sea-vc> --new <sea-vc> <corpus-dir | f.mlir...>
      [--args="--lower --widen-memory=8 --seam"] [--par 12]
      [--z3 <z3>] [--rlimit 200000000] [--timeout 300] [--project-blocks]
      [--strict] [--keep <dir>]
CLASSES (what each proves; OLD/NEW = the queries, symbols matched by name):
  identical   byte-identical queries
  order       equal up to the order of commutative arguments and forms
  both-unsat  OLD unsat and NEW unsat, two separate z3 checks (reason=derived:
              one check and OLD <=> NEW); only equisatisfiable, NOT
              evidence of equivalence
  equiv-sat   both sat and OLD <=> NEW for every assignment (z3)
              (reason=derived: one sat check unknown, sat by equivalence)
              (by=exists: (exists guards. OLD) <=> (exists guards'. NEW))
  DIFF        one sat and one unsat, or a distinguishing assignment
  unknown     a z3 check gave unknown (rlimit, timeout): no proof either way
  error       sea-vc or z3 failed, or the queries are not comparable
ORDER: identical, order, sat(OLD) + sat(NEW), then (distinct OLD NEW)
  unless both are unsat or one is sat and the other unsat;
  --project-blocks: a quantified check of the ^bbN_k guards only after
  the by-name check found a difference.
OUTPUT (stdout, one line per job, then totals):
  <job> <class> [reason=<r>] [old=<verdict> new=<verdict>] [by=exists]
      [inb old=<acc>/<cond> new=<acc>/<cond>]
  total: identical=<n> order=<n> both-unsat=<n> equiv-sat=<n> diff=<n> unknown=<n> error=<n> [by-exists=<n>]
  evidence: equiv-sat=<n> identical+order=<n> both-unsat=<n> (verdict only)
  limits: rlimit=<n> timeout=<s> project-blocks=<0|1>
  [strict: fail both-unsat=<n> (verdict only, no evidence about the change)]
  inb: accesses <old> -> <new> conditions <old> -> <new>
STATUS: 0 if every job is identical / order / both-unsat / equiv-sat;
  1 if some job is DIFF or error; 2 if some is unknown; 3 with --strict
  if some job is both-unsat (checked in this order).
--keep <dir> keeps the two queries of every DIFF / unknown / error job.
"""

COMM = {"bvadd", "bvmul", "and", "or", "="}
CLASSES = ("identical", "order", "both-unsat", "equiv-sat", "DIFF", "unknown", "error")
GUARD = re.compile(r"\^bb\d+_\d+")


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


class Incomparable(Exception):
    """The queries cannot be put into one script (reported as error)."""


class Query:
    """One query: its declarations (name -> (sort text, command)), its
    zero-argument definitions and the conjunction of its asserts."""

    IGNORED = ("check-sat", "set-info", "set-logic", "set-option", "exit")

    def __init__(self, text):
        self.decls, self.defs, self.asserts = {}, [], []
        for f in parse(tokens(text)):
            if not isinstance(f, list) or not f:
                raise Incomparable(f"unsupported-form:{ser(f)[:40]}")
            head = f[0]
            if head == "declare-fun" and len(f) == 4:
                self.decls[f[1]] = (ser(f[2]) + " " + ser(f[3]), ser(f))
            elif head == "declare-const" and len(f) == 3:
                self.decls[f[1]] = ("() " + ser(f[2]), ser(f))
            elif head == "define-fun":
                self.defs.append((f[1], ser(f)))
            elif head == "assert":
                self.asserts.append(ser(f[1]))
            elif head not in self.IGNORED:
                raise Incomparable(f"unsupported-command:{head}")

    def formula(self):
        return "(and true " + " ".join(self.asserts) + ")"

    def guards(self):
        return [n for n in self.decls if GUARD.fullmatch(n)]

    def script(self):
        """This query alone (no set-logic: the queries use const arrays and
        bvumul_noovfl, which QF_AUFBV rejects)."""
        out = [c for _, c in self.decls.values()] + [c for _, c in self.defs]
        return out + [f"(assert {self.formula()})"]


def shared_decls(o, n, skip=()):
    """The declarations of both queries; a name with two sorts (or one
    name defined differently) is not comparable."""
    out = {}
    for q in (o, n):
        for name, (sort, cmd) in q.decls.items():
            if name in skip:
                continue
            if name in out and out[name][0] != sort:
                raise Incomparable(f"sort-conflict:{name}")
            out[name] = (sort, cmd)
    defs = {}
    for q in (o, n):
        for name, cmd in q.defs:
            if name in defs and defs[name] != cmd:
                raise Incomparable(f"define-conflict:{name}")
            defs[name] = cmd
    return [c for _, c in out.values()] + list(defs.values())


def equiv_script(o, n):
    """Unsat iff OLD <=> NEW for every assignment of the symbols."""
    out = shared_decls(o, n)
    out.append(f"(define-fun OLD__q () Bool {o.formula()})")
    out.append(f"(define-fun NEW__q () Bool {n.formula()})")
    return out + ["(assert (distinct OLD__q NEW__q))"]


def exists(q):
    gs = q.guards()
    if not gs:
        return q.formula()
    for name, cmd in q.defs:
        if set(gs) & set(tokens(cmd)):
            # the quantifier would not bind the guard inside the definition
            raise Incomparable(f"guard-in-define:{name}")
    binders = " ".join(f"({g} {q.decls[g][0].split(' ', 1)[1]})" for g in gs)
    return f"(exists ({binders}) {q.formula()})"


def project_equiv(o, n, hints=None):
    """Unsat iff (exists g. OLD) <=> (exists g'. NEW), g / g' the block
    guards of each query; the other symbols are matched by name.

    hints: the hook for Skolem hints, a pair of maps (old guard -> term over
    the symbols of NEW, new guard -> term over the symbols of OLD), e.g.
    from a side file written by the pass (lines `old-symbol new-term`).
    With them the check would be two quantifier-free scripts,
    NEW and not OLD[g := s(g')], and OLD and not NEW[g' := t(g)], each unsat;
    a sat one falls back to this quantified script. Not implemented."""
    if hints is not None:
        raise NotImplementedError("Skolem hints for --project-blocks")
    skip = set(o.guards()) | set(n.guards())
    out = shared_decls(o, n, skip)
    out.append(f"(define-fun OLD__q () Bool {exists(o)})")
    out.append(f"(define-fun NEW__q () Bool {exists(n)})")
    return out + ["(assert (distinct OLD__q NEW__q))"]


class Z3:
    def __init__(self, path, rlimit, timeout):
        self.path, self.rlimit, self.timeout = path, rlimit, timeout

    def check(self, lines):
        """sat / unsat / unknown:<reason> / error:<text>, for a script."""
        text = "\n".join(lines + ["(check-sat)", "(get-info :reason-unknown)"]) + "\n"
        with tempfile.NamedTemporaryFile("w", suffix=".smt2", delete=False) as t:
            t.write(text)
        cmd = [self.path, f"rlimit={self.rlimit}", f"-T:{self.timeout}", "-st", t.name]
        try:
            z = subprocess.run(
                cmd, capture_output=True, text=True, timeout=self.timeout + 30
            )
        except subprocess.TimeoutExpired:
            return "unknown:timeout"
        finally:
            os.unlink(t.name)
        out = z.stdout.splitlines()
        first = out[0].strip() if out else ""
        if first in ("sat", "unsat"):
            return first
        if first == "timeout":
            return "unknown:timeout"
        if first == "unknown":
            # z3 says "canceled" for an exhausted rlimit in some tactics:
            # the resource count tells the two apart
            m = re.search(r":rlimit-count\s+(\d+)", z.stdout)
            if self.rlimit and m and int(m.group(1)) >= self.rlimit:
                return "unknown:rlimit"
            why = out[1] if len(out) > 1 else ""
            if "resource limit" in why:
                return "unknown:rlimit"
            if "timeout" in why or "canceled" in why:
                return "unknown:timeout"
            return "unknown:incomplete"
        return "error:z3:" + (first or z.stderr.strip())[:60].replace(" ", "_")


def classify(old, new, z3, project_blocks=False):
    """(class, details) for two query texts; details are key=value strings."""
    if old == new:
        return "identical", []
    if normalized(old) == normalized(new):
        return "order", []
    try:
        o, n = Query(old), Query(new)
    except Incomparable as e:
        return "error", [f"reason={e}"]
    so = z3.check(o.script())
    sn = z3.check(n.script())
    for s in (so, sn):
        if s.startswith("error"):
            return "error", [f"reason={s}"]
    if so == sn == "unsat":
        return "both-unsat", []
    if {so, sn} == {"sat", "unsat"}:
        return "DIFF", ["reason=verdict", f"old={so}", f"new={sn}"]
    # neither is unsat, or one is unsat and the other unknown
    sides = [f"old={so}", f"new={sn}"]
    try:
        e = z3.check(equiv_script(o, n))
        by = []
        if e == "sat" and project_blocks and (o.guards() or n.guards()):
            e = z3.check(project_equiv(o, n))
            by = ["by=exists"]
    except Incomparable as x:
        return "error", [f"reason={x}"]
    if e.startswith("error"):
        return "error", [f"reason={e}"]
    if e == "sat":
        return "DIFF", ["reason=model"] + by
    if e != "unsat":
        return "unknown", [f"reason=equiv-{e.split(':')[1]}"] + sides + by
    # OLD and NEW are equivalent: a known verdict of one side is the other's
    if so == sn == "sat":
        return "equiv-sat", by
    if "sat" in (so, sn):
        return "equiv-sat", ["reason=derived"] + sides + by
    if "unsat" in (so, sn):
        return "both-unsat", ["reason=derived"] + sides + by
    return "unknown", ["reason=verdict-unknown"] + sides + by


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
        cls, details = "error", ["reason=sea-vc"]
    else:
        cls, details = classify(
            old, new, Z3(a.z3, a.rlimit, a.timeout), a.project_blocks
        )
    if a.keep and cls in ("DIFF", "unknown", "error"):
        os.makedirs(a.keep, exist_ok=True)
        for tag, text in (("old", old), ("new", new)):
            if text:
                open(os.path.join(a.keep, f"{job}.{tag}.smt2"), "w").write(text)
    return job, cls, details, inb_o, inb_n


def main():
    if "--agent" in sys.argv:
        print(AGENT, end="")
        return 0
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n", 1)[0],
        epilog=AGENT.split("CLASSES", 1)[1].split("OUTPUT", 1)[0].join(["CLASSES", ""]),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--old", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("--args", default="--lower --widen-memory=8 --seam")
    ap.add_argument("--par", type=int, default=12)
    ap.add_argument("--z3", default=os.environ.get("SEA_Z3", "z3"))
    ap.add_argument(
        "--rlimit",
        type=int,
        default=200000000,
        help="z3 resource limit of each check (deterministic; 0 = none;"
        " default: %(default)s)",
    )
    ap.add_argument(
        "--timeout",
        type=int,
        default=300,
        help="seconds, per sea-vc run and per z3 check (a backstop;"
        " default: %(default)s)",
    )
    ap.add_argument(
        "--project-blocks",
        action="store_true",
        help="equivalence up to existential projection of ^bbN_k guards",
    )
    ap.add_argument(
        "--strict",
        action="store_true",
        help="exit 3 when some job is only both-unsat (verdict, not equivalence)",
    )
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
    counts = {k: 0 for k in CLASSES}
    by_exists = 0
    inb = [0, 0, 0, 0]
    with cf.ThreadPoolExecutor(a.par) as ex:
        for job, cls, details, io, iN in ex.map(lambda f: one(f, a), files):
            counts[cls] += 1
            by_exists += cls == "equiv-sat" and "by=exists" in details
            line = " ".join([job, cls] + details)
            if io and iN:
                line += f" inb old={io[0]}/{io[1]} new={iN[0]}/{iN[1]}"
                inb = [inb[0] + io[0], inb[1] + iN[0], inb[2] + io[1], inb[3] + iN[1]]
            print(line, flush=True)
    total = " ".join(f"{k.lower()}={v}" for k, v in counts.items())
    if a.project_blocks:
        total += f" by-exists={by_exists}"
    print("total: " + total)
    print(
        f"evidence: equiv-sat={counts['equiv-sat']} "
        f"identical+order={counts['identical'] + counts['order']} "
        f"both-unsat={counts['both-unsat']} (verdict only)"
    )
    print(
        f"limits: rlimit={a.rlimit} timeout={a.timeout} "
        f"project-blocks={int(a.project_blocks)}"
    )
    print(f"inb: accesses {inb[0]} -> {inb[1]} conditions {inb[2]} -> {inb[3]}")
    if a.strict and counts["both-unsat"]:
        print(
            f"strict: fail both-unsat={counts['both-unsat']} "
            "(verdict only, no evidence about the change)"
        )
    if counts["DIFF"] or counts["error"]:
        return 1
    if counts["unknown"]:
        return 2
    return 3 if a.strict and counts["both-unsat"] else 0


if __name__ == "__main__":
    sys.exit(main())

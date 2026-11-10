#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""The query-diff classifier on small handwritten query pairs.

usage: query-diff.py <utils/query-diff.py> <z3>
"""

import importlib.util
import os
import subprocess
import sys
import tempfile

spec = importlib.util.spec_from_file_location("query_diff", sys.argv[1])
qd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qd)
z3 = qd.Z3(sys.argv[2], rlimit=0, timeout=60)

X = "(declare-fun x () (_ BitVec 8))\n"


def q(body, decls=X):
    return decls + f"(assert {body})\n(check-sat)\n"


def expect(cls, old, new, project=False, solver=z3):
    got, details = qd.classify(old, new, solver, project)
    assert got == cls, (cls, got, details, old, new)
    return details


# The text classes.
a = q("(bvult x #x05)")
expect("identical", a, a)
expect("order", q("(= x (bvadd x #x00))"), q("(= x (bvadd #x00 x))"))

# equiv-sat: sat, and the same assignments satisfy both.
expect("equiv-sat", q("(bvult x #x05)"), q("(not (bvuge x #x05))"))

# both-unsat: two unsat queries, however unrelated; never equiv-sat.
expect("both-unsat", q("(bvult x #x00)"), q("(and (= x #x01) (= x #x02))"))

# DIFF: a verdict change, and two sat queries with a distinguishing x.
assert "new=unsat" in expect("DIFF", q("(bvult x #x05)"), q("(bvult x #x00)"))
assert "reason=model" in expect("DIFF", q("(bvult x #x05)"), q("(bvult x #x06)"))

# unknown: a resource limit is reported as unknown, not as DIFF.
F = (
    "(declare-fun x () (_ BitVec 32))\n(declare-fun y () (_ BitVec 32))\n"
    "(assert (and (bvugt x #x00000001) (bvugt y #x00000001)"
    " (bvult x #x0000ffff) (bvult y #x0000ffff)"
    " (= (bvmul x y) #x3b9aca07)))\n(check-sat)\n"
)
d = expect(
    "unknown",
    F,
    F.replace("(bvugt x", "(bvugt (bvadd x #x00000000)"),
    solver=qd.Z3(sys.argv[2], rlimit=1000, timeout=60),
)
# equivalent by the (easy) equivalence check, neither verdict known
assert d == ["reason=verdict-unknown", "old=unknown:rlimit", "new=unknown:rlimit"], d

# error: one symbol with two sorts cannot be compared by name.
expect(
    "error",
    q("(= x #x01)"),
    q("(= x #x0001)", "(declare-fun x () (_ BitVec 16))\n"),
)

# Renamed block guards: the same program with ^bb1 / ^bb2 swapped. DIFF by
# names; equivalent once the guards are projected.
G = (
    X
    + "(declare-fun y () (_ BitVec 8))\n"
    + ("(declare-fun ^bb1_0 () Bool)\n(declare-fun ^bb2_0 () Bool)\n")
)


def blocks(then, other):
    return q(
        f"(and (= {then} (bvult x #x05)) (= {other} (not {then}))"
        f" (=> {then} (= y x)) (=> {other} (= y #x00)))",
        G,
    )


old, new = blocks("^bb1_0", "^bb2_0"), blocks("^bb2_0", "^bb1_0")
expect("DIFF", old, new)
assert "by=exists" in expect("equiv-sat", old, new, project=True)
# Projection does not hide a change of the values: y = x + 1 in the then block.
changed = new.replace("(= y x)", "(= y (bvadd x #x01))")
expect("DIFF", old, changed, project=True)
# A guard set that grows (an extra, unconstrained block) is still equivalent.
extra = new.replace("(declare-fun x", "(declare-fun ^bb3_0 () Bool)\n(declare-fun x")
assert "by=exists" in expect("equiv-sat", old, extra, project=True)

# The Skolem-hint hook is not implemented: it says so.
try:
    qd.project_equiv(qd.Query(old), qd.Query(new), hints=({}, {}))
    raise AssertionError("hints accepted")
except NotImplementedError:
    pass


# Derived verdicts (a stub z3 answers in order: OLD, NEW, equivalence).
class Stub:
    def __init__(self, *answers):
        self.answers = list(answers)

    def check(self, lines):
        return self.answers.pop(0)


s1, s2 = q("(bvult x #x05)"), q("(not (bvuge x #x05))")
d = expect("equiv-sat", s1, s2, solver=Stub("sat", "unknown:rlimit", "unsat"))
assert d[0] == "reason=derived" and "new=unknown:rlimit" in d, d
d = expect("both-unsat", s1, s2, solver=Stub("unknown:timeout", "unsat", "unsat"))
assert d[0] == "reason=derived" and "old=unknown:timeout" in d, d
# A verdict alone never derives: equivalence unknown stays unknown.
expect("unknown", s1, s2, solver=Stub("unknown:rlimit", "unsat", "unknown:rlimit"))

# Projection refuses a definition that mentions a guard (the quantifier
# would not bind it there).
dg = "(define-fun d () Bool ^bb1_0)\n"
d = expect(
    "error",
    old.replace("(assert", dg + "(assert"),
    new.replace("(assert", dg + "(assert"),
    project=True,
)
assert d == ["reason=guard-in-define:d"], d

# An unsupported command, and a query z3 rejects, are errors, not DIFF.
d = expect("error", a, a.replace("(assert", "(push 1)\n(assert"))
assert d == ["reason=unsupported-command:push"], d
d = expect("error", a, q("(foo x)"))
assert d[0].startswith("reason=error:z3:"), d

# The command line: --strict fails a run whose jobs are only both-unsat.
with tempfile.TemporaryDirectory() as t:
    for tag, body in (("old", "(bvult x #x00)"), ("new", "(= x (bvadd x #x01))")):
        with open(os.path.join(t, tag + ".smt2"), "w") as f:
            f.write(q(body))
        with open(os.path.join(t, tag), "w") as f:
            f.write(f"#!/bin/sh\ncat {os.path.join(t, tag)}.smt2\n")
        os.chmod(os.path.join(t, tag), 0o755)
    job = os.path.join(t, "job.mlir")
    open(job, "w").close()
    base = [
        sys.executable,
        sys.argv[1],
        "--old",
        os.path.join(t, "old"),
        "--new",
        os.path.join(t, "new"),
        "--z3",
        sys.argv[2],
        "--args=",
        job,
    ]
    p = subprocess.run(base, capture_output=True, text=True)
    assert p.returncode == 0, p
    assert "job both-unsat\n" in p.stdout, p.stdout
    assert (
        "evidence: equiv-sat=0 identical+order=0 both-unsat=1 (verdict only)"
        in p.stdout
    ), p.stdout
    p = subprocess.run(base + ["--strict"], capture_output=True, text=True)
    assert p.returncode == 3, p
    assert "strict: fail both-unsat=1" in p.stdout, p.stdout

print("query-diff: ok")

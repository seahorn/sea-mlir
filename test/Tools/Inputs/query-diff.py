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


# Skolem hints: block ids (sea-vc's `; sea.block-ids` lines) on both
# queries replace the quantified check by two quantifier-free ones.
def ids(text, **guards):
    return text + "".join(f"; sea.block-ids {g} {i}\n" for g, i in guards.items())


def with_ids(text, then, other):
    return ids(text, **{then: "1", other: "2"})


h_old = with_ids(old, "^bb1_0", "^bb2_0")
h_new = with_ids(new, "^bb2_0", "^bb1_0")
# a correct hint: by=hints, no quantified check
assert expect("equiv-sat", h_old, h_new, project=True) == ["by=hints"]
# --no-hints: the quantified check
assert "by=exists" in qd.classify(h_old, h_new, z3, True, hints=None)[1]
# a wrong hint (the ids name the other block) falls back to the quantified
# check and says which direction failed; still equivalent
wrong = with_ids(new, "^bb1_0", "^bb2_0")
d = expect("equiv-sat", h_old, wrong, project=True)
assert "by=exists" in d and any(x.startswith("hints=old-new:sat") for x in d), d
# a hint and a real difference: DIFF (the hint check is sat, so is the
# quantified one)
d = expect("DIFF", h_old, with_ids(changed, "^bb2_0", "^bb1_0"), project=True)
assert "by=exists" in d and "hints=old-new:sat" in d, d
# hand-written hints: a wrong one never passes a real difference, a right
# one passes the renaming
o_q, c_q = qd.Query(old), qd.Query(changed)
for hint in (
    ({"^bb1_0": "^bb2_0", "^bb2_0": "^bb1_0"},) * 2,
    ({"^bb1_0": "true", "^bb2_0": "true"},) * 2,
    ({"^bb1_0": "false", "^bb2_0": "false"},) * 2,
):
    got, d = qd.classify(old, changed, z3, True, hints=hint)
    assert got == "DIFF", (hint, got, d)
    for name, script in qd.project_equiv(o_q, c_q, hint):
        assert z3.check(script) == "sat", (hint, name)
swap = ({"^bb1_0": "^bb2_0", "^bb2_0": "^bb1_0"},) * 2
assert qd.classify(old, new, z3, True, hints=swap) == ("equiv-sat", ["by=hints"])
# a merged block: NEW holds ^bb1 and ^bb2 of OLD in one block ^bb1 (ids 1,2)
M = X + "(declare-fun y () (_ BitVec 8))\n"
m_old = ids(
    q(
        "(and ^bb0_0 (=> ^bb1_0 ^bb0_0) (=> ^bb2_0 ^bb1_0) ^bb2_0"
        " (=> ^bb1_0 (bvult x #x05)) (=> ^bb2_0 (= y x)))",
        M + "(declare-fun ^bb0_0 () Bool)\n(declare-fun ^bb1_0 () Bool)\n"
        "(declare-fun ^bb2_0 () Bool)\n",
    ),
    **{"^bb0_0": "0", "^bb1_0": "1", "^bb2_0": "2"},
)
m_new = ids(
    q(
        "(and ^bb0_0 (=> ^bb1_0 ^bb0_0) ^bb1_0 (=> ^bb1_0 (bvult x #x05))"
        " (=> ^bb1_0 (= y x)))",
        M + "(declare-fun ^bb0_0 () Bool)\n(declare-fun ^bb1_0 () Bool)\n",
    ),
    **{"^bb0_0": "0", "^bb1_0": "1,2"},
)
assert expect("equiv-sat", m_old, m_new, project=True) == ["by=hints"]
s_, t_, missing = qd.derive_hints(qd.Query(m_old), qd.Query(m_new))
assert s_ == {"^bb0_0": "^bb0_0", "^bb1_0": "^bb1_0", "^bb2_0": "^bb1_0"}, s_
assert t_["^bb1_0"] == "(and ^bb1_0 ^bb2_0)" and missing == 0, t_
# a block the other side erased (input id, held nowhere): false; a derived
# one the other side never made: no hint, its cases expanded
e_old = ids(m_old, **{"^bb3_0": "3"}).replace(
    "(declare-fun x", "(declare-fun ^bb3_0 () Bool)\n(declare-fun x"
)
s_, _, _ = qd.derive_hints(qd.Query(e_old), qd.Query(m_new))
assert s_["^bb3_0"] == "false", s_
r_old = ids(m_old, **{"^bb3_0": "2.q0"}).replace(
    "(declare-fun x", "(declare-fun ^bb3_0 () Bool)\n(declare-fun x"
)
s_, _, _ = qd.derive_hints(qd.Query(r_old), qd.Query(m_new))
assert s_["^bb3_0"] is None, s_
assert expect("equiv-sat", r_old, m_new, project=True) == ["by=hints"]
# a guard without ids: no hints (reported), the quantified check
p_old = m_old.replace("; sea.block-ids ^bb2_0 2\n", "")
d = expect("equiv-sat", p_old, m_new, project=True)
assert d == ["by=exists", "hints=incomplete:1"], d
# the comments are not part of the query: identical without them
assert expect("identical", h_old, old) == []


# A symbol of one query only that it defines as a numeral (an object's
# address, obj.<name> = base, of an object the other lowering erased) is
# substituted: exists o. (o = c and phi) is phi[c/o]; not a free input.
O = "(declare-fun o () (_ BitVec 8))\n"
d = expect(
    "equiv-sat", q("(and (= o #x10) (bvult x #x05))", X + O), q("(bvult x #x05)")
)
assert d == ["subst=1"], d
# either side, either order of the equality, nested and, (_ bvN w)
d = expect(
    "equiv-sat",
    q("(bvult x #x05)"),
    q("(and (bvult x #x05) (and (= (_ bv16 8) o) (bvult x (bvadd o #x00))))", X + O),
)
assert d == ["subst=1"], d
# through let bodies (z3 prints shared terms as lets)
d = expect(
    "equiv-sat",
    q("(let ((a!1 (bvult x #x05))) (and (= o #x10) a!1))", X + O),
    q("(bvult x #x05)"),
)
assert d == ["subst=1"], d
# a name a binder also uses is not substituted (it may be the binder's)
expect(
    "DIFF",
    q("(and (= o #x10) (let ((o x)) (bvult o #x05)))", X + O),
    q("(bvult x #x05)"),
)
# the value counts: o = #x10 with x < o is not x < 5
assert "subst=1" in expect(
    "DIFF", q("(and (= o #x10) (bvult x o))", X + O), q("(bvult x #x05)")
)
# a one-sided symbol that is not defined (under an implication, or by a
# non-numeral) stays a free input: today's behavior
expect("DIFF", q("(=> (bvult x #x05) (= o #x10))", X + O), q("true"))
expect("DIFF", q("(and (= o x) (bvult o #x05))", X + O), q("(bvult x #x05)"))
# a symbol of both queries is never substituted
expect("DIFF", q("(and (= o #x10) (bvult x #x05))", X + O), q("(bvult x #x05)", X + O))


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

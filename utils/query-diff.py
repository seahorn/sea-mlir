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

Defined object addresses: a symbol obj.<name> declared in one query only
(not a block guard, not a bound name) that a top-level conjunct of that
query (through `and` and `let` bodies) defines as a numeral, (= x c), is
substituted by c in that query before any z3 check
(details: subst=<n>). Exact: exists x. (x = c and phi) is phi[c/x]. OpSem
names the address of a placed object obj.<segment name>; the other
lowering may have erased the object, and without the substitution its
query leaves the symbol free and the equivalence check finds a
"difference" in it. Any other one-sided symbol (a program input, nd.<site>
too) stays a free input.

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

Skolem hints: before the quantified check, a map s from each guard of
OLD to a term over NEW's guards, and t from each guard of NEW to a term
over OLD's, replace the quantifiers: OLD(x, g) and not NEW(x, t(g)), and
NEW(x, g') and not OLD(x, s(g')), two quantifier-free checks. Both unsat
proves (exists g. OLD) <=> (exists g'. NEW) (equiv-sat by=hints): t(g)
and s(g') are the witnesses. The hints are checked, not trusted: a wrong
one makes its check sat, and the quantified check runs (by=exists, with
hints=<direction>:<answer> saying which failed); it never makes a pass.
The hints come from the block ids sea-vc prints with a query of a program
that carries them (`; sea.block-ids <guard> <id>,...`: sea-vc --reach
programs, R_T): each guard gets the conjunction of the guards of the
other query whose blocks hold one of its ids, false for an input block
the other lowering erased, or no term (a block the other lowering did not
make), and then the check expands its cases (at most HINT_CASES_MAX per
check). See derive_hints() and project_equiv().

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
      [--no-hints]
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
              (by=hints: the same, by the quantifier-free Skolem-hint
              checks; both queries carry `; sea.block-ids` lines)
  DIFF        one sat and one unsat, or a distinguishing assignment
  unknown     a z3 check gave unknown (rlimit, timeout): no proof either way
  error       sea-vc or z3 failed, or the queries are not comparable
SUBST: a symbol obj.<name> declared in one query only (not a guard) that a
  top-level conjunct of it (through and / let bodies) defines as a numeral,
  (= x #x..), is replaced by the numeral (exact; the address of an object
  one lowering erased): subst=<n>. Other one-sided symbols are free inputs.
ORDER: identical, order, sat(OLD) + sat(NEW), then (distinct OLD NEW)
  unless both are unsat or one is sat and the other unsat;
  --project-blocks: after the by-name check found a difference, the
  Skolem-hint checks (queries with `; sea.block-ids` lines: sea-vc on R_T),
  then, if a hint check is sat or unknown, the quantified check.
OUTPUT (stdout, one line per job, then totals):
  <job> <class> [reason=<r>] [old=<verdict> new=<verdict>] [subst=<n>]
      [by=hints|by=exists [hints=<old-new|new-old>:<answer>|incomplete:<n>|none:<why>]]
      [inb old=<acc>/<cond> new=<acc>/<cond>]
  total: identical=<n> order=<n> both-unsat=<n> equiv-sat=<n> diff=<n> unknown=<n> error=<n> [by-hints=<n> by-exists=<n>]
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
BLOCK_IDS = re.compile(r"; sea\.block-ids (\S+) (\S+)")


def strip_comments(text):
    """The query without its comment lines (the block ids)."""
    return "\n".join(x for x in text.splitlines() if not x.lstrip().startswith(";"))


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
        # guard -> the ids of its block (sea-vc's `; sea.block-ids` lines)
        self.block_ids = {}
        for line in text.splitlines():
            m = BLOCK_IDS.fullmatch(line.strip())
            if m:
                self.block_ids[m.group(1)] = m.group(2).split(",")
        for f in parse(tokens(strip_comments(text))):
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


NUMERAL = re.compile(r"#x[0-9a-fA-F]+|#b[01]+")


def is_numeral(x):
    """A bit-vector numeral: #x.., #b.. or (_ bvN w)."""
    if isinstance(x, str):
        return NUMERAL.fullmatch(x) is not None
    return (
        len(x) == 3
        and x[0] == "_"
        and isinstance(x[1], str)
        and x[1].startswith("bv")
        and x[1][2:].isdigit()
    )


def conjuncts(f):
    """The top-level conjuncts of a formula: nested `and` flattened, and
    through the body of a `let` ((let B (and c1 c2)) is (and (let B c1)
    (let B c2)); a conjunct that mentions no name of B is c itself)."""
    if isinstance(f, list) and f and f[0] == "and":
        return [c for k in f[1:] for c in conjuncts(k)]
    if isinstance(f, list) and len(f) == 3 and f[0] == "let":
        return conjuncts(f[2])
    return [f]


def bound_names(x, out):
    """The names a let, exists or forall binds anywhere in x."""
    if isinstance(x, list):
        if len(x) == 3 and x[0] in ("let", "exists", "forall"):
            out.update(b[0] for b in x[1] if isinstance(b, list) and b)
        for k in x:
            bound_names(k, out)
    return out


def substitute_defined(q, other_names):
    """Eliminates from q every symbol declared in q only (not a block
    guard, no arguments, not used by a define-fun, not a bound name) that
    a top-level conjunct of q (through `and` and `let` bodies) defines as
    a numeral, (= x c) or (= c x): x is replaced
    by c everywhere in q's asserts and its declaration dropped. Exact:
    exists x. (x = c and phi) is phi[c/x]; only symbols named
    obj.<name> (a placed object's address) are candidates: they are not
    inputs, so they must not count as ones that the other query leaves
    free. Other one-sided symbols (program inputs) stay.
    Returns the number of symbols eliminated."""
    own = {
        x
        for x, (sort, _) in q.decls.items()
        if x not in other_names
        and x.startswith("obj.")
        and not GUARD.fullmatch(x)
        and sort.startswith("() ")
    }
    if not own:
        return 0
    in_defs = {t for _, cmd in q.defs for t in tokens(cmd)}
    trees = [parse(tokens(a))[0] for a in q.asserts]
    # a name some binder of q also uses could be that binder's anywhere
    bound = set()
    for t in trees:
        bound_names(t, bound)
    own -= bound
    sub = {}
    for t in trees:
        for c in conjuncts(t):
            if isinstance(c, list) and len(c) == 3 and c[0] == "=":
                for x, v in ((c[1], c[2]), (c[2], c[1])):
                    if (
                        isinstance(x, str)
                        and x in own
                        and x not in in_defs
                        and x not in sub
                        and is_numeral(v)
                    ):
                        sub[x] = v
    if not sub:
        return 0

    def put(x):
        if isinstance(x, str):
            return sub.get(x, x)
        return [put(k) for k in x]

    q.asserts = [ser(put(t)) for t in trees]
    for x in sub:
        del q.decls[x]
    return len(sub)


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


# a guard without a hint stays existential; up to this many per check are
# expanded into a disjunction of their cases (2^k applications of the
# query); more: no hints (untuned)
HINT_CASES_MAX = 6


def derive_hints(o, n):
    """(s, t, missing) from the block ids of the two queries: s maps each
    guard of OLD to a term over NEW's guards (or None: no hint), t each
    guard of NEW to one over OLD's; missing counts the guards without ids.
    None if a query has no ids at all (no hints: the normal queries).

    Ids: `<n>` an input block; derived ones (`<id>.<role><k>`, `x`, `t`)
    blocks a pass made on a path of their own; `<a>><b>` the block
    sea-split-critical-edges put on the edge from the block of a to the
    block of b. A guard's term, from its block's ids:
      - the conjunction of the guards of the other query whose blocks hold
        one of its ids;
      - none held, an edge a>b: the conjunction of the guards of the blocks
        that hold a and b (not exact: both ends can be on a path that takes
        another edge out of a);
      - none held, only input ids: false (the other lowering erased the
        block: no path reaches it);
      - else no hint (a block the other lowering did not make, such as the
        pass / fail blocks of an assert it folded): the guard stays
        existential (see project_equiv()).
    Why the conjunction is the other side's guard: in a model of a
    BMC-form query the true guards are the blocks of one entry-to-halt path
    (an edge is pred && succ, a cond_br block has at most one successor
    whose edge condition holds, the halt block's guard is asserted); blocks
    that share an id are halves of one input block (a split) or a block
    merged into its only predecessor, on a path together. So on the path of
    an input x the term is the other query's guard of the block on its path
    of x."""
    if not o.block_ids or not n.block_ids:
        return None

    def side(src, dst):
        held = {}
        dst_guards = set(dst.guards())
        for g, ids in dst.block_ids.items():
            if g in dst_guards:
                for i in ids:
                    held.setdefault(i, set()).add(g)

        def conj(gs):
            gs = sorted(gs)
            return gs[0] if len(gs) == 1 else "(and " + " ".join(gs) + ")"

        out, missing = {}, 0
        for g in src.guards():
            ids = src.block_ids.get(g)
            if ids is None:
                missing += 1
                continue
            hs = set().union(*(held.get(i, set()) for i in ids))
            ends = [i.split(">", 1) for i in ids if ">" in i]
            if hs:
                out[g] = conj(hs)
            elif ends and all(held.get(a) and held.get(b) for a, b in ends):
                out[g] = conj(set().union(*(held[a] | held[b] for a, b in ends)))
            elif all(i.isdigit() for i in ids):
                out[g] = "false"
            else:
                out[g] = None
        return out, missing

    s, ms = side(o, n)
    t, mt = side(n, o)
    return s, t, ms + mt


class NoHints(Exception):
    """The hints cannot make a quantifier-free check (reported, then the
    quantified check runs)."""


def implies_script(a, b, hint):
    """Unsat iff A(x, g) => exists r. B(x, hint(g), r) for every x and g:
    g the guards of A, B's guards replaced by their hints (terms over A's
    guards), r B's guards without one (hint None), expanded into the
    disjunction of their 2^|r| cases."""
    gs = set(b.guards())
    for name, cmd in b.defs:
        if gs & set(tokens(cmd)):
            raise Incomparable(f"guard-in-define:{name}")
    if gs - set(hint):
        raise NoHints("guard-without-ids")
    rest = sorted(g for g in gs if hint[g] is None)
    if len(rest) > HINT_CASES_MAX:
        raise NoHints(f"cases:{len(rest)}")
    # a guard name of both queries is A's (B's is replaced)
    out = shared_decls(a, b, gs - set(a.guards()))
    named = sorted(g for g in gs if hint[g] is not None)
    out += [f"(define-fun hint!{g} () Bool {hint[g]})" for g in named]

    def rename(x):
        if isinstance(x, str):
            return ("hint!" if x in gs else "") + x
        return [rename(k) for k in x]

    body = ser(rename(parse(tokens(b.formula()))[0]))
    params = " ".join(f"(hint!{g} Bool)" for g in rest)
    out.append(f"(define-fun B__h ({params}) Bool {body})")
    cases = []
    for k in range(1 << len(rest)):
        vals = " ".join("true" if k >> j & 1 else "false" for j in range(len(rest)))
        cases.append(f"(B__h {vals})" if rest else "B__h")
    out.append(f"(assert {a.formula()})")
    return out + [f"(assert (not (or false {' '.join(cases)})))"]


def project_equiv(o, n, hints=None):
    """The scripts, (name, lines) each to be unsat, that prove
    (exists g. OLD) <=> (exists g'. NEW), g / g' the block guards of each
    query; the other symbols are matched by name.

    hints None: one quantified script (`exists`). hints = (s, t): s maps
    each guard of OLD to a term over the guards of NEW, t each guard of NEW
    to a term over the guards of OLD, or None for no term (derive_hints());
    two scripts, OLD and not NEW[g' := t(g)] (`old-new`) and NEW and not
    OLD[g := s(g')] (`new-old`), quantifier-free: a guard without a term is
    expanded by cases. Unsat says t(g) (s(g')) witnesses the existential; a
    wrong hint makes its script sat, which proves nothing either way.
    Raises NoHints when the hints cannot make such a script."""
    if hints is not None:
        s, t = hints
        return [
            ("old-new", implies_script(o, n, t)),
            ("new-old", implies_script(n, o, s)),
        ]
    skip = set(o.guards()) | set(n.guards())
    out = shared_decls(o, n, skip)
    out.append(f"(define-fun OLD__q () Bool {exists(o)})")
    out.append(f"(define-fun NEW__q () Bool {exists(n)})")
    return [("exists", out + ["(assert (distinct OLD__q NEW__q))"])]


def projected(o, n, z3, hints="auto"):
    """(answer, details) of the projected equivalence: unsat proves it.
    The Skolem-hint checks first (hints: "auto" = derive_hints(), None =
    none, or a pair (s, t)), the quantified check if one is not unsat."""
    details = []
    if hints == "auto":
        d = derive_hints(o, n)
        if d is None:
            hints = None
        elif d[2]:
            hints = None
            details.append(f"hints=incomplete:{d[2]}")
        else:
            hints = d[:2]
    if hints is not None:
        try:
            scripts = project_equiv(o, n, hints)
        except NoHints as e:
            scripts = []
            details.append(f"hints=none:{e}")
        for name, script in scripts:
            r = z3.check(script)
            if r != "unsat":
                details.append(f"hints={name}:{r.replace(' ', '_')}")
                break
        else:
            if scripts:
                return "unsat", ["by=hints"]
    ((_, script),) = project_equiv(o, n)
    return z3.check(script), ["by=exists"] + details


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


def classify(old, new, z3, project_blocks=False, hints="auto"):
    """(class, details) for two query texts; details are key=value strings.
    hints: the Skolem hints of --project-blocks (see projected())."""
    # the block-id comments are not part of the query
    old_q, new_q = strip_comments(old), strip_comments(new)
    if old_q == new_q:
        return "identical", []
    if normalized(old_q) == normalized(new_q):
        return "order", []
    try:
        o, n = Query(old), Query(new)
    except Incomparable as e:
        return "error", [f"reason={e}"]
    # symbols of one query only that it defines as numerals (addresses of
    # objects the other lowering erased): substituted, not inputs; both
    # sides are computed against the declarations before either changes
    o_names, n_names = set(o.decls), set(n.decls)
    subst = substitute_defined(o, n_names) + substitute_defined(n, o_names)
    cls, details = classify_queries(o, n, z3, project_blocks, hints)
    return cls, details + ([f"subst={subst}"] if subst else [])


def classify_queries(o, n, z3, project_blocks, hints):
    """classify() for two parsed queries that differ in text."""
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
            e, by = projected(o, n, z3, hints)
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
            old,
            new,
            Z3(a.z3, a.rlimit, a.timeout),
            a.project_blocks,
            "auto" if a.hints else None,
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
        "--hints",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="with --project-blocks: the Skolem-hint checks from the queries'"
        " block ids before the quantified check (default on)",
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
    by_exists = by_hints = 0
    inb = [0, 0, 0, 0]
    with cf.ThreadPoolExecutor(a.par) as ex:
        for job, cls, details, io, iN in ex.map(lambda f: one(f, a), files):
            counts[cls] += 1
            by_exists += cls == "equiv-sat" and "by=exists" in details
            by_hints += cls == "equiv-sat" and "by=hints" in details
            line = " ".join([job, cls] + details)
            if io and iN:
                line += f" inb old={io[0]}/{io[1]} new={iN[0]}/{iN[1]}"
                inb = [inb[0] + io[0], inb[1] + iN[0], inb[2] + io[1], inb[3] + iN[1]]
            print(line, flush=True)
    total = " ".join(f"{k.lower()}={v}" for k, v in counts.items())
    if a.project_blocks:
        total += f" by-hints={by_hints} by-exists={by_exists}"
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

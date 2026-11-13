#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

r"""encoding-diff: compare the SMT queries of two programs, two encodings of
one job, with one sea-vc.

The complement of query-diff.py: that one runs two binaries on one corpus
(a change of the tool that must not change queries); this one runs one
binary on two programs (a change of the program: a claim written another
way, a different loop shape). The question is whether the solver receives
the same query.

Each side runs `sea-vc FILE --lower --seam --smt2 --ackermannize=off
--print-lowered=...` plus its own flags. Ackermann is off so that no symbol
is named after a read's position (ack.nd.<site>_<k>.<n>): the comparison is
of the query the solver boundary receives. Symbols are renamed from sites to
the names the programs give them (from the lowered program):
  nd.<site>_<k>   -> nd.<nondet name>_<k>      (sea.nondet / sea.mem.nondet)
  obj.<segment>   -> obj.<alloca label>
An alloca label ending in a position number, `<name>.a<n>` (a program
generator that numbers its buffers by position), is renamed to
`<name>#<k>` (k-th such label of that name), so that a different number of
lines before it does not rename the object. The renaming must be one-to-one: two
sites with one name, a site without a name, or an alias onto an existing name is an
error (name collision), not a comparison. memhavoc symbols nd.<site> keep their
site number (matched by position; listed as positional=). --label-suffix changes the
pattern of the suffix (default `\.a\d+`; empty: labels are left as they are).
--alias OLD=NEW renames the left side's nondet OLD to NEW (a hand-written
skolem `kframe.cell` to the box's `box.all.kframe.0`).
--drop-left NAME removes from the left program the nondets whose name starts with NAME (a
prefix match) and every op that uses it, transitively (a helper input that
one encoding places in every job and constrains separately); reported.

Classes, first that applies:
  identical  the renamed queries are equal as text
  same-dag   the same set of top-level conjuncts as hash-consed terms (let
             bindings substituted): equal up to z3's let names and order
  equiv-parts the same symbols and common conjuncts; z3 proves the differing
             top-level conjuncts equivalent on their own (valid
             (and L-only) <=> (and R-only)): the same query up to equivalent
             terms, whatever the verdict
  equiv      both satisfiable, and z3 proves (distinct LEFT RIGHT) unsat over
             the union of the symbols: the same assignments satisfy both
             (LEFT/RIGHT: the conjunction of each query's asserts)
  both-unsat each query unsat on its own: equisatisfiable only, NOT evidence
             of equivalence
  DIFF       one sat and one unsat, or an assignment on which they differ
  unknown / error
Object addresses the placement defines ((= obj.<x> #x...) conjuncts) are
substituted on their own side (a ghost allocation moves the others).
Symbols declared on one side only (a hand skolem's cell when the other side
expands the box, a ghost allocation's base) are projected: the check is
(exists only-left. LEFT) <=> (exists only-right. RIGHT) (z3, quantified).

Run `encoding-diff.py --agent` for the short guide.
"""

import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LABEL_SUFFIX = r"\.a\d+"

AGENT = """\
encoding-diff: compare the SMT queries of two programs (two encodings of one
job) with one sea-vc.
WHY: a program rewritten without changing the claim (a hand-skolemized claim
  vs sea.box.all, reordered conjuncts, another loop shape) should give the
  solver the same query; this says whether it does, and if not, whether z3
  proves it equivalent. query-diff.py is the complement: two binaries, one
  corpus.
USAGE:
  utils/encoding-diff.py [--sea-vc build/bin/sea-vc] [--z3 <z3>]
      [--left-flags="--box-all-expand=0"] [--right-flags="..."]
      [--alias OLD=NEW ...] [--drop-left NAME ...] [--label-suffix RE]
      [--rlimit 200000000] [--timeout 300] [--strict] [--verbose]
      LEFT.mlir RIGHT.mlir [LEFT2.mlir RIGHT2.mlir ...]
  (an even number of files: consecutive pairs; flags apply to every pair)
RENAMING: both sides run sea-vc --smt2 with Ackermann off; nd.<site>_<k>
  becomes nd.<nondet name>_<k> and obj.<segment> becomes obj.<alloca label>
  (a label ending in a position number, <name>.a<n>, is renamed <name>#<k>,
  k-th such label; --label-suffix sets the pattern, empty = off). The
  renaming must be one-to-one: two sites with one name (two `"x"` nondets),
  a nondet site without a name, or an --alias target that is already a name
  is result: error (name collision / unresolved site), never a comparison.
  memhavoc symbols nd.<site> keep their site number (matched by position,
  not by name; listed in info: as positional=[..]).
REDUCED COMPARISONS (each prints a line, so identity is not mistaken for
  full identity):
  --alias OLD=NEW  renames the LEFT nondet named OLD to NEW (a hand skolem
                   to the box's index nondet); prints `alias:`.
  --drop-left NAME removes the LEFT nondets whose name starts with NAME
                   (prefix match) and every op that uses them; an error if
                   a NAME matches nothing; prints `dropped: ... ops=<n>`.
  placement        a symbol (= obj.<x> #x..) defined by a top-level conjunct
                   is replaced by the numeral on its own side (exact).
CLASSES (what each proves; LEFT/RIGHT = the conjunctions of the asserts):
  identical    the renamed queries are equal as text
  same-dag     same top-level conjuncts as terms: equal up to let names/order
  equiv-parts  same symbols; z3 proves the differing conjuncts equivalent
               on their own: same query up to equivalent terms
  equiv        not both unsat, and LEFT <=> RIGHT for every assignment (z3);
               with symbols on one side only: (exists only-left. LEFT) <=>
               (exists only-right. RIGHT)
  both-unsat   each unsat on its own: equisatisfiable only, NOT evidence
               of equivalence
  DIFF         one sat and one unsat, or a distinguishing assignment
  unknown      a z3 check gave unknown (rlimit, timeout): no proof either way
  error        sea-vc or z3 failed, a symbol has two sorts, a name collision
OUTPUT (stdout, one block per pair, then totals; key: value):
  pair: <left> <right>
  result: <class>
  info: [left=<verdict> right=<verdict>] [parts=<verdict>] asserts=<l>/<r>
        only-left=[..] only-right=[..] [positional=[nd.<site>,..]] [conjuncts=<l>/<r> differing=<l>/<r>]
        (left= / right= are z3's verdicts of each query; parts= the verdict
        of the equiv-parts check when it did not prove equivalence)
  [alias: ...] [dropped: ...]
  [model: <line>]   (DIFF by an assignment: the first lines of z3's model)
  total: identical=<n> same-dag=<n> equiv-parts=<n> equiv=<n> both-unsat=<n>
         diff=<n> unknown=<n> error=<n>
  limits: rlimit=<n> timeout=<s>
  [strict: fail both-unsat=<n> ...]   (--strict, some pair is both-unsat)
STATUS: 0 if every pair is identical / same-dag / equiv-parts / equiv /
  both-unsat; 1 if some is DIFF or error; 2 if some is unknown; 3 with
  --strict if some is both-unsat (checked in this order).
"""


def forms(text):
    """Top-level s-expressions of an SMT-LIB text (comments dropped)."""
    out, depth, start, i, n = [], 0, None, 0, len(text)
    while i < n:
        ch = text[i]
        if ch == ";":
            i = text.find("\n", i)
            i = n if i < 0 else i
            continue
        if ch == "|":
            i = text.index("|", i + 1)
        elif ch == "(":
            if depth == 0:
                start = i
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                out.append(text[start : i + 1])
        i += 1
    return out


def parse(text):
    """One s-expression -> nested lists of atoms."""
    toks = re.findall(r"\|[^|]*\||\(|\)|[^\s()|]+", text)
    stack = [[]]
    for t in toks:
        if t == "(":
            stack.append([])
        elif t == ")":
            x = stack.pop()
            stack[-1].append(x)
        else:
            stack[-1].append(t)
    return stack[0][0]


class Dag:
    """Hash-consed terms with `let` bindings substituted: two terms get the
    same id iff they are the same DAG (z3's let names and order do not count)."""

    def __init__(self):
        self.ids = {}

    def node(self, key):
        return self.ids.setdefault(key, len(self.ids))

    def term(self, e, env=None):
        env = env or {}
        while isinstance(e, list) and e and e[0] == "let":
            env = dict(env)
            new = {b[0]: self.term(b[1], env) for b in e[1]}  # parallel let
            env.update(new)
            e = e[2]
        if isinstance(e, str):
            return env[e] if e in env else self.node(("atom", e))
        return self.node(
            tuple(
                self.term(x, env)
                if isinstance(x, list)
                else (env[x] if x in env else self.node(("atom", x)))
                for x in e
            )
        )

    def conjuncts(self, asserts, subst=None):
        """The top-level conjuncts (through `and` and `let`) of the asserts, as
        ids; subst: symbol -> numeral, substituted (trivial (= c c) dropped)."""
        out = []
        and_id = self.node(("atom", "and"))
        env = {k: self.node(("atom", v)) for k, v in (subst or {}).items()}
        for a in asserts:
            out.append(self.term(parse(a), env))
        flat = []
        inv = {v: k for k, v in self.ids.items()}
        stack = list(out)
        while stack:
            t = stack.pop()
            k = inv.get(t)
            if isinstance(k, tuple) and k and k[0] == and_id:
                stack.extend(k[1:])
            elif not (
                isinstance(k, tuple)
                and len(k) == 3
                and inv[k[0]] == ("atom", "=")
                and k[1] == k[2]
            ):
                flat.append(t)
        return sorted(flat)

    def smt(self, roots):
        """define-funs t<id> for the DAG below roots (topological order)."""
        inv = {v: k for k, v in self.ids.items()}
        out, done = [], set()

        def emit(t):
            stack = [(t, False)]
            while stack:
                x, ready = stack.pop()
                if x in done:
                    continue
                k = inv[x]
                if k[0] == "atom":
                    done.add(x)
                    continue
                if inv[k[0]] == (
                    "atom",
                    "_",
                ):  # an indexed identifier: inline, no binding
                    done.add(x)
                    continue
                if not ready:
                    stack.append((x, True))
                    stack.extend((c, False) for c in k if c not in done)
                    continue
                done.add(x)
                out.append((x, text(k)))

        def text(k):  # indexed identifiers ((_ extract 7 0)) stay inline
            return (
                "("
                + " ".join(
                    inv[c][1]
                    if inv[c][0] == "atom"
                    else (text(inv[c]) if inv[inv[c][0]] == ("atom", "_") else f"t{c}")
                    for c in k
                )
                + ")"
            )

        for r in roots:
            emit(r)
        return out


def placed(dag, asserts):
    """obj.<x> -> numeral for the top-level conjuncts (= obj.<x> #x...) of a
    query (through `and` and `let`): an object address the placement defines.
    Substituted on its own side (exact: exists x. x = c and phi is phi[c/x]);
    a ghost allocation moves the placement of the objects after it, as
    query-diff.py's SUBST. An object defined by two different numerals is left
    alone."""
    defs = {}
    conj = dag.conjuncts(asserts)
    inv = {v: k for k, v in dag.ids.items()}
    for c in conj:
        k = inv[c]
        if not (isinstance(k, tuple) and len(k) == 3 and inv[k[0]] == ("atom", "=")):
            continue
        x, y = inv[k[1]], inv[k[2]]
        if x[0] != "atom" or y[0] != "atom":
            continue
        for s, n in ((x[1], y[1]), (y[1], x[1])):
            if s.startswith("|obj.") and re.fullmatch(r"#x[0-9a-f]+", n):
                defs.setdefault(s, set()).add(n)
    return {k: next(iter(v)) for k, v in defs.items() if len(v) == 1}


class Failure(Exception):
    pass


def names(lowered, suffix):
    """site -> nondet name; object segment name -> label, from --print-lowered."""
    nd, obj, seen = {}, {}, {}
    for m in re.finditer(r'sea\.(?:\w+\.)?nondet "([^"]*)" \[(\d+)\]', lowered):
        if "|" in m[1]:
            raise Failure(f"nondet name contains '|': {m[1]!r}")
        nd[m[2]] = m[1]
    for m in re.finditer(r'sea\.segment "([^"]*)" .*? label "([^"]*)"', lowered):
        if "|" in m[2]:
            raise Failure(f"label contains '|': {m[2]!r}")
        lab = re.sub(f"{suffix}(?=$|\\.)", "", m[2]) if suffix else m[2]
        seen[lab] = seen.get(lab, 0) + 1
        obj[m[1]] = lab if seen[lab] == 1 else f"{lab}#{seen[lab]}"
    return nd, obj


def drop(text, prefixes):
    """The program without the nondets whose name starts with one of prefixes
    (a prefix match) and their transitive users; (text, number of ops dropped)."""
    dead, out, n, hit = set(), [], 0, set()
    for line in text.splitlines():
        m = re.match(r'\s*(%[\w.]+) = sea\.nondet "([^"]*)"', line)
        if m:
            hit |= {p for p in prefixes if m[2].startswith(p)}
        uses = set(
            re.findall(r"%[\w.]+", line.split("=", 1)[1] if " = " in line else line)
        )
        if (m and any(m[2].startswith(p) for p in prefixes)) or (uses & dead):
            d = re.match(r"\s*(%[\w.]+) = ", line)
            if d:
                dead.add(d[1])
            n += 1
            continue
        out.append(line)
    missing = [p for p in prefixes if p not in hit]
    if missing:
        raise Failure(
            f"--drop-left {missing}: no nondet of the left program has this prefix"
        )
    return "\n".join(out) + "\n", n


def query(a, path, flags, alias, drops=()):
    with tempfile.TemporaryDirectory() as d:
        low = Path(d) / "lowered.mlir"
        nops = 0
        if drops:
            text, nops = drop(Path(path).read_text(), drops)
            path = Path(d) / "dropped.mlir"
            path.write_text(text)
        cmd = [
            a.sea_vc,
            str(path),
            "--lower",
            "--seam",
            "--smt2",
            "--ackermannize=off",
            f"--print-lowered={low}",
            *flags,
        ]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=a.timeout)
        except subprocess.TimeoutExpired:
            raise Failure(f"sea-vc timed out on {path}") from None
        if r.returncode or not low.exists():
            raise Failure(f"sea-vc failed on {path}: {r.stderr.strip()[-400:]}")
        nd, obj = names(low.read_text(), a.label_suffix)
    stats = {
        k: v
        for k, v in re.findall(
            r"^(vc\.(?:box-all|expand-memmove)): (.*)$", r.stderr + r.stdout, re.M
        )
    }
    fs = forms(r.stdout)
    fin = {}  # original symbol -> final name

    def ren(s):
        def nd_name(m):
            name = nd.get(m[1])
            if name is None:
                raise Failure(
                    f"unresolved site nd.{m[1]}_{m[2]}: no named nondet at site {m[1]}"
                )
            f = f"|nd.{alias.get(name, name)}_{m[2]}|"
            fin[m[0]] = f
            return f

        def obj_name(m):
            f = f"|obj.{obj.get(m[1], m[1])}|"
            fin[m[0]] = f
            return f

        s = re.sub(r"\bnd\.(\d+)_(\d+)\b", nd_name, s)
        return re.sub(r"\bobj\.([A-Za-z_]+\.\d+)\b", obj_name, s)

    decls = [ren(f) for f in fs if f.startswith("(declare-fun")]
    asserts = [
        ren(f)[len("(assert") : -1].strip() for f in fs if f.startswith("(assert")
    ]
    # the renaming must be one-to-one: two sites with one name would merge
    # two inputs into one symbol
    rev = {}
    for o, f in fin.items():
        rev.setdefault(f, set()).add(o)
    for f, os_ in rev.items():
        if len(os_) > 1:
            raise Failure(f"name collision: {sorted(os_)} -> {f}")
    others = {re.match(r"\(declare-fun (\|[^|]*\||\S+)", x)[1] for x in decls} - set(
        fin.values()
    )
    for f, os_ in rev.items():
        if f in others:
            raise Failure(
                f"name collision: {sorted(os_)} and a symbol of that name -> {f}"
            )
    positional = sorted(
        x
        for x in (re.match(r"\(declare-fun (\S+)", d)[1] for d in decls)
        if re.fullmatch(r"nd\.\d+", x)
    )
    return decls, asserts, stats, nops, positional


def compare(a, left, right):
    """-> (class, extra info, model text or '')."""
    alias = dict(x.split("=", 1) for x in a.alias)
    ld, la, ls, nops, lpos = query(
        a, left, shlex.split(a.left_flags), alias, a.drop_left
    )
    rd, ra, rs, _, rpos = query(a, right, shlex.split(a.right_flags), {})
    notes = []
    if alias:
        notes.append(
            "alias: left nondet " + " ".join(f"{k}->{v}" for k, v in alias.items())
        )
    if a.drop_left:
        notes.append(f"dropped: left names={a.drop_left} ops={nops} (prefix match)")
    sym = lambda ds: {re.match(r"\(declare-fun (\|[^|]*\||\S+)", x)[1]: x for x in ds}
    lsym, rsym = sym(ld), sym(rd)
    only_l, only_r = sorted(set(lsym) - set(rsym)), sorted(set(rsym) - set(lsym))
    info = f"asserts={len(la)}/{len(ra)} only-left={only_l} only-right={only_r}"
    pos = sorted(set(lpos) | set(rpos))
    if pos:
        info += f" positional={pos}"
    if a.verbose:
        info += f" left={ls} right={rs}"
    if la == ra and lsym == rsym:
        return "identical", info, "", notes
    dag = Dag()
    lsub, rsub = placed(dag, la), placed(dag, ra)
    lc, rc = dag.conjuncts(la, lsub), dag.conjuncts(ra, rsub)
    if lsym == rsym and lc == rc:
        return "same-dag", info, "", notes  # same terms, other let names / order
    dl, dr = sorted(set(lc) - set(rc)), sorted(set(rc) - set(lc))
    info += f" conjuncts={len(lc)}/{len(rc)} differing={len(dl)}/{len(dr)}"
    parts_goal = None
    if lsym == rsym and dl and dr:
        # the differing conjuncts alone, unconditionally (nothing from the
        # common ones): (and dl) <=> (and dr) valid -> equiv-parts
        goal = f"(distinct (and true {' '.join(f't{x}' for x in dl)}) (and true {' '.join(f't{x}' for x in dr)}))"
        for x, e in reversed(dag.smt(dl + dr)):
            goal = f"(let ((t{x} {e})) {goal})"
        parts_goal = goal
    conflict = [s for s in set(lsym) & set(rsym) if lsym[s] != rsym[s]]
    if conflict:
        raise Failure(f"sort conflict {conflict}")

    def proj(body, only, decls):
        """exists the one-sided symbols. body (they are bound, not free)."""
        if not only:
            return body
        decl_re = re.compile(r"\(declare-fun \S+ \(\) (.*)\)$", re.S)
        sorts = []
        for x in only:
            m = decl_re.match(decls[x])
            if not m:
                raise Failure(f"cannot read the declaration of {x}")
            sorts.append(f"({x} {m[1]})")
        bs = " ".join(sorts)
        return f"(exists ({bs}) {body})"

    common = {
        k: v for k, v in {**lsym, **rsym}.items() if k not in only_l and k not in only_r
    }

    def sub(body, m):
        return (
            f"(let ({' '.join(f'({k} {v})' for k, v in m.items())}) {body})"
            if m
            else body
        )

    defs = list(common.values()) + [
        f"(define-fun LEFT () Bool {proj(sub('(and true ' + ' '.join(la) + ')', lsub), only_l, lsym)})",
        f"(define-fun RIGHT () Bool {proj(sub('(and true ' + ' '.join(ra) + ')', rsub), only_r, rsym)})",
    ]

    def z3(goal, model=False):
        with tempfile.NamedTemporaryFile("w", suffix=".smt2", delete=False) as f:
            f.write(
                "\n".join(
                    defs
                    + [f"(assert {goal})", "(check-sat)"]
                    + (["(get-model)"] if model else [])
                )
            )
        try:
            r = subprocess.run(
                [a.z3, f"rlimit={a.rlimit}", f"-T:{a.timeout}", f.name],
                capture_output=True,
                text=True,
                timeout=a.timeout + 30,
            )
        except subprocess.TimeoutExpired:
            return "unknown", ""
        finally:
            os.unlink(f.name)
        first = (r.stdout.split() or ["error"])[0]
        if first not in ("sat", "unsat"):
            # the verdict is the first line; a later (error ...) is get-model
            # after unknown, not a failure
            first = (
                "unknown" if first in ("unknown", "timeout", "canceled") else "error"
            )
        return first, r.stdout

    pr = z3(parts_goal) if parts_goal else None
    if pr and pr[0] != "unsat":
        info += f" parts={pr[0]}"
    if pr and pr[0] == "unsat":
        return "equiv-parts", info, "", notes  # same query up to equivalent conjuncts
    sl, sr = z3("LEFT")[0], z3("RIGHT")[0]
    if sl == sr == "unsat":
        return "both-unsat", info, "", notes
    info = f"left={sl} right={sr} " + info
    if "error" in (sl, sr):
        raise Failure(f"z3 failed {info}")
    if {sl, sr} == {"sat", "unsat"}:
        return "DIFF", info, "", notes
    first, out = z3("(distinct LEFT RIGHT)", model=True)
    if first == "unsat":
        return "equiv", info, "", notes
    if first == "sat":
        return "DIFF", info, out[:3000], notes
    if first == "error":
        raise Failure(f"z3 failed on (distinct LEFT RIGHT) {info}")
    return "unknown", info, "", notes


def main():
    if "--agent" in sys.argv:
        print(AGENT, end="")
        return 0
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n", 1)[0],
        epilog="--agent: the short guide (classes, output, exit codes)",
    )
    ap.add_argument("files", nargs="+", help="LEFT RIGHT [LEFT RIGHT ...]")
    ap.add_argument("--sea-vc", default=str(ROOT / "build" / "bin" / "sea-vc"))
    ap.add_argument(
        "--left-flags", default="", help="extra sea-vc flags of the left side"
    )
    ap.add_argument(
        "--right-flags", default="", help="extra sea-vc flags of the right side"
    )
    ap.add_argument(
        "--alias",
        action="append",
        default=[],
        metavar="OLD=NEW",
        help="rename the left side's nondet OLD to NEW",
    )
    ap.add_argument(
        "--drop-left",
        action="append",
        default=[],
        metavar="NAME",
        help="remove the left nondet NAME (prefix) and its transitive users",
    )
    ap.add_argument(
        "--label-suffix",
        default=LABEL_SUFFIX,
        help="regex of a position number ending an alloca label, removed"
        " and replaced by #k (empty: off; default: %(default)s)",
    )
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
        "--verbose",
        action="store_true",
        help="add the vc.box-all / vc.expand-memmove stats of both sides to info:",
    )
    ap.add_argument(
        "--strict", action="store_true", help="exit 3 when some pair is only both-unsat"
    )
    a = ap.parse_args()
    for x in a.alias:
        if "=" not in x:
            ap.error(f"--alias {x}: expected OLD=NEW")
    if len(a.files) % 2:
        ap.error("an even number of files: LEFT RIGHT pairs")
    counts = dict.fromkeys(
        (
            "identical",
            "same-dag",
            "equiv-parts",
            "equiv",
            "both-unsat",
            "diff",
            "unknown",
            "error",
        ),
        0,
    )
    for left, right in zip(a.files[::2], a.files[1::2]):
        print(f"pair: {left} {right}")
        try:
            cls, info, model, notes = compare(a, left, right)
        except Failure as e:
            cls, info, model, notes = "error", str(e), "", []
        counts[cls.lower()] += 1
        print(f"result: {cls}")
        print(f"info: {info}")
        for n in notes:
            print(n)
        for line in model.splitlines():
            print(f"model: {line}")
    print("total: " + " ".join(f"{k}={v}" for k, v in counts.items()))
    print(f"limits: rlimit={a.rlimit} timeout={a.timeout}")
    c = counts
    if c["diff"] or c["error"]:
        return 1
    if c["unknown"]:
        return 2
    if a.strict and c["both-unsat"]:
        print(
            f"strict: fail both-unsat={c['both-unsat']} (verdict only, no evidence about the change)"
        )
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())

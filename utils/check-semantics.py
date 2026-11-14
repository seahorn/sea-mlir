#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Differential check of sea op semantics against z3.

For every pure bit-vector op, several widths, edge values and random values,
z3 computes the expected result with `(simplify ...)` on the SMT-LIB term
(the reference). Checked against it:
- sea-run, interpreting a generated program with --trace (the interpreter's
  op semantics);
- with --sea-expr-eval, the same term built as an Expr and solved with z3
  and with Yices (and with Bitwuzla under --bitwuzla) through sea's
  marshalling (what the VC generator relies on);
- with --sea-opt, sea-run on the program after sea-expand-intrinsics (the
  expansion of intrinsic ops into core ops);
- with --sea-opt and --fold, the op folders (sea-opt --sccp): every case
  has constant operands, so every result must fold to a sea.const, whose
  value is compared (a result that does not fold is a mismatch).
Intrinsic ops (min/max, abs, bswap, ctpop, funnel shifts, overflow bits) are
checked against reference terms that are their LLVM LangRef / SMT-LIB 2.7
definitions written in SMT-LIB 2.6 (wide arithmetic, shifts of the
concatenation), deliberately not the formulas the expansion uses (z3 4.12
has no bvuaddo & co). The VC encodes only mul overflow natively (z3, and
Bitwuzla): Expr checks that encoding with those; the rest reach the VC only
expanded.
`sea.addr.from_bits` / `to_bits` (docs/design/PtrSort.md D2) are checked as
the pair to_bits(from_bits x) = x at widths 1..64, also without a folder.
`sea.ptr.index` (docs/design/PtrIndex.md) is checked on address bases of
widths 1..64: its base is made by `sea.addr.from_int` and its result read
by `sea.addr.to_int`. It has no folder (address ops never fold: provenance),
so it is excluded from the fold check (maintainer-approved exception); the
--sea-opt run also applies sea-lower-ptr-index, so its lowering is checked.
Any difference is a bug in the checked side.

ORACLE (see test/README.md): changing what it accepts needs sign-off.
"""

import argparse
import os
import random
import re
import subprocess
import sys
import tempfile

BINOPS = {
    "add": "bvadd",
    "sub": "bvsub",
    "mul": "bvmul",
    "udiv": "bvudiv",
    "sdiv": "bvsdiv",
    "urem": "bvurem",
    "srem": "bvsrem",
    "and": "bvand",
    "or": "bvor",
    "xor": "bvxor",
    "shl": "bvshl",
    "lshr": "bvlshr",
    "ashr": "bvashr",
}
ICMPS = {
    "eq": "=",
    "ne": "distinct",
    "slt": "bvslt",
    "sle": "bvsle",
    "sgt": "bvsgt",
    "sge": "bvsge",
    "ult": "bvult",
    "ule": "bvule",
    "ugt": "bvugt",
    "uge": "bvuge",
}
WIDTHS = [1, 2, 3, 7, 8, 16, 33, 64, 65, 128]
MINMAX = {"umin": "bvult", "umax": "bvugt", "smin": "bvslt", "smax": "bvsgt"}


def ovf_ref(op, a, b, w):
    """SMT-LIB 2.7 / LangRef: the result on unbounded integers is not
    representable in w bits. Computed in 2w+2 bits, where it is exact.
    Division by 0 is not an overflow (SMT-LIB 2.7 bvsdivo; it is its own
    check): excluded, as bvsdiv by 0 gives -1 or 1, and 1 does not fit in a
    signed i1. `b` is unused for neg."""
    W, ext = 2 * w + 2, "zero_extend" if op[0] == "u" else "sign_extend"
    x, y = f"((_ {ext} {W - w}) {bv(a, w)})", f"((_ {ext} {W - w}) {bv(b, w)})"
    if op == "neg":
        r = f"(bvneg ((_ sign_extend {W - w}) {bv(a, w)}))"
        ext = "sign_extend"
    else:
        smt = {"add": "bvadd", "sub": "bvsub", "mul": "bvmul", "div": "bvsdiv"}[op[1:4]]
        r = f"({smt} {x} {y})"
    back = f"((_ {ext} {W - w}) ((_ extract {w - 1} 0) {r}))"
    if op == "sdiv":
        return f"(and (distinct {bv(b, w)} {bv(0, w)}) (distinct {back} {r}))"
    return f"(distinct {back} {r})"


def funnel_ref(left, hi, lo, s, w):
    """LangRef: shift hi:lo by s mod w; fshl keeps the high half, fshr the
    low half."""
    sh = f"((_ zero_extend {w}) (bvurem {bv(s, w)} {bv(w % (1 << w), w)}))"
    cat = f"(concat {bv(hi, w)} {bv(lo, w)})"
    if left:
        return f"((_ extract {2 * w - 1} {w}) (bvshl {cat} {sh}))"
    return f"((_ extract {w - 1} 0) (bvlshr {cat} {sh}))"


def bswap_ref(a, w):
    """LangRef: byte i of the input is byte n-1-i of the result."""
    return (
        "(concat "
        + " ".join(
            f"((_ extract {8 * i + 7} {8 * i}) {bv(a, w)})" for i in range(w // 8)
        )
        + ")"
    )


def ctpop_ref(a, w):
    """LangRef: the number of 1 bits, as a w-bit sum of the extracted bits."""
    bits = [f"((_ extract {i} {i}) {bv(a, w)})" for i in range(w)]
    if w == 1:
        return bits[0]
    terms = [f"((_ zero_extend {w - 1}) {t})" for t in bits]
    return "(bvadd " + " ".join(terms) + ")"


def abs_ref(a, w):
    """LangRef (is_int_min_poison false): the absolute value of the signed
    reading, computed exactly in w+1 bits, then its low w bits (the
    minimal signed value is its own result)."""
    x = f"((_ sign_extend 1) {bv(a, w)})"
    return f"((_ extract {w - 1} 0) (ite (bvslt {x} {bv(0, w + 1)}) (bvneg {x}) {x}))"


def edge_values(w):
    m = (1 << w) - 1
    vals = {
        0,
        1,
        2,
        m,
        m - 1,
        1 << (w - 1),
        (1 << (w - 1)) - 1,
        (1 << (w - 1)) + 1,
        w,
        w - 1,
        w + 1,
    }
    return sorted(v & m for v in vals)


def bv(v, w):
    return f"(_ bv{v} {w})"


def parse_z3(tok):
    if tok == "true":
        return 1
    if tok == "false":
        return 0
    if tok.startswith("#x"):
        return int(tok[2:], 16)
    if tok.startswith("#b"):
        return int(tok[2:], 2)
    raise ValueError(f"unexpected z3 output {tok!r}")


def build_cases(rng, nrandom):
    """Returns (op, width, [(value, width)], operand template, smt, expr_line)
    cases; expr_line is the sea-expr-eval input for the same term."""
    cases = []
    for w in WIDTHS:
        vals = edge_values(w) + [rng.getrandbits(w) for _ in range(nrandom)]
        pairs = [(a, b) for a in edge_values(w) for b in edge_values(w)]
        pairs += [(rng.choice(vals), rng.choice(vals)) for _ in range(nrandom)]
        for a, b in pairs:
            for op, smt in BINOPS.items():
                cases.append(
                    (
                        f"sea.{op}",
                        w,
                        [(a, w), (b, w)],
                        f"{{0}}, {{1}} : i{w}",
                        f"({smt} {bv(a, w)} {bv(b, w)})",
                        f"bin {smt} {w} {a} {b}",
                    )
                )
            for op, smt in MINMAX.items():
                cases.append(
                    (
                        f"sea.{op}",
                        w,
                        [(a, w), (b, w)],
                        f"{{0}}, {{1}} : i{w}",
                        f"(ite ({smt} {bv(a, w)} {bv(b, w)}) {bv(a, w)} {bv(b, w)})",
                        None,
                    )
                )
            for op in ("uadd", "sadd", "usub", "ssub", "umul", "smul", "sdiv"):
                # the VC encodes mul overflow natively for z3 and Bitwuzla:
                # check that encoding too (Yices always gets the expansion)
                expr = (
                    (("z3", "bitwuzla"), f"mulo {op[0]} {w} {a} {b}")
                    if op.endswith("mul")
                    else None
                )
                cases.append(
                    (
                        f"sea.{op}o",
                        w,
                        [(a, w), (b, w)],
                        f"{{0}}, {{1}} : i{w}",
                        ovf_ref(op, a, b, w),
                        expr,
                    )
                )
            s = rng.choice(edge_values(w) + [rng.getrandbits(w)])
            for left, op in ((True, "fshl"), (False, "fshr")):
                cases.append(
                    (
                        f"sea.{op}",
                        w,
                        [(a, w), (b, w), (s, w)],
                        f"{{0}}, {{1}}, {{2}} : i{w}",
                        funnel_ref(left, a, b, s, w),
                        None,
                    )
                )
            for pred, smt in ICMPS.items():
                cases.append(
                    (
                        "sea.icmp",
                        w,
                        [(a, w), (b, w)],
                        f"{pred}, {{0}}, {{1}} : i{w}",
                        f"({smt} {bv(a, w)} {bv(b, w)})",
                        f"cmp {smt} {w} {a} {b}",
                    )
                )
        for a in vals:
            for w2 in (w + 1, w + 7, 2 * w):
                cases.append(
                    (
                        "sea.zext",
                        w,
                        [(a, w)],
                        f"{{0}} : i{w} to i{w2}",
                        f"((_ zero_extend {w2 - w}) {bv(a, w)})",
                        f"zext {w} {w2} {a}",
                    )
                )
                cases.append(
                    (
                        "sea.sext",
                        w,
                        [(a, w)],
                        f"{{0}} : i{w} to i{w2}",
                        f"((_ sign_extend {w2 - w}) {bv(a, w)})",
                        f"sext {w} {w2} {a}",
                    )
                )
            for w2 in sorted({1, w // 2, w - 1} - {0}):
                if w2 < w:
                    cases.append(
                        (
                            "sea.trunc",
                            w,
                            [(a, w)],
                            f"{{0}} : i{w} to i{w2}",
                            f"((_ extract {w2 - 1} 0) {bv(a, w)})",
                            f"trunc {w} {w2} {a}",
                        )
                    )
            # concat with a 1..8-bit low part; extract of every 1..w-bit slice
            # starting at a random bit
            w2 = rng.randint(1, 8)
            lo = rng.getrandbits(w2)
            cases.append(
                (
                    "sea.concat",
                    w,
                    [(a, w), (lo, w2)],
                    f"{{0}}, {{1}} : i{w}, i{w2} -> i{w + w2}",
                    f"(concat {bv(a, w)} {bv(lo, w2)})",
                    f"concat {w} {w2} {a} {lo}",
                )
            )
            low = rng.randint(0, w - 1)
            ew = rng.randint(1, w - low)
            cases.append(
                (
                    "sea.extract",
                    w,
                    [(a, w)],
                    f"{{0}}[{low}] : i{w} -> i{ew}",
                    f"((_ extract {low + ew - 1} {low}) {bv(a, w)})",
                    f"extract {w} {low} {ew} {a}",
                )
            )
            cases.append(
                (
                    "sea.nego",
                    w,
                    [(a, w)],
                    f"{{0}} : i{w}",
                    ovf_ref("neg", a, 0, w),
                    None,
                )
            )
            if w % 16 == 0:
                cases.append(
                    ("sea.bswap", w, [(a, w)], f"{{0}} : i{w}", bswap_ref(a, w), None)
                )
            cases.append(
                ("sea.ctpop", w, [(a, w)], f"{{0}} : i{w}", ctpop_ref(a, w), None)
            )
            cases.append(("sea.abs", w, [(a, w)], f"{{0}} : i{w}", abs_ref(a, w), None))
            c = rng.getrandbits(1)
            b = rng.getrandbits(w)
            cases.append(
                (
                    "sea.select",
                    w,
                    [(c, 1), (a, w), (b, w)],
                    f"{{0}}, {{1}}, {{2}} : i{w}",
                    f"(ite (= {bv(c, 1)} #b1) {bv(a, w)} {bv(b, w)})",
                    f"ite {w} {c} {a} {b}",
                )
            )
    # sea.ptr.index on an address base: base + sum sext(i_k)*s_k + c
    for w in [x for x in WIDTHS if x <= 64]:
        lo, hi = -(1 << (w - 1)), (1 << (w - 1)) - 1

        def sconst(lo=lo, hi=hi):
            return rng.randint(lo, hi)

        def stride():
            while True:
                v = sconst()
                if v:
                    return v

        for _ in range(4 + nrandom):
            base = rng.getrandbits(w)
            m = rng.randint(1, 3)
            idx = []
            for _ in range(m):
                iw = rng.randint(1, w)
                idx.append((rng.getrandbits(iw), iw, stride()))
            c = sconst()
            terms = []
            for v, iw, st in idx:
                x = bv(v, iw) if iw == w else f"((_ sign_extend {w - iw}) {bv(v, iw)})"
                terms.append(f"(bvmul {x} {bv(st % (1 << w), w)})")
            ref = bv(base, w)
            for t in terms + [bv(c % (1 << w), w)]:
                ref = f"(bvadd {ref} {t})"
            names = ", ".join("{%d}" % (k + 1) for k in range(m))
            strides = ", ".join(str(st) for _, _, st in idx)
            types = ", ".join(f"i{iw}" for _, iw, _ in idx)
            cases.append(
                (
                    "sea.ptr.index",
                    w,
                    [(base, w)] + [(v, iw) for v, iw, _ in idx],
                    f"{{0}} [{names}] strides [{strides}] offset {c} "
                    f": !sea.addr<{w}>, {types}",
                    ref,
                    None,
                )
            )
    # sea.addr.from_bits / to_bits (docs/design/PtrSort.md D2): a pointer's
    # own bits reassembled / split; checked as the pair, to_bits(from_bits
    # x) = x (the case's line has both ops, see emit_case)
    for w in range(1, 65):
        a = rng.choice(edge_values(w) + [rng.getrandbits(w)])
        cases.append(
            (
                "sea.addr.from_bits",
                w,
                [(a, w)],
                f"{{0}} : i{w} to !sea.addr<{w}>",
                bv(a, w),
                None,
            )
        )
    return cases


# ops without a folder by design (address arithmetic keeps provenance)
NO_FOLD = {"sea.ptr.index", "sea.addr.from_bits"}


def emit_case(f, i, case):
    """Writes case i (operand constants, then the op on one line); returns
    the number of lines written. A sea.ptr.index case makes its base an
    address and reads its result as an integer, on the op's line, so the
    last value traced there is the result's bits."""
    name, w, operands, template = case[:4]
    names, lines = [], 0
    for j, (v, ow) in enumerate(operands):
        names.append(f"%v{i}_{j}")
        f.write(f"  {names[-1]} = sea.const {v} : i{ow}\n")
        lines += 1
    if name == "sea.addr.from_bits":
        f.write(
            f"  %r{i}_a = {name} {template.format(*names)} "
            f"%r{i} = sea.addr.to_bits %r{i}_a : !sea.addr<{w}> to i{w}\n"
        )
        return lines + 1
    if name == "sea.ptr.index":
        f.write(f"  %v{i}_a = sea.addr.from_int {names[0]} : i{w} to !sea.addr<{w}>\n")
        lines += 1
        names[0] = f"%v{i}_a"
        f.write(
            f"  %r{i}_a = {name} {template.format(*names)} "
            f"%r{i} = sea.addr.to_int %r{i}_a : !sea.addr<{w}> to i{w}\n"
        )
    else:
        f.write(f"  %r{i} = {name} {template.format(*names)}\n")
    return lines + 1


def result_width(case):
    """Width of a case's result: i1 for predicates, the written result type
    for casts, concat and extract, else the operand width."""
    name, w, _, template = case[:4]
    if name in ("sea.icmp", "sea.nego") or re.fullmatch(r"sea\.\w+o", name):
        return 1
    m = re.search(r"(?:to|->) i(\d+)$", template)
    return int(m.group(1)) if m else w


def run_folded(args, tmp, cases):
    """The value of each case after sea-opt --sccp, or None if it did not
    fold. Each result is observed by `r ^ obs`, with `obs` a nondet (which
    does not fold), so sccp must replace r by a constant operand of the
    observer; that operand is read from the printed program."""
    path = os.path.join(tmp, "f.mlir")
    keep = [i for i, c in enumerate(cases) if c[0] not in NO_FOLD]
    cases = [cases[i] for i in keep]
    obs_lines = []
    with open(path, "w") as f:
        f.write("sea.program @fold {\n")
        line = 2
        obs = {}
        for c in cases:
            w = result_width(c)
            if w not in obs:
                obs[w] = f"%obs{w}"
                f.write(f'  %obs{w} = sea.nondet "obs" [{len(obs)}] : i{w}\n')
                line += 1
        for i, (name, _, operands, template, _, _) in enumerate(cases):
            names = []
            for j, (v, w) in enumerate(operands):
                names.append(f"%v{i}_{j}")
                f.write(f"  {names[-1]} = sea.const {v} : i{w}\n")
                line += 1
            f.write(f"  %r{i} = {name} {template.format(*names)}\n")
            w = result_width(cases[i])
            f.write(f"  %o{i} = sea.xor %r{i}, {obs[w]} : i{w}\n")
            line += 2
            obs_lines.append(line - 1)
        f.write("  sea.halt\n}\n")
    opt = subprocess.run(
        [
            args.sea_opt,
            path,
            "--sccp",
            "--mlir-print-debuginfo",
            "--mlir-print-local-scope",
        ],
        capture_output=True,
        text=True,
    )
    if opt.returncode != 0:
        sys.exit(f"sea-opt --sccp failed:\n{opt.stderr}")
    consts, observed = {}, {}
    for t in opt.stdout.splitlines():
        m = re.match(
            r"\s*(%\w+) = sea\.const (true|false|-?\d+)"
            r"(?: : i(\d+))? loc",
            t,
        )
        if m:
            v = {"true": 1, "false": 0}.get(m.group(2))
            if v is None:
                v = int(m.group(2)) & ((1 << int(m.group(3))) - 1)
            consts[m.group(1)] = v
            continue
        m = re.match(
            r"\s*%\w+ = sea\.xor (%\w+), %\w+ : i\d+ "
            r'loc\("[^"]*":(\d+):\d+\)',
            t,
        )
        if m:
            observed[int(m.group(2))] = m.group(1)
    return {keep[k]: consts.get(observed.get(l)) for k, l in enumerate(obs_lines)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sea-run", default="sea-run")
    ap.add_argument("--z3", default="z3")
    ap.add_argument("--sea-expr-eval", help="also check Expr -> z3 and Expr -> Yices")
    ap.add_argument(
        "--no-yices",
        action="store_true",
        help="skip Expr -> Yices (sea-expr-eval built without Yices)",
    )
    ap.add_argument(
        "--bitwuzla",
        action="store_true",
        help="also check Expr -> Bitwuzla (needs --sea-expr-eval built with Bitwuzla)",
    )
    ap.add_argument("--sea-opt", help="also check sea-run after sea-expand-intrinsics")
    ap.add_argument(
        "--fold",
        action="store_true",
        help="also check the op folders (sea-opt --sccp); needs --sea-opt",
    )
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument(
        "--random", type=int, default=10, help="random values/pairs per width"
    )
    args = ap.parse_args()
    # Tools run from a temporary directory: make relative paths absolute.
    for attr in ("sea_run", "sea_expr_eval", "sea_opt", "z3"):
        v = getattr(args, attr)
        if v and os.sep in v:
            setattr(args, attr, os.path.abspath(v))

    rng = random.Random(args.seed)
    cases = build_cases(rng, args.random)

    with tempfile.TemporaryDirectory() as tmp:
        smt = os.path.join(tmp, "q.smt2")
        with open(smt, "w") as f:
            for case in cases:
                f.write(f"(simplify {case[4]})\n")
        out = subprocess.run(
            [args.z3, smt], cwd=tmp, capture_output=True, text=True, check=True
        ).stdout.split()
        if len(out) != len(cases):
            sys.exit(f"z3 returned {len(out)} results for {len(cases)} cases")
        expected = [parse_z3(t) for t in out]

        prog = os.path.join(tmp, "p.mlir")
        result_lines = []  # the source line of each case's result
        with open(prog, "w") as f:
            f.write("sea.program @semantics {\n")
            line = 2
            for i, case in enumerate(cases):
                n = emit_case(f, i, case)
                result_lines.append(line + n - 1)
                line += n
            f.write("  sea.halt\n}\n")

        def run_traced(what, path):
            """The value of each case: the last value traced at its result
            line (an expanded op keeps the location of the op it replaces,
            and the op computing the result is created last)."""
            run = subprocess.run(
                [args.sea_run, path, "--trace", "--max-steps=100000000"],
                capture_output=True,
                text=True,
            )
            if run.returncode != 0:
                sys.exit(f"{what} failed:\n{run.stdout}{run.stderr}")
            at = {}
            for t in run.stdout.splitlines():
                m = re.match(r'value: \S+ loc\("[^"]*":(\d+):\d+\) .* = (\d+)$', t)
                if m:
                    at[int(m.group(1))] = int(m.group(2))
            missing = [l for l in result_lines if l not in at]
            if missing:
                sys.exit(f"{what}: no traced value at lines {missing[:5]}")
            return [at[l] for l in result_lines]

        checked = {"sea-run": run_traced("sea-run", prog)}
        if args.sea_opt:
            expanded = os.path.join(tmp, "x.mlir")
            opt = subprocess.run(
                [
                    args.sea_opt,
                    prog,
                    "--sea-expand-intrinsics",
                    "--sea-lower-ptr-index",
                    "-o",
                    expanded,
                    "--mlir-print-debuginfo",
                ],
                capture_output=True,
                text=True,
            )
            if opt.returncode != 0:
                sys.exit(f"sea-opt failed:\n{opt.stderr}")
            checked["expanded"] = run_traced("sea-run (expanded)", expanded)
            if args.fold:
                checked["folded"] = run_folded(args, tmp, cases)

        if args.sea_expr_eval:
            terms = os.path.join(tmp, "terms.txt")
            solvers = (
                ["z3"]
                + ([] if args.no_yices else ["yices"])
                + (["bitwuzla"] if args.bitwuzla else [])
            )
            for solver in solvers:
                # an Expr line is a term for every solver, or (solvers, term)
                expr_cases = [
                    i
                    for i, c in enumerate(cases)
                    if c[5] and (isinstance(c[5], str) or solver in c[5][0])
                ]
                with open(terms, "w") as f:
                    for i in expr_cases:
                        c = cases[i][5]
                        f.write((c if isinstance(c, str) else c[1]) + "\n")
                ev = subprocess.run(
                    [args.sea_expr_eval, f"--solver={solver}", terms],
                    cwd=tmp,
                    capture_output=True,
                    text=True,
                )
                if ev.returncode != 0:
                    sys.exit(f"sea-expr-eval --solver={solver} failed:\n{ev.stderr}")
                vals = [int(v) for v in ev.stdout.split()]
                if len(vals) != len(expr_cases):
                    sys.exit(
                        f"sea-expr-eval --solver={solver} returned "
                        f"{len(vals)} values for {len(expr_cases)} cases"
                    )
                checked[f"expr-{solver}"] = dict(zip(expr_cases, vals))

    bad = 0
    total = 0
    for name, vals in checked.items():
        if isinstance(vals, list):
            vals = dict(enumerate(vals))
        for i, g in vals.items():
            case, e = cases[i], expected[i]
            total += 1
            if e != g:
                bad += 1
                if bad <= 20:
                    print(f"MISMATCH {name} {case[4]}: z3={e} {name}={g}")
    print(
        f"checked {len(cases)} cases, {total} results from "
        f"{len(checked)} implementations ({', '.join(checked)}), "
        f"{bad} mismatches"
    )
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

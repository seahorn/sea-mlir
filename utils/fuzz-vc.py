#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Differential fuzzing of the VC generator against the interpreter.

Generates random loop-free sea programs (straight-line code, nested
diamonds with block arguments (structs too), structs (create, nondet,
nested, extract, insert, select), empty arms that make critical edges and
equal cond_br targets, assumes, nondets of several widths,
memory regions with 3-bit addresses so that aliasing is common: in-place
stores and loads of one and two cells (i16, little-endian), memset /
memmove (within and across regions) / memhavoc with
constant lengths 0-3 (ranges overlap and wrap), memset with symbolic
lengths too, loads of the current
version, address ops, memory versions merged at joins; one assert at the
exit) and checks, for each:
  1. z3 and Yices (and Bitwuzla, with --bitwuzla) agree on sat/unsat;
  2. sat: the model replays in the interpreter to assert-failed with every
     computed value equal to the model's (sea-vc exit code 0, else 2), and
     the counterexample's inputs also fail the original, unlowered program
     (validates the lowering passes: splitting wide accesses, expanding
     intrinsics): sea-vc --replay-original must report match=yes (it fails
     the same assertion; exit 6 if not) or match=undetermined (counted:
     `replay-original: undetermined=<n>`), and sea-run on the inputs
     sea-vc prints must fail it too;
  3. unsat: random interpreter runs never fail the assertion (a failing run
     would be a bogus unsat, the dangerous direction);
  4. the original program and its lowering (sea-opt --sea-vc-lower, what
     sea-vc --lower runs) agree, on the same random inputs, on the status
     and on the value of every op of the original: the lowered program's
     last value at the op's location (a lowered op keeps the location of
     the op it replaces and computes its result last). The values sccp
     folds lose their location: they are compared on the lowering without
     sccp (fold=false), and through their uses on the full one.

ORACLE (see test/README.md): changing what it accepts needs sign-off.
"""

import argparse
import os
import random
import re
import subprocess
import sys
import tempfile

WIDTHS = [1, 3, 8, 16]
BINOPS = [
    "add",
    "sub",
    "mul",
    "udiv",
    "sdiv",
    "urem",
    "srem",
    "and",
    "or",
    "xor",
    "shl",
    "lshr",
    "ashr",
]
# Intrinsic ops: the VC sees them only after sea-expand-intrinsics (--lower).
MINMAX = ["umin", "umax", "smin", "smax"]
OVF = ["uaddo", "saddo", "usubo", "ssubo", "umulo", "smulo", "sdivo"]
WRAP = {"add", "sub", "mul", "shl"}  # ops that take nsw / nuw
PREDS = ["eq", "ne", "slt", "sle", "sgt", "sge", "ult", "ule", "ugt", "uge"]
AW = 3  # address width: 8 addresses, so symbolic addresses alias often
ADDR = f"!sea.addr<{AW}>"
MEM = f"!sea.mem<{ADDR}, i8>"


class Gen:
    def __init__(self, rng, memory=True):
        self.rng = rng
        self.memory = memory
        self.lines = []
        self.n = 0
        self.site = 0
        self.block = 0
        self.stypes = {}

    def fresh(self):
        self.n += 1
        return f"%v{self.n}"

    def emit(self, s):
        self.lines.append("  " + s)

    def label(self, name, args=""):
        self.lines.append(f"^{name}{args}:")

    def new_block(self):
        self.block += 1
        return f"b{self.block}"

    def pick(self, scope, w):
        vals = [v for v, vw in scope if vw == w]
        if vals and self.rng.random() < 0.8:
            return self.rng.choice(vals)
        return self.const(scope, w)

    # Structs: a struct type is its text; self.stypes maps it to its
    # fields (widths or struct types). Scope entries are (value, type).
    def tname(self, t):
        return f"i{t}" if isinstance(t, int) else t

    def value(self, scope, t):
        if isinstance(t, int):
            return self.pick(scope, t)
        vals = [v for v, vt in scope if vt == t]
        if vals and self.rng.random() < 0.8:
            return self.rng.choice(vals)
        return self.make_struct(scope, t)

    def new_stype(self):
        fields = [self.rng.choice(WIDTHS) for _ in range(self.rng.randint(1, 3))]
        if self.stypes and self.rng.random() < 0.3:
            fields.insert(
                self.rng.randint(0, len(fields)), self.rng.choice(sorted(self.stypes))
            )
        t = "!sea.struct<" + ", ".join(map(self.tname, fields)) + ">"
        self.stypes[t] = fields
        return t

    def make_struct(self, scope, t):
        fields = self.stypes[t]
        vs = [self.value(scope, f) for f in fields]
        v = self.fresh()
        self.emit(
            f"{v} = sea.struct.create {', '.join(vs)} : "
            f"{', '.join(map(self.tname, fields))} -> {t}"
        )
        scope.append((v, t))
        return v

    def struct_op(self, scope):
        structs = [
            (v, t)
            for v, t in scope
            if isinstance(t, str) and t.startswith("!sea.struct")
        ]
        kind = self.rng.choice(
            ["create", "extract", "extract", "extract", "insert", "select"]
        )
        if not structs or kind == "create":
            t = (
                self.rng.choice(sorted(self.stypes))
                if self.stypes and self.rng.random() < 0.5
                else self.new_stype()
            )
            if self.rng.random() < 0.5:
                # explicit nondet initialization (LLVM's insertvalue undef)
                v = self.fresh()
                self.emit(f'{v} = sea.struct.nondet "undef" [{self.site}] : {t}')
                self.site += 1
                scope.append((v, t))
                return v
            return self.make_struct(scope, t)
        s, t = self.rng.choice(structs)
        fields = self.stypes[t]
        i = self.rng.randrange(len(fields))
        v = self.fresh()
        if kind == "extract":
            self.emit(
                f"{v} = sea.struct.extract {s}[{i}] : {t} -> {self.tname(fields[i])}"
            )
            scope.append((v, fields[i]))
            if isinstance(fields[i], int):
                # An observer: scalarization erases the extract, so its
                # value has no location in the lowered program; the
                # observer (x | x = x) is in both, so check 4 compares it.
                o = self.fresh()
                self.emit(f"{o} = sea.or {v}, {v} : i{fields[i]}")
        elif kind == "insert":
            x = self.value(scope, fields[i])
            self.emit(
                f"{v} = sea.struct.insert {x}, {s}[{i}] : "
                f"{self.tname(fields[i])} into {t}"
            )
            scope.append((v, t))
        else:
            c, o = self.pick(scope, 1), self.value(scope, t)
            self.emit(f"{v} = sea.select {c}, {s}, {o} : {t}")
            scope.append((v, t))
        return v

    def const(self, scope, w):
        v = self.fresh()
        self.emit(f"{v} = sea.const {self.rng.getrandbits(w)} : i{w}")
        scope.append((v, w))
        return v

    def nondet(self, scope, w):
        v = self.fresh()
        self.emit(f'{v} = sea.nondet "nondet_i{w}" [{self.site}] : i{w}')
        self.site += 1
        scope.append((v, w))
        return v

    def cmp(self, scope):
        w = self.rng.choice(WIDTHS)
        a, b = self.pick(scope, w), self.pick(scope, w)
        v = self.fresh()
        self.emit(f"{v} = sea.icmp {self.rng.choice(PREDS)}, {a}, {b} : i{w}")
        scope.append((v, 1))
        return v

    def addr(self, scope):
        """An address value: an existing one, or a new one from an int."""
        addrs = [v for v, w in scope if w == "addr"]
        if addrs and self.rng.random() < 0.6:
            return self.rng.choice(addrs)
        i = self.pick(scope, AW)
        v = self.fresh()
        self.emit(f"{v} = sea.addr.from_int {i} : i{AW} to {ADDR}")
        scope.append((v, "addr"))
        return v

    def mem_op(self, scope, mems):
        """A memory or address op. `mems` maps region -> current version."""
        r = self.rng.random()
        region = self.rng.choice(sorted(mems))
        if r < 0.3:
            self.range_op(scope, mems, region)
        elif r < 0.35:
            # i8 = one cell; i16 = two cells, little-endian (may wrap)
            w = self.rng.choice([8, 8, 16])
            a, val = self.addr(scope), self.pick(scope, w)
            v = self.fresh()
            self.emit(
                f"{v} = sea.store {mems[region]}[{a}], {val} inplace : {MEM}, i{w}"
            )
            mems[region] = v
        elif r < 0.7:
            w = self.rng.choice([8, 8, 16])
            a = self.addr(scope)
            v = self.fresh()
            self.emit(f"{v} = sea.load {mems[region]}[{a}] : {MEM} -> i{w}")
            scope.append((v, w))
        elif r < 0.8:
            a, off = self.addr(scope), self.pick(scope, AW)
            v = self.fresh()
            self.emit(f"{v} = sea.addr.add {a}, {off} : {ADDR}, i{AW}")
            scope.append((v, "addr"))
        elif r < 0.9:
            a, b = self.addr(scope), self.addr(scope)
            v = self.fresh()
            self.emit(f"{v} = sea.addr.cmp {self.rng.choice(PREDS)}, {a}, {b} : {ADDR}")
            scope.append((v, 1))
        else:
            a = self.addr(scope)
            v = self.fresh()
            self.emit(f"{v} = sea.addr.to_int {a} : {ADDR} to i{AW}")
            scope.append((v, AW))

    def range_op(self, scope, mems, region):
        """memset, memmove or memhavoc with a constant length (0-3); a
        memset also with a symbolic one (sea-expand-memset; with 3-bit
        lengths its bound, 16, never cuts)."""
        a = self.addr(scope)
        v = self.fresh()
        kind = self.rng.choice(["memset", "memmove", "memhavoc"])
        if kind == "memset" and self.rng.random() < 0.4:
            n = self.pick(scope, AW)
        else:
            n = self.fresh()
            self.emit(f"{n} = sea.const {self.rng.randint(0, 3)} : i{AW}")
        if kind == "memset":
            val = self.pick(scope, 8)
            self.emit(
                f"{v} = sea.memset {mems[region]}[{a}], {val}, {n} inplace : {MEM}"
            )
        elif kind == "memmove":
            src = self.rng.choice(sorted(mems))  # same region or another one
            if src == region and self.rng.random() < 0.6:
                # overlapping on purpose: source = destination +- 1
                d = self.fresh()
                self.emit(f"{d} = sea.const {self.rng.choice([1, 7])} : i{AW}")
                sa = self.fresh()
                self.emit(f"{sa} = sea.addr.add {a}, {d} : {ADDR}, i{AW}")
            else:
                sa = self.addr(scope)
            self.emit(
                f"{v} = sea.memmove {mems[region]}[{a}], "
                f"{mems[src]}[{sa}], {n} inplace : {MEM}"
            )
        else:
            self.emit(
                f"{v} = sea.memhavoc {mems[region]}[{a}], {n} "
                f"[{self.site}] inplace : {MEM}"
            )
            self.site += 1
        mems[region] = v

    def intrinsic(self, scope, w):
        a, b = self.pick(scope, w), self.pick(scope, w)
        v = self.fresh()
        kind = self.rng.choice(["minmax", "ovf", "funnel", "bswap", "abs"])
        if kind == "abs":
            # nsw: LLVM's is_int_min_poison, a promise (diff_wrap_flags)
            flag = self.rng.choice(["", " nsw"])
            self.emit(f"{v} = sea.abs {a}{flag} : i{w}")
        elif kind == "bswap" and w % 16 == 0:
            self.emit(f"{v} = sea.bswap {a} : i{w}")
        elif kind == "funnel":
            s = self.pick(scope, w)
            self.emit(
                f"{v} = sea.{self.rng.choice(['fshl', 'fshr'])} {a}, {b}, {s} : i{w}"
            )
        elif kind == "ovf":
            if self.rng.random() < 0.15:
                self.emit(f"{v} = sea.nego {a} : i{w}")
            else:
                self.emit(f"{v} = sea.{self.rng.choice(OVF)} {a}, {b} : i{w}")
            scope.append((v, 1))
            return v
        else:
            self.emit(f"{v} = sea.{self.rng.choice(MINMAX)} {a}, {b} : i{w}")
        scope.append((v, w))
        return v

    def op(self, scope, mems):
        if mems and self.rng.random() < 0.35:
            return self.mem_op(scope, mems)
        if self.rng.random() < 0.15:
            return self.struct_op(scope)
        r = self.rng.random()
        w = self.rng.choice(WIDTHS)
        if r < 0.15:
            return self.nondet(scope, w)
        if r < 0.45:
            a, b = self.pick(scope, w), self.pick(scope, w)
            v = self.fresh()
            op = self.rng.choice(BINOPS)
            flags = ""
            if op in WRAP:
                flags = self.rng.choice(["", "", " nsw", " nuw", " nsw nuw"])
            self.emit(f"{v} = sea.{op} {a}, {b}{flags} : i{w}")
            scope.append((v, w))
            return v
        if r < 0.55:
            return self.intrinsic(scope, w)
        if r < 0.7:
            return self.cmp(scope)
        if r < 0.8:
            c = self.pick(scope, 1)
            a, b = self.pick(scope, w), self.pick(scope, w)
            v = self.fresh()
            self.emit(f"{v} = sea.select {c}, {a}, {b} : i{w}")
            scope.append((v, w))
            return v
        if r < 0.9:
            w2 = self.rng.choice([x for x in WIDTHS if x != w])
            a = self.pick(scope, w)
            v = self.fresh()
            if w2 > w:
                kind = self.rng.choice(["zext", "sext"])
            else:
                kind = "trunc"
            self.emit(f"{v} = sea.{kind} {a} : i{w} to i{w2}")
            scope.append((v, w2))
            return v
        # an assume, rarely, so that most programs stay feasible
        c = self.cmp(scope)
        if self.rng.random() < 0.3:
            self.emit(f"sea.assume {c}")
        return c

    def region(self, scope, depth, mems):
        """Emits code into the current block; returns with the current block
        open. `scope` holds the values usable here; `mems` the current
        version of each memory region (updated in place)."""
        for _ in range(self.rng.randint(1, 4)):
            self.op(scope, mems)
        if depth > 0 and self.rng.random() < 0.6:
            c = self.pick(scope, 1)
            then_b, else_b, join_b = (
                self.new_block(),
                self.new_block(),
                self.new_block(),
            )
            nargs = self.rng.randint(0, 2)
            arg_ws = [self.rng.choice(WIDTHS) for _ in range(nargs)]
            if self.stypes and self.rng.random() < 0.4:
                arg_ws.append(self.rng.choice(sorted(self.stypes)))
            regions = sorted(mems)
            # an empty arm jumps straight to the join: a critical edge (both
            # empty: equal targets); sea-split-critical-edges fixes them
            targets = []
            for b in (then_b, else_b):
                if self.rng.random() < 0.25:
                    outs = [self.value(scope, w) for w in arg_ws]
                    outs += [mems[r] for r in regions]
                    types = [self.tname(w) for w in arg_ws] + [MEM] * len(regions)
                    targets.append(
                        f"^{join_b}({', '.join(outs)} : {', '.join(types)})"
                        if outs
                        else f"^{join_b}"
                    )
                else:
                    targets.append(f"^{b}")
            self.emit(f"sea.cond_br {c}, {targets[0]}, {targets[1]}")
            for b, t in zip((then_b, else_b), targets):
                if t != f"^{b}":
                    continue
                self.label(b)
                inner = list(scope)
                inner_mems = dict(mems)
                self.region(inner, depth - 1, inner_mems)
                outs = [self.value(inner, w) for w in arg_ws]
                types = [self.tname(w) for w in arg_ws]
                # each region's current version flows into the join
                outs += [inner_mems[r] for r in regions]
                types += [MEM] * len(regions)
                if outs:
                    self.emit(
                        f"sea.br ^{join_b}({', '.join(outs)} : {', '.join(types)})"
                    )
                else:
                    self.emit(f"sea.br ^{join_b}")
            args = []
            for w in arg_ws:
                v = self.fresh()
                args.append(f"{v} : {self.tname(w)}")
                scope.append((v, w))
            for r in regions:
                v = self.fresh()
                args.append(f"{v} : {MEM}")
                mems[r] = v
            self.label(join_b, f"({', '.join(args)})" if args else "")
            self.region(scope, depth - 1, mems)

    def program(self):
        self.lines.append("sea.program @fuzz {")
        scope = []
        self.nondet(scope, self.rng.choice(WIDTHS))
        mems = {}
        if self.memory:
            for r in range(self.rng.randint(0, 2)):
                v = self.fresh()
                self.emit(f'{v} = sea.mem.nondet "r{r}" [{self.site}] : {MEM}')
                self.site += 1
                mems[r] = v
        self.region(scope, depth=3, mems=mems)
        if mems and self.rng.random() < 0.7:
            # make the property depend on memory contents
            x = self.fresh()
            self.emit(
                f"{x} = sea.load {mems[self.rng.choice(sorted(mems))]}"
                f"[{self.addr(scope)}] : {MEM} -> i8"
            )
            y = self.fresh()
            self.emit(
                f"{y} = sea.load {mems[self.rng.choice(sorted(mems))]}"
                f"[{self.addr(scope)}] : {MEM} -> i8"
            )
            c = self.fresh()
            self.emit(
                f"{c} = sea.icmp {self.rng.choice(PREDS)}, {x}, "
                f"{self.rng.choice([y, self.pick(scope, 8)])} : i8"
            )
        else:
            c = self.cmp(scope)
        self.emit(f"sea.assert {c}")
        self.emit("sea.halt")
        self.lines.append("}")
        return "\n".join(self.lines) + "\n"


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def field(out, key):
    for line in out.splitlines():
        if line.startswith(key + ": "):
            return line.split(": ", 1)[1]
    return None


VALUE = re.compile(r"^value: (\S+) loc\((.*)\) : (\S+) = (\d+)$")


def trace(out):
    """[(op, loc, type, value)] from sea-run --trace."""
    return [m.groups() for m in map(VALUE.match, out.splitlines()) if m]


INPUT = re.compile(
    r"^(?:nondet: site=(\d+)|memread: site=(\d+) addr=(\d+))"
    r" .*value=(\d+)"
)


def inputs_of(out):
    """The nondet values and memory cells a sea-run read, as an --input
    list (the random draws are sequential: a lowered program that reads
    fewer cells, its dead loads erased, would otherwise draw others)."""
    keys = []
    for m in map(INPUT.match, out.splitlines()):
        if m:
            site, msite, addr, val = m.groups()
            keys.append(f"{site}={val}" if site else f"{msite}@{addr}={val}")
    return ",".join(keys)


def diff_lowering(args, path, tmp, runs):
    """Runs the original and the lowered program on the same random inputs
    (the lowered run gets the values the original run read); they must
    agree on the status and on the value of every op of the original
    (compared with the last value the lowered program computes at the op's
    location). Twice: the lowering without sccp and without cse /
    canonicalize (sea-vc-lower{fold=false opt=false}), where every op keeps
    a location, so every value is compared; and the full lowering (what
    sea-vc --lower runs), where sccp and canonicalize have replaced the ops
    computed from constants by uniqued constants without a location, and
    cse / canonicalize erased duplicate and dead ops: there the values left
    at their location are compared (the folded ones through their uses; the
    folders themselves are checked by check-semantics.py --fold). Returns a
    problem or None."""
    for fold, options in (("false", "fold=false opt=false"), ("true", "")):
        lowered = f"{path}.lowered-{fold}.mlir"
        r = run(
            [
                args.sea_opt,
                path,
                f"--pass-pipeline=builtin.module(sea-vc-lower{{{options}}})",
                "--mlir-print-debuginfo",
                "-o",
                lowered,
            ]
        )
        if r.returncode != 0:
            return f"lowering (fold={fold}) failed\n{r.stderr}"
        for seed in range(runs):
            cmd = ["--missing=random", f"--seed={seed}", "--trace"]
            a = run([args.sea_run, path] + cmd)
            given = inputs_of(a.stdout)
            b = run(
                [args.sea_run, lowered] + cmd + ([f"--input={given}"] if given else [])
            )
            if field(a.stdout, "status") != field(b.stdout, "status"):
                return (
                    f"fold={fold} seed {seed}: original status "
                    f"{field(a.stdout, 'status')}, lowered "
                    f"{field(b.stdout, 'status')}"
                )
            last = {loc: (ty, val) for _, loc, ty, val in trace(b.stdout)}
            for op, loc, ty, val in trace(a.stdout):
                if op == "sea.struct.extract":
                    continue  # erased by scalarization; its value is
                    # compared through the observer the generator
                    # puts after it
                if fold == "true" and loc not in last:
                    continue  # folded or erased (see above)
                if last.get(loc) != (ty, val):
                    return (
                        f"fold={fold} seed {seed}: original {op} at "
                        f"{loc} = {ty} {val}, lowered {last.get(loc)}"
                    )
    return None


def diff_wrap_flags(args, path, runs, final_assert_line):
    """sea-wrap-flags{mode=assert} against the interpreter's own check of
    the promises: on the same random inputs, the original run breaks a
    promise (warning interp.wrap-promise) iff the transformed run fails an
    inserted assert (one other than the program's final assert). Returns a
    problem or None."""
    with open(path) as f:
        if not re.search(r" (nsw|nuw)( |:)", f.read()):
            return None
    checked = path + ".wrap.mlir"
    r = run(
        [
            args.sea_opt,
            path,
            "--pass-pipeline=builtin.module(sea.program(sea-wrap-flags{mode=assert}))",
            "--mlir-print-debuginfo",
            "-o",
            checked,
        ]
    )
    if r.returncode != 0:
        return f"sea-wrap-flags failed\n{r.stderr}"
    for seed in range(runs):
        cmd = ["--missing=random", f"--seed={seed}"]
        a, b = run([args.sea_run, path] + cmd), run([args.sea_run, checked] + cmd)
        broken = "interp.wrap-promise" in a.stdout + a.stderr
        at = field(b.stdout, "at") or ""
        failed = (
            field(b.stdout, "status") == "assert-failed"
            and f":{final_assert_line}:" not in at
        )
        if broken != failed:
            return (
                f"seed {seed}: original breaks a promise: {broken}; "
                f"sea-wrap-flags{{mode=assert}} fails an inserted assert: "
                f"{failed} ({field(b.stdout, 'status')} at {at})"
            )
    return None


def cex_inputs(out):
    """sea-run --input items from sea-vc's nondet: and memread: lines."""
    items = []
    for line in out.splitlines():
        m = re.match(r"nondet: site=(\d+) .* value=(\d+)", line)
        if m:
            items.append(f"{m.group(1)}={m.group(2)}")
        m = re.match(r"memread: site=(\d+) addr=(\d+) width=\d+ value=(\d+)", line)
        if m:
            items.append(f"{m.group(1)}@{m.group(2)}={m.group(3)}")
    return items


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sea-vc", default="sea-vc")
    ap.add_argument("--sea-opt", default="sea-opt")
    ap.add_argument("--sea-run", default="sea-run")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("-n", type=int, default=50, help="programs")
    ap.add_argument(
        "--runs", type=int, default=20, help="random interpreter runs per unsat program"
    )
    ap.add_argument("--keep", help="directory to keep failing programs")
    ap.add_argument(
        "--bitwuzla",
        action="store_true",
        help="also solve with Bitwuzla (sea-vc built with it)",
    )
    ap.add_argument(
        "--no-yices",
        action="store_true",
        help="do not solve with Yices (sea-vc built without it)",
    )
    ap.add_argument(
        "--seam",
        action="store_true",
        help="also run sea-vc --seam (and --seam-inb=0) with each "
        "solver: verdicts must agree, sat models must replay",
    )
    ap.add_argument(
        "--sink-assumes",
        action="store_true",
        help="also run sea-vc --sink-assumes (with --seam: also "
        "--seam --sink-assumes) with each solver: verdicts must "
        "agree, sat models must replay and fail the original "
        "program",
    )
    ap.add_argument(
        "--no-memory", action="store_true", help="generate scalar programs only"
    )
    args = ap.parse_args()
    for attr in ("sea_vc", "sea_run", "sea_opt"):
        v = getattr(args, attr)
        if os.sep in v:
            setattr(args, attr, os.path.abspath(v))

    rng = random.Random(args.seed)
    stats = {"sat": 0, "unsat": 0}
    undetermined = 0  # sat runs whose replay.original is match=undetermined
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        for i in range(args.n):
            text = Gen(rng, memory=not args.no_memory).program()
            path = os.path.join(tmp, f"p{i}.mlir")
            with open(path, "w") as f:
                f.write(text)
            verdicts = {}
            problem = None
            variants = [("", [])]
            if args.seam:
                variants += [
                    ("+seam", ["--seam"]),
                    ("+seam0", ["--seam", "--seam-inb=0"]),
                ]
            if args.sink_assumes:
                variants += [("+sink", ["--sink-assumes"])]
                if args.seam:
                    variants += [("+seam+sink", ["--seam", "--sink-assumes"])]
            sink_runs = []
            for solver, (vname, vflags) in [
                (s, v)
                for s in ("z3",)
                + (() if args.no_yices else ("yices",))
                + (("bitwuzla",) if args.bitwuzla else ())
                for v in variants
            ]:
                r = run(
                    [args.sea_vc, path, f"--solver={solver}", "--lower"]
                    + ["--replay-original"]
                    + vflags
                )
                res = field(r.stdout, "result")
                verdicts[solver + vname] = res
                if solver == "z3" and not vname:
                    r_z3 = r
                if "sink" in vname and res == "sat":
                    sink_runs.append((solver + vname, r))
                if r.returncode == 2:
                    problem = (
                        f"{solver}{vname}: replay disagrees with the model\n" + r.stdout
                    )
                elif r.returncode == 6:
                    problem = (
                        f"{solver}{vname}: the original program does not fail the "
                        "assertion of the lowered program's counterexample "
                        "(--replay-original)\n" + r.stdout
                    )
                elif r.returncode != 0:
                    problem = (
                        f"{solver}{vname}: exit {r.returncode}\n{r.stdout}{r.stderr}"
                    )
                elif (
                    res == "sat"
                    and (ro := (field(r.stdout, "replay.original") or ""))
                    and "match=undetermined" in ro
                ):
                    # the replay decides nothing (sea-vc warns); counted
                    undetermined += 1
                elif res == "sat" and "match=yes" not in (
                    field(r.stdout, "replay.original") or ""
                ):
                    problem = (
                        f"{solver}{vname}: sat, but no replay.original: match=yes\n"
                        + r.stdout
                        + r.stderr
                    )
            if not problem and verdicts["z3"] == "sat":
                # sea-vc replays the lowered program; the counterexample must
                # also fail the original one (validates the lowering passes).
                inputs = cex_inputs(r_z3.stdout)
                o = run(
                    [args.sea_run, path, "--missing=zero"]
                    + ([f"--input={','.join(inputs)}"] if inputs else [])
                )
                if field(o.stdout, "status") != "assert-failed":
                    problem = (
                        "the counterexample of the lowered program "
                        "does not fail the original one: "
                        f"{field(o.stdout, 'status')}\n{o.stdout}"
                    )
            for name, rs in sink_runs:
                if problem:
                    break
                # the counterexample of the sunk program fails the original
                inputs = cex_inputs(rs.stdout)
                o = run(
                    [args.sea_run, path, "--missing=zero"]
                    + ([f"--input={','.join(inputs)}"] if inputs else [])
                )
                if field(o.stdout, "status") != "assert-failed":
                    problem = (
                        f"{name}: the counterexample does not fail the "
                        f"original program: {field(o.stdout, 'status')}"
                        f"\n{o.stdout}"
                    )
            if not problem:
                problem = diff_lowering(args, path, tmp, args.runs)
            if not problem:
                with open(path) as f:
                    final = next(i for i, l in enumerate(f, 1) if "sea.assert" in l)
                problem = diff_wrap_flags(args, path, args.runs, final)
            for other in verdicts:
                if not problem and verdicts["z3"] != verdicts[other]:
                    problem = (
                        f"z3 says {verdicts['z3']}, {other} says {verdicts[other]}"
                    )
            if not problem and verdicts["z3"] == "unsat":
                for seed in range(args.runs):
                    r = run([args.sea_run, path, "--missing=random", f"--seed={seed}"])
                    if field(r.stdout, "status") == "assert-failed":
                        problem = (
                            "unsat, but the interpreter fails the "
                            f"assertion with --missing=random --seed={seed}"
                        )
                        break
            if problem:
                failures.append((i, problem, text))
                if args.keep:
                    os.makedirs(args.keep, exist_ok=True)
                    with open(os.path.join(args.keep, f"fail{i}.mlir"), "w") as f:
                        f.write(text)
            elif verdicts["z3"] in stats:
                stats[verdicts["z3"]] += 1

    for i, problem, text in failures[:5]:
        print(f"FAIL program {i}: {problem}\n{text}")
    print(
        f"programs: {args.n} sat: {stats['sat']} unsat: {stats['unsat']} "
        f"failures: {len(failures)}"
    )
    print(f"replay-original: undetermined={undetermined}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()

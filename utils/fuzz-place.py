#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Differential fuzzing of static placement (sea-place-static).

Generates random abstract programs (objects of small constant sizes on the
stack, the heap and as globals; in-bounds pointer arithmetic; loads, stores,
memset / memmove / memhavoc within objects; pointer comparisons and
differences; pointers and memory passed through diamonds; an assert on
loaded values) and checks, for each:
  1. placement succeeds;
  2. on the same random inputs, the abstract interpreter (objects, exact
     bounds) and the concrete interpreter on the placed program reach the
     same status and compute the same values op by op (addresses excluded:
     they exist only after placement);
  3. sea-vc --lower: sat replays (exit 0), unsat survives the abstract runs.

ORACLE (see test/README.md): changing what it accepts needs sign-off.
"""

import argparse
import os
import random
import re
import subprocess
import sys
import tempfile

PW = 8  # pointer width: compact layout, small numbers
PTR = f"!sea.ptr<{PW}>"
MEM = f"!sea.mem<{PTR}, i8>"
PREDS = ["eq", "ne", "ult", "ule", "slt", "sge"]


class Gen:
    def __init__(self, rng):
        self.rng = rng
        self.lines = []
        self.n = 0
        self.site = 0
        self.block = 0

    def fresh(self):
        self.n += 1
        return f"%v{self.n}"

    def emit(self, s):
        self.lines.append("  " + s)

    def const(self, value, width):
        v = self.fresh()
        self.emit(f"{v} = sea.const {value} : i{width}")
        return v

    def new_site(self):
        self.site += 1
        return self.site - 1

    def objects(self):
        """Allocates 1-3 objects plus maybe a global; returns [(base, size)]."""
        objs = []
        for _ in range(self.rng.randint(1, 3)):
            size = self.rng.randint(1, 4)
            kind = self.rng.choice(["alloca", "malloc"])
            v = self.fresh()
            n = self.const(size, PW)
            self.emit(
                f"{v} = sea.{kind} {n} align {self.rng.choice([1, 2])} "
                f"[{self.new_site()}] : i{PW} -> {PTR}"
            )
            objs.append((v, size))
        if self.rng.random() < 0.5:
            size = self.rng.randint(1, 4)
            v = self.fresh()
            self.emit(f"{v} = sea.global @g size {size} align 1 : {PTR}")
            objs.append((v, size))
        return objs

    def ptr(self, objs, room=1):
        """A pointer into an object with at least `room` cells from it."""
        base, size = self.rng.choice(objs)
        off = self.rng.randint(0, size - min(room, size))
        if off == 0:
            return base, size
        v = self.fresh()
        self.emit(f"{v} = sea.ptr.add {base}, {self.const(off, PW)} : {PTR}, i{PW}")
        return v, size - off

    def body(self, objs, state, depth):
        """Random memory and pointer ops; `state` holds the current memory
        version and the values usable here."""
        for _ in range(self.rng.randint(2, 6)):
            r = self.rng.random()
            if r < 0.3:
                p, _ = self.ptr(objs)
                val = self.rng.choice(
                    state["vals"] + [self.const(self.rng.randint(0, 255), 8)]
                )
                v = self.fresh()
                self.emit(
                    f"{v} = sea.store {state['mem']}[{p}], {val} inplace : {MEM}, i8"
                )
                state["mem"] = v
            elif r < 0.55:
                p, _ = self.ptr(objs)
                v = self.fresh()
                self.emit(f"{v} = sea.load {state['mem']}[{p}] : {MEM} -> i8")
                state["vals"].append(v)
            elif r < 0.75:
                n = self.rng.randint(0, 2)
                p, room = self.ptr(objs, n)
                n = min(n, room)
                nv = self.const(n, PW)
                v = self.fresh()
                kind = self.rng.choice(["memset", "memmove", "memhavoc"])
                if kind == "memset":
                    val = self.const(self.rng.randint(0, 255), 8)
                    self.emit(
                        f"{v} = sea.memset {state['mem']}[{p}], {val}, "
                        f"{nv} inplace : {MEM}"
                    )
                elif kind == "memmove":
                    q, qroom = self.ptr(objs, n)
                    if qroom < n:
                        continue
                    self.emit(
                        f"{v} = sea.memmove {state['mem']}[{p}], "
                        f"{state['mem']}[{q}], {nv} inplace : {MEM}"
                    )
                else:
                    self.emit(
                        f"{v} = sea.memhavoc {state['mem']}[{p}], {nv} "
                        f"[{self.new_site()}] inplace : {MEM}"
                    )
                state["mem"] = v
            elif r < 0.85:
                (p, _), (q, _) = self.ptr(objs), self.ptr(objs)
                v = self.fresh()
                self.emit(
                    f"{v} = sea.ptr.cmp {self.rng.choice(['eq', 'ne'])}, "
                    f"{p}, {q} : {PTR}"
                )
                state["bools"].append(v)
            else:
                base, size = self.rng.choice(objs)
                q = base
                if size > 1:
                    q = self.fresh()
                    self.emit(
                        f"{q} = sea.ptr.add {base}, "
                        f"{self.const(self.rng.randint(0, size - 1), PW)}"
                        f" : {PTR}, i{PW}"
                    )
                v = self.fresh()
                self.emit(f"{v} = sea.ptr.diff {q}, {base} : {PTR} -> i{PW}")
        if depth > 0 and self.rng.random() < 0.5:
            c = (
                self.rng.choice(state["bools"])
                if state["bools"]
                else self.nondet_bool()
            )
            names = [f"b{self.block + i}" for i in range(1, 4)]
            self.block += 3
            t, e, j = names
            self.emit(f"sea.cond_br {c}, ^{t}, ^{e}")
            for b in (t, e):
                self.lines.append(f"^{b}:")
                inner = {
                    "mem": state["mem"],
                    "vals": list(state["vals"]),
                    "bools": list(state["bools"]),
                }
                self.body(objs, inner, depth - 1)
                p, _ = self.ptr(objs)
                self.emit(f"sea.br ^{j}({inner['mem']}, {p} : {MEM}, {PTR})")
            m, p = self.fresh(), self.fresh()
            self.lines.append(f"^{j}({m} : {MEM}, {p} : {PTR}):")
            state["mem"] = m
            # the joined pointer is in bounds for one cell of its object
            x = self.fresh()
            self.emit(f"{x} = sea.load {m}[{p}] : {MEM} -> i8")
            state["vals"].append(x)
            self.body(objs, state, depth - 1)

    def nondet_bool(self):
        v = self.fresh()
        self.emit(f'{v} = sea.nondet "nondet_bool" [{self.new_site()}] : i1')
        return v

    def program(self):
        self.lines.append("sea.program @fuzz {")
        m = self.fresh()
        self.emit(f'{m} = sea.mem.nondet "m" [{self.new_site()}] : {MEM}')
        objs = self.objects()
        state = {"mem": m, "vals": [], "bools": []}
        self.body(objs, state, depth=2)
        p, _ = self.ptr(objs)
        x = self.fresh()
        self.emit(f"{x} = sea.load {state['mem']}[{p}] : {MEM} -> i8")
        y = self.rng.choice(state["vals"] + [self.const(self.rng.randint(0, 255), 8)])
        c = self.fresh()
        self.emit(f"{c} = sea.icmp {self.rng.choice(PREDS)}, {x}, {y} : i8")
        self.emit(f"sea.assert {c}")
        self.emit("sea.halt")
        self.lines.append("}")
        return "\n".join(self.lines) + "\n"


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


VALUE = re.compile(r"^value: (\S+) loc\((.*)\) : (\S+) = (\d+)$")


def trace(out):
    """[(op, loc, type, value)] from sea-run --trace. Placement turns
    sea.ptr.<op> into sea.addr.<op> at the same location: compare them as
    one op."""
    return [
        (op.replace("sea.ptr.", "sea.addr."), loc, ty, val)
        for op, loc, ty, val in (
            m.groups() for m in map(VALUE.match, out.splitlines()) if m
        )
    ]


def field(out, key):
    for line in out.splitlines():
        if line.startswith(key + ": "):
            return line.split(": ", 1)[1]
    return None


def check(args, path, placed):
    """Returns a problem description, or None."""
    for seed in range(args.runs):
        cmd = ["--missing=random", f"--seed={seed}", "--trace"]
        a = run([args.sea_run, path] + cmd)
        c = run([args.sea_run, placed] + cmd)
        sa, sc = field(a.stdout, "status"), field(c.stdout, "status")
        if sa == "error":
            return f"abstract run errs (seed {seed}): {field(a.stdout, 'message')}"
        if sa != sc:
            return (
                f"seed {seed}: abstract status {sa}, placed status {sc} "
                f"({field(c.stdout, 'message')})"
            )
        ta = trace(a.stdout)
        keys = {(op, loc) for op, loc, _, _ in ta}
        tc = [
            t
            for t in trace(c.stdout)
            if (t[0], t[1]) in keys and not t[2].startswith("!sea.addr")
        ]
        if ta != tc:
            for x, y in zip(ta, tc):
                if x != y:
                    return f"seed {seed}: abstract {x} vs placed {y}"
            return f"seed {seed}: traces differ in length ({len(ta)} vs {len(tc)})"
    v = run([args.sea_vc, path, "--lower"])
    if v.returncode not in (0,):
        return f"sea-vc --lower exit {v.returncode}\n{v.stdout}{v.stderr}"
    if field(v.stdout, "result") == "unsat":
        for seed in range(args.runs):
            a = run([args.sea_run, path, "--missing=random", f"--seed={seed}"])
            if field(a.stdout, "status") == "assert-failed":
                return f"unsat, but the abstract run fails the assertion (seed {seed})"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sea-opt", default="sea-opt")
    ap.add_argument("--sea-run", default="sea-run")
    ap.add_argument("--sea-vc", default="sea-vc")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("-n", type=int, default=50)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--keep", help="directory to keep failing programs")
    args = ap.parse_args()
    for attr in ("sea_opt", "sea_run", "sea_vc"):
        v = getattr(args, attr)
        if os.sep in v:
            setattr(args, attr, os.path.abspath(v))

    rng = random.Random(args.seed)
    stats = {"sat": 0, "unsat": 0}
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        for i in range(args.n):
            text = Gen(rng).program()
            path = os.path.join(tmp, f"p{i}.mlir")
            placed = os.path.join(tmp, f"p{i}.placed.mlir")
            with open(path, "w") as f:
                f.write(text)
            p = run(
                [
                    args.sea_opt,
                    path,
                    "--pass-pipeline=builtin.module(sea.program(sea-place-static))",
                    "--mlir-print-debuginfo",
                    "-o",
                    placed,
                ]
            )
            problem = None if p.returncode == 0 else f"placement failed\n{p.stderr}"
            if not problem:
                problem = check(args, path, placed)
            if problem:
                failures.append((i, problem, text))
                if args.keep:
                    os.makedirs(args.keep, exist_ok=True)
                    with open(os.path.join(args.keep, f"fail{i}.mlir"), "w") as f:
                        f.write(text)
                continue
            res = field(run([args.sea_vc, path, "--lower"]).stdout, "result")
            if res in stats:
                stats[res] += 1
    for i, problem, text in failures[:5]:
        print(f"FAIL program {i}: {problem}\n{text}")
    print(
        f"programs: {args.n} sat: {stats['sat']} unsat: {stats['unsat']} "
        f"failures: {len(failures)}"
    )
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Differential fuzzing of sea-vc --seam on programs with objects.

fuzz-vc.py's programs address memory with 3-bit addresses and never allocate,
so SeaM's object-aware rules (cmp.diff-base, row.store-skip on different
objects) never fire there. This generator makes 2-4 allocas (plus sometimes a
ptr.nondet), pointer arithmetic with numeral and nondet offsets, stores and
loads of i32 and of pointers, pointer compares and int casts of pointers, and
asserts a pointer compare. Not an oracle (test/README.md): a second check of
--seam against the plain VC.

  --seam --seam-inb=0 is free of A-INB here only because the generator makes
  no memhavoc: the lowering's dropped memhavocs (assume.a-inb) rest on A-INB
  even without it, and a run that reports one is a problem.

  --accesses=bases  every access is at an object's base: A-INB holds, so
                    plain, --seam and --seam --seam-inb=0 must give the same
                    verdict, and no run may fall back to no A-INB.
  --accesses=any    an access may use any pointer, so it may leave its object
                    (A-INB may fail): plain and --seam --seam-inb=0 must
                    agree; --seam may differ (violating mode, below).
  --accesses=wide   like any, and the program always has pointers that are
                    sums over ites of 5-8 pointers (more leaves than the
                    syntactic rules look at: the ARM case, and its access
                    check) with a numeral or nondet offset, used by most
                    accesses. The same agreement as for any.
  --accesses=cells  many stores and loads at numeral offsets of two to four
                    16 / 32-byte objects, every access inside its object
                    (a 4-byte cell at an offset in [0, size - 4]), with a few
                    nondet offsets assumed in [0, size - 4] (the barriers and
                    the R-ABORT of Store-Map): A-INB holds, every variant
                    agrees, and no run may fall back to no A-INB. The programs
                    in which Store-Map (--seam-store-map, default on) groups
                    writes into store-maps, commutes writes across objects
                    and reads them back.
  --accesses=cells-violate
                    the cells programs with accesses that may leave their
                    object (offsets up to twice the size, a nondet offset up to
                    size + 8) and one store at offset `size` that surely does:
                    the deliberate-violation mode (violating). The surely
                    out-of-bounds store has a numeral offset, which --seam
                    detects statically (fallbacks, counted).
  --accesses=circular
                    a potential circularity in the A-INB assumption: stores through an object plus an unconstrained
                    (or loosely bounded) nondet offset into zero-initialized
                    memory, that can land in another object, then a load and a
                    guard that depends on it: an assume, a branch whose other
                    arm is unreachable, or a branch with a load-dependent
                    address, and sometimes a later store whose address comes
                    from the load. Under A-INB the load reads the old value and
                    the guard can remove the very executions that violate A-INB;
                    plain and --seam --seam-inb=0 agree; --seam may differ
                    (violating mode).
  --accesses=ptrmem
                    the pointer-through-memory shape (review of the A-INB
                    check): a pointer into an object (symbolic or numeral
                    offset, possibly out of the object) is stored in a cell, a
                    store at an in-bounds symbolic offset of another object
                    follows, the pointer is reloaded (the rewriting can skip
                    the symbolic store only under A-INB) and stored through,
                    and a cell of another object is read back. The access
                    through the reloaded pointer has a condition that only the
                    encoding under A-INB sees (violating mode).
  --accesses=addr-forms
                    completeness, not soundness (metamorphic): an NKI-shaped
                    abstract program (64-bit pointers; indices loaded from an
                    index tile, also through a memmove copy of it, sext and
                    assumed in range; memmoves whose source bytes are read
                    directly and whose destination bytes are read back; every
                    access in its object) is rendered in the FORM_VARIANTS
                    forms: flat sums (the reference), ptr.add chains,
                    sea.ptr.index nodes, nested nodes, one stride-1 index of
                    the computed sum, the copied index values, and mixed
                    (each address its own form, operands commuted, constants
                    split, the index reloaded), with and without the --lower
                    CSE / canonicalize passes (--lower-opt=false). Every form
                    must give the reference's verdict and SeaM resolution
                    counters (FORM_KEYS: vc.ackermann-arrays / -reads /
                    -constraints, seam.inb, seam.alias.open.all; one --seam
                    run each). A difference is a failure; the reference and
                    the variant are kept with the commands and counters
                    (forms-<seed>-<i>-*.mlir, .txt). The memmoves as byte
                    loads and stores (bytes) is another memory program: held
                    to the verdict only; its counter differences are counted
                    (memop-expansion-deltas; -fewer-constraints lists the
                    programs whose memmove form left more constraints).
                    first-finding: the index of the first failing program.
                    The other options of this help (variants, --seam-check,
                    the A-INB checks) do not apply to it.
Every variant also runs with --seam-store-map=0 (linear store chains):
the verdict must be the same.
--seam-check: every --seam variant also runs sea-vc --seam-check;
each rule firing is a solver query `premise => lhs = rhs`. An invalid
firing (exit 5) is a failure; the summary sums the seam-check counts
(seam-check-<count>: <n>).
Every run must exit 0 (a sat is confirmed by replay), except a --seam run in a
violating mode (its answer is conditional on A-INB: it may be a sat that does
not replay).

A-INB is assumed and reported, not discharged (docs/design/SeaM.md). In the
in-bounds modes (bases, cells) every variant must agree with plain, and no run
may fall back to no A-INB (the static check of numeral offsets). In the
violating modes (any, wide, cells-violate, circular, ptrmem) --seam may give
another verdict than plain: counted (seam-differs). When --seam says unsat and
plain says sat, plain's counterexample must really leave an object: the
program is run by sea-run (abstract, every access checked against its object)
with the values of the nondet sites of the counterexample. "oob-confirmed":
it stops with a memory access out of bounds; "oob-undetermined": it stops for
another reason (the abstract interpreter has no address of a pointer:
ptr.to_int / ptr.from_int); a run that reaches the end or the failed assertion
without leaving an object is a failure (the counterexample is in bounds, so
--seam's unsat is wrong). Every --seam run has --seam-stats; the summary reports
the counts of the final run of the seam variant, summed over the programs:
wow.commute, smap.commute, the cross-base skips (cmp.diff-base,
arm.skip-diff-base, row.smap-skip; cross-base-skips is their sum), premise.a-inb
(the rewrites that depend on A-INB), the programs in which that run fell back to
no A-INB (fallbacks), and the programs in which ARM decided a compare
(arm-fired) and Store-Map made or read a store-map (smap-fired: the Store-Map
construction alone, not a rule that survived into the final run: use the
final-run counts for that).
"""

import argparse
import concurrent.futures
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile


def as_ptr_index(text, rng, strides=(1,)):
    """Each `p = sea.ptr.add b, o : T, i32` as `p = sea.ptr.index b [o]
    strides [s] offset 0 <flag> : T, i32`, s drawn from `strides`: with
    s = 1 the same address (the VC encodes a PTR_INDEX node under --seam);
    another s is another program (b + o * s), on which every variant runs
    alike. A promise the program breaks is a warning of the interpreter
    (interp.ptr-index-promise), no value changes."""

    def one(m):
        flag = rng.choice(["", " inbounds", " nuw"])
        s = rng.choice(strides) if len(strides) > 1 else strides[0]
        return (
            f"{m.group(1)} = sea.ptr.index {m.group(2)} [{m.group(3)}] "
            f"strides [{s}] offset 0{flag} : {m.group(4)}, i32"
        )

    return re.sub(r"(%\w+) = sea\.ptr\.add (%\w+), (%\w+) : ([^,]+), i32", one, text)


def gen(rng, any_access, wide=False, cells=False, violate=False):
    lines = []
    n = [0]

    def fresh():
        n[0] += 1
        return f"%v{n[0]}"

    def emit(s):
        lines.append("  " + s)

    PT = "!sea.ptr<32>"
    M = f"!sea.mem<{PT}, i8>"
    mem = fresh()
    emit(f'{mem} = sea.mem.nondet "r" [0] : {M}')
    ptrs, bases, ints, i32s = [], [], [], []
    sizes = {}  # base pointer -> object size in bytes
    last_sel = []
    site = [1]

    def const(v):
        x = fresh()
        emit(f"{x} = sea.const {v} : i32")
        return x

    for _ in range(rng.randint(3, 5) if wide else rng.randint(2, 4)):
        size = rng.choice(
            [16, 32] if wide or cells else [1, 2, 4, 8] if any_access else [4, 8, 16]
        )
        c = const(size)
        p = fresh()
        emit(f"{p} = sea.alloca {c} align 1 [{site[0]}] : i32 -> {PT}")
        site[0] += 1
        sizes[p] = size
        ptrs.append(p)
        bases.append(p)
    if rng.random() < 0.3:
        p = fresh()
        emit(f'{p} = sea.ptr.nondet "np" [{site[0]}] : {PT}')
        site[0] += 1
        ptrs.append(p)

    def nondet_offset():
        x = fresh()
        emit(f'{x} = sea.nondet "i" [{site[0]}] : i32')
        site[0] += 1
        if rng.random() < 0.6:  # bounded
            lo, hi = (
                const(rng.randint(0, 2)),
                const(rng.randint(2, 40 if wide else 9)),
            )  # wide: may leave an object
            a, b, c = fresh(), fresh(), fresh()
            emit(f"{a} = sea.icmp uge, {x}, {lo} : i32")
            emit(f"{b} = sea.icmp ule, {x}, {hi} : i32")
            emit(f"{c} = sea.and {a}, {b} : i1")
            emit(f"sea.assume {c}")
        return x

    wides = []
    if wide:
        for _ in range(rng.randint(1, 2)):
            k = rng.randint(5, 8)
            cur = rng.choice(bases)
            for _ in range(k - 1):
                c, p = fresh(), fresh()
                emit(f'{c} = sea.nondet "c" [{site[0]}] : i1')
                site[0] += 1
                emit(f"{p} = sea.select {c}, {rng.choice(bases)}, {cur} : {PT}")
                cur = p
            # mostly inside the objects (an access that surely leaves one
            # makes sea-vc run without A-INB: ARM then has nothing to do)
            o = (
                const(
                    rng.choice(
                        [-8, 100] if rng.random() < 0.15 else [0, 1, 2, 4, 8, 12]
                    )
                )
                if rng.random() < 0.4
                else nondet_offset()
            )
            q = fresh()
            emit(f"{q} = sea.ptr.add {cur}, {o} : {PT}, i32")
            wides.append(q)
            ptrs.append(q)

    def access_any():
        if wides and rng.random() < 0.6:
            return rng.choice(wides)
        if ptrs[len(ptrs) // 2 :] and rng.random() < 0.4:
            return rng.choice(ptrs[len(ptrs) // 2 :])  # a recent pointer
        return rng.choice(ptrs)

    access = access_any if any_access else (lambda: rng.choice(bases))
    if cells:
        # i32 cells of every object at numeral offsets (in bounds: a 4-byte
        # cell at an offset in [0, size - 4]; with `violate` up to twice the
        # size), and a few pointers with a nondet offset in [0, size - 4]
        # (a barrier for Store-Map; with `violate` up to size + 8)
        cellptrs, symptrs = [], []
        for b in list(bases):
            size = sizes[b]
            offs = list(range(0, 2 * size if violate else size - 3, 4))
            for k in rng.sample(offs, min(4, len(offs))):
                p = fresh()
                o = const(k)
                emit(f"{p} = sea.ptr.add {b}, {o} : {PT}, i32")
                cellptrs.append(p)
            if rng.random() < 0.5:
                x = fresh()
                emit(f'{x} = sea.nondet "i" [{site[0]}] : i32')
                site[0] += 1
                hi = const(size + 8 if violate else size - 4)
                c = fresh()
                emit(f"{c} = sea.icmp ule, {x}, {hi} : i32")
                emit(f"sea.assume {c}")
                p = fresh()
                emit(f"{p} = sea.ptr.add {b}, {x} : {PT}, i32")
                symptrs.append(p)
        ptrs.extend(cellptrs + symptrs)

        def access_cells():
            r = rng.random()
            if symptrs and r < 0.12:
                return rng.choice(symptrs)
            if r < 0.2:
                return rng.choice(bases)
            return rng.choice(cellptrs)

        access = access_cells
    if violate:
        # one store that surely leaves its object, at the start (the program
        # is one block: it always executes)
        b = rng.choice(bases)
        p, o, w, m = fresh(), const(sizes[b]), const(rng.randint(1, 255)), fresh()
        emit(f"{p} = sea.ptr.add {b}, {o} : {PT}, i32")
        emit(f"{m} = sea.store {mem}[{p}], {w} inplace align 1 : {M}, i32")
        mem = m
    guard = None
    if wide:  # a cell no access should touch: the assertion reads it back
        guard = rng.choice(bases)
        one = const(1)
        m = fresh()
        emit(f"{m} = sea.store {mem}[{guard}], {one} inplace align 1 : {M}, i32")
        mem = m
    for _ in range(rng.randint(14, 30) if cells else rng.randint(6, 18)):
        r = rng.random()
        if cells:
            # mostly accesses: stores 45%, loads 35%, the rest as below
            r = rng.choice([0.4] * 9 + [0.5] * 7 + [0.1, 0.8, 0.9])
        if rng.random() < 0.3:
            # a pointer chosen by a nondet condition: ites of pointers with
            # many leaves (the ARM case: more leaves than the syntactic
            # rules look at). Accesses at bases stay at bases: an ite of
            # bases (offset 0) only.
            c, p = fresh(), fresh()
            emit(f'{c} = sea.nondet "c" [{site[0]}] : i1')
            site[0] += 1
            src = bases if not any_access else ptrs
            # grow the previous one half of the time (a chain of ites)
            a = last_sel[0] if last_sel and rng.random() < 0.6 else rng.choice(src)
            emit(f"{p} = sea.select {c}, {a}, {rng.choice(src)} : {PT}")
            last_sel[:] = [p]
            ptrs.append(p)
            if not any_access:
                bases.append(p)
            continue
        if r < 0.3:
            o = (
                const(
                    rng.choice(
                        [0, 1, 2, 4, 8]
                        if wide
                        else [0, 1, 2, 3, 4, 5, 7, 8, -1, 0xFFFFFFFF]
                    )
                )
                if rng.random() < 0.6
                else nondet_offset()
            )
            p = fresh()
            src = last_sel[0] if last_sel and rng.random() < 0.5 else rng.choice(ptrs)
            emit(f"{p} = sea.ptr.add {src}, {o} : {PT}, i32")
            ptrs.append(p)
        elif r < 0.45:
            m = fresh()
            if rng.random() < 0.3:
                emit(
                    f"{m} = sea.store {mem}[{access()}], {rng.choice(ptrs)} "
                    f"inplace align 1 : {M}, {PT}"
                )
            else:
                w = const(rng.randint(0, 255))
                emit(
                    f"{m} = sea.store {mem}[{access()}], {w} inplace align 1 : {M}, i32"
                )
            mem = m
        elif r < 0.6:
            x = fresh()
            emit(f"{x} = sea.load {mem}[{access()}] align 1 : {M} -> i32")
            ints.append(x)
            i32s.append(x)
        elif r < 0.7:
            x = fresh()
            emit(f"{x} = sea.load {mem}[{access()}] align 1 : {M} -> {PT}")
            ptrs.append(x)
        elif r < 0.85:
            x = fresh()
            a, b = rng.choice(ptrs), rng.choice(ptrs)
            pred = rng.choice(["eq", "ne", "ult", "ule"])
            emit(f"{x} = sea.ptr.cmp {pred}, {a}, {b} : {PT}")
            ints.append(x)
        else:
            a, b = rng.choice(ptrs), rng.choice(ptrs)
            x, y, z = fresh(), fresh(), fresh()
            emit(f"{x} = sea.ptr.to_int {a} : {PT} to i32")
            emit(f"{y} = sea.ptr.to_int {b} : {PT} to i32")
            pred = rng.choice(["eq", "ne", "ult"])
            emit(f"{z} = sea.icmp {pred}, {x}, {y} : i32")
            ints.append(z)
    if cells:
        # the assertion reads memory back: loads of cells after all the
        # stores, compared with each other and with the values stored
        for _ in range(rng.randint(3, 6)):
            x = fresh()
            emit(f"{x} = sea.load {mem}[{access()}] align 1 : {M} -> i32")
            i32s.append(x)
        conds = []
        for _ in range(rng.randint(1, 3)):
            x, y = rng.choice(i32s[-8:]), rng.choice(i32s[-8:])
            if rng.random() < 0.3:
                y = const(rng.randint(0, 255))
            c = fresh()
            emit(f"{c} = sea.icmp {rng.choice(['eq', 'ne', 'ult'])}, {x}, {y} : i32")
            conds.append(c)
        ass = conds[0]
        for c in conds[1:]:
            z = fresh()
            emit(f"{z} = sea.{rng.choice(['and', 'or'])} {ass}, {c} : i1")
            ass = z
        emit(f"sea.assert {ass}")
        emit("sea.halt")
        return 'sea.program @"main" {\n^bb0:\n' + "\n".join(lines) + "\n}\n"
    a, b = rng.choice(ptrs), rng.choice(ptrs)
    z = fresh()
    emit(f"{z} = sea.ptr.cmp {rng.choice(['eq', 'ne'])}, {a}, {b} : {PT}")
    ass = z
    if guard and rng.random() < 0.8:
        x, one = fresh(), const(1)
        emit(f"{x} = sea.load {mem}[{guard}] align 1 : {M} -> i32")
        ass = fresh()
        emit(f"{ass} = sea.icmp eq, {x}, {one} : i32")
        emit(f"sea.assert {ass}")
        emit("sea.halt")
        return 'sea.program @"main" {\n^bb0:\n' + "\n".join(lines) + "\n}\n"
    if rng.random() < 0.5:
        one = fresh()
        ass = fresh()
        emit(f"{one} = sea.const 1 : i1")
        emit(f"{ass} = sea.xor {z}, {one} : i1")
    emit(f"sea.assert {ass}")
    emit("sea.halt")
    return 'sea.program @"main" {\n^bb0:\n' + "\n".join(lines) + "\n}\n"


def gen_ptrmem(rng):
    """The pointer-through-memory shape (review of the A-INB check): a pointer
    base+off (off nondet or a numeral that may reach another object) is
    stored in a cell, a store through another object at an in-bounds symbolic
    offset (an unresolvable store) follows, the pointer is reloaded and
    stored through, and a cell of another object is read back. The reload
    only has a pointer view when the rewriting skips the symbolic store (it
    needs A-INB for it): the access through the reloaded pointer needs its own
    A-INB condition, which an encoding without A-INB does not have."""
    lines = []
    n = [0]
    site = [1]

    def fresh():
        n[0] += 1
        return f"%v{n[0]}"

    def emit(t):
        lines.append("  " + t)

    PT = "!sea.ptr<32>"
    M = f"!sea.mem<{PT}, i8>"

    def const(v, ty="i32"):
        x = fresh()
        emit(f"{x} = sea.const {v} : {ty}")
        return x

    mem = fresh()
    emit(f'{mem} = sea.mem.nondet "r" [0] : {M}')
    objs = []
    for _ in range(rng.randint(3, 4)):
        c = const(16)
        p = fresh()
        emit(f"{p} = sea.alloca {c} align 1 [{site[0]}] : i32 -> {PT}")
        site[0] += 1
        objs.append(p)

    def nondet():
        x = fresh()
        emit(f'{x} = sea.nondet "n" [{site[0]}] : i32')
        site[0] += 1
        return x

    def add(base, off):
        p = fresh()
        emit(f"{p} = sea.ptr.add {base}, {off} : {PT}, i32")
        return p

    def store(m, addr, v, ty="i32"):
        r = fresh()
        emit(f"{r} = sea.store {m}[{addr}], {v} inplace align 1 : {M}, {ty}")
        return r

    zero = const(0)
    for o in objs:
        for k in (0, 4, 8, 12):
            mem = store(mem, add(o, const(k)), zero)
    # the pointer, stored in a cell of object c
    ci = rng.randrange(len(objs))
    src = rng.choice(objs)
    if rng.random() < 0.5:
        off = nondet()
        if rng.random() < 0.3:  # bounded in the object: A-INB holds for it
            c = fresh()
            emit(f"{c} = sea.icmp ule, {off}, {const(12)} : i32")
            emit(f"sea.assume {c}")
    else:
        off = const(rng.choice([0, 4, 12, 16, 32, 48]))
    ptr = add(src, off)
    mem = store(mem, objs[ci], ptr, PT)
    # the unresolvable store (in bounds, symbolic), assumed by the
    # check
    for _ in range(rng.randint(1, 2)):
        j = rng.randrange(len(objs))
        k = nondet()
        if rng.random() < 0.85:
            c = fresh()
            emit(f"{c} = sea.icmp ule, {k}, {const(12)} : i32")
            emit(f"sea.assume {c}")
        mem = store(mem, add(objs[j], k), const(rng.choice([5, 6])))
    # the reload, the store through it, the read of another object
    q = fresh()
    emit(f"{q} = sea.load {mem}[{objs[ci]}] align 1 : {M} -> {PT}")
    mem = store(mem, q, const(9))
    d = rng.randrange(len(objs))
    x = fresh()
    emit(
        f"{x} = sea.load {mem}[{add(objs[d], const(rng.choice([0, 4])))}] "
        f"align 1 : {M} -> i32"
    )
    c = fresh()
    emit(
        f"{c} = sea.icmp {rng.choice(['eq', 'ne', 'ult'])}, {x}, "
        f"{const(rng.choice([0, 5, 9]))} : i32"
    )
    emit(f"sea.assert {c}")
    emit("sea.halt")
    return 'sea.program @"main" {\n^bb0:\n' + "\n".join(lines) + "\n}\n"


def gen_circular(rng):
    """The A-INB circularity shape (see the module doc)."""
    lines = []
    n = [0]
    site = [1]

    def fresh():
        n[0] += 1
        return f"%v{n[0]}"

    def emit(t):
        lines.append("  " + t)

    PT = "!sea.ptr<32>"
    M = f"!sea.mem<{PT}, i8>"

    def const(v, ty="i32"):
        x = fresh()
        emit(f"{x} = sea.const {v} : {ty}")
        return x

    mem = fresh()
    emit(f'{mem} = sea.mem.nondet "r" [0] : {M}')
    objs = []
    for _ in range(rng.randint(2, 3)):
        c = const(16)
        p = fresh()
        emit(f"{p} = sea.alloca {c} align 1 [{site[0]}] : i32 -> {PT}")
        site[0] += 1
        objs.append(p)

    def nondet():
        x = fresh()
        emit(f'{x} = sea.nondet "n" [{site[0]}] : i32')
        site[0] += 1
        return x

    def store(m, addr, v):
        r = fresh()
        emit(f"{r} = sea.store {m}[{addr}], {v} inplace align 1 : {M}, i32")
        return r

    def load(m, addr):
        r = fresh()
        emit(f"{r} = sea.load {m}[{addr}] align 1 : {M} -> i32")
        return r

    def at(base, off):
        o, p = const(off), fresh()
        emit(f"{p} = sea.ptr.add {base}, {o} : {PT}, i32")
        return p

    values = [0]
    # every cell of every object starts at 0 (explicit stores: the programs
    # are about what a store through another object changes)
    zero = const(0)
    for o in objs:
        for k in (0, 4, 8, 12):
            mem = store(mem, at(o, k), zero)
    # stores through an object plus a nondet offset: the offset is
    # unconstrained, bounded loosely (it can leave the object), or bounded in
    # the object (A-INB then holds for that store)
    for _ in range(rng.randint(1, 2)):
        base = rng.choice(objs)
        off = nondet()
        r = rng.random()
        if r < 0.3:
            hi = const(12)
        elif r < 0.5:
            hi = const(rng.choice([20, 40]))
        else:
            hi = None
        if hi:
            c = fresh()
            emit(f"{c} = sea.icmp ule, {off}, {hi} : i32")
            emit(f"sea.assume {c}")
        p = fresh()
        emit(f"{p} = sea.ptr.add {base}, {off} : {PT}, i32")
        w = rng.choice([1, 2, 7, 255])
        values.append(w)
        mem = store(mem, p, const(w))
    for _ in range(rng.randint(0, 2)):  # noise: cells in bounds
        w = rng.choice([1, 3, 9])
        values.append(w)
        mem = store(mem, at(rng.choice(objs), rng.choice([0, 4, 8, 12])), const(w))
    xs = [
        load(mem, at(rng.choice(objs), rng.choice([0, 4, 8, 12])))
        for _ in range(rng.randint(1, 2))
    ]
    x = xs[0]
    guard = fresh()
    emit(f"{guard} = sea.icmp eq, {x}, {const(rng.choice(values))} : i32")
    if rng.random() < 0.3:  # the guard the other way round
        one, ng = const(1, "i1"), fresh()
        emit(f"{ng} = sea.xor {guard}, {one} : i1")
        guard = ng
    shape = rng.choice(["assume", "dead", "join"])

    def later(m):
        """after the guard: a store whose address comes from the load (masked:
        in bounds; unmasked: it may leave its object), then a reload"""
        if rng.random() < 0.5:
            return m, []
        base = rng.choice(objs)
        if rng.random() < 0.6:
            mask = const(12)
            xm = fresh()
            emit(f"{xm} = sea.and {x}, {mask} : i32")
            off = xm
        else:
            off = x
        q = fresh()
        emit(f"{q} = sea.ptr.add {base}, {off} : {PT}, i32")
        m = store(m, q, const(rng.choice([5, 6])))
        return m, [load(m, at(rng.choice(objs), 0))]

    def finish(cands, m_final=None):
        a = rng.choice(cands)
        b = rng.choice(cands + [const(rng.choice(values))])
        if a == b:
            b = const(rng.choice(values))
        c = fresh()
        emit(f"{c} = sea.icmp {rng.choice(['eq', 'ne', 'ult'])}, {a}, {b} : i32")
        emit(f"sea.assert {c}")
        emit("sea.halt")

    if shape == "assume":
        emit(f"sea.assume {guard}")
        mem, extra = later(mem)
        finish(xs + extra)
    elif shape == "dead":
        emit(f"sea.cond_br {guard}, ^go, ^dead")
        lines.append("^dead:")
        emit("sea.unreachable")
        lines.append("^go:")
        mem, extra = later(mem)
        finish(xs + extra)
    else:
        emit(f"sea.cond_br {guard}, ^t, ^e")
        lines.append("^t:")
        mask = const(12)
        xm = fresh()
        emit(f"{xm} = sea.and {x}, {mask} : i32")
        q = fresh()
        emit(f"{q} = sea.ptr.add {rng.choice(objs)}, {xm} : {PT}, i32")
        y1 = load(mem, q)
        emit(f"sea.br ^j({y1} : i32)")
        lines.append("^e:")
        y2 = const(rng.choice(values))
        emit(f"sea.br ^j({y2} : i32)")
        lines.append("^j(%r : i32):")
        finish(xs + ["%r"])
    return 'sea.program @"main" {\n^bb0:\n' + "\n".join(lines) + "\n}\n"


FPT = "!sea.ptr<64>"
FM = f"!sea.mem<{FPT}, i8>"


def gen_forms(rng):
    """--accesses=addr-forms: an abstract program over symbolic addresses
    (obj, ((coef, idx), ...), const), NKI-shaped (docs: the module doc).
    Data objects of 8-48 bytes, an index tile `ix` of i32 cells and its
    copy `sb` (a memmove, as NKI's dma to sbuf); each index is a nondet
    or an ix cell (sext, assumed in [0, b)); every address stays in its
    object. Accesses: few stores, loads, memmoves of 1-4 bytes whose
    source is also read directly (before the copy) and whose destination
    is read back (gather: the expected value vs the copied one); new
    addresses are often an earlier one plus a small constant."""
    ops = []
    objs = {}
    for k in range(rng.randint(2, 4)):
        objs[f"o{k}"] = rng.choice([8, 16, 24, 32, 48])
        ops.append(("obj", f"o{k}", objs[f"o{k}"]))
    # the index object (read only): i32 cells, NKI's index tiles
    nidx = rng.randint(1, 3)
    ops.append(("obj", "ix", 4 * nidx))
    ops.append(("obj", "sb", 4 * nidx))
    ops.append(("copyix", 4 * nidx))
    idxs = {}
    for k in range(nidx):
        b = rng.randint(2, 4)
        kind = rng.choice(["load", "load", "nondet"])
        idxs[f"i{k}"] = b
        ops.append(("idx", f"i{k}", kind, 4 * k, b))
    addrs = []
    loads = []
    pairs = []

    def addr(w, base=None):
        for _ in range(20):
            if base is not None and rng.random() < 0.6:
                o, terms, c = base
                c2 = c + rng.randint(-2, 3)
                mx = sum(cf * (idxs[i] - 1) for cf, i in terms)
                if c2 >= 0 and c2 + mx + w <= objs[o]:
                    return (o, terms, c2)
            o = rng.choice(list(objs))
            nt = rng.choice([0, 1, 1, 1, 2])
            terms = tuple(
                sorted(
                    (rng.choice([1, 2, 3, 4, 8]), rng.choice(list(idxs)))
                    for _ in range(nt)
                )
            )
            mx = sum(cf * (idxs[i] - 1) for cf, i in terms)
            room = objs[o] - w - mx
            if room >= 0:
                return (o, terms, rng.randint(0, room))
        return (o, (), 0) if objs[o] >= w else None

    def pick_old():
        return rng.choice(addrs) if addrs and rng.random() < 0.6 else None

    for _ in range(rng.randint(5, 14)):
        r = rng.random()
        if r < 0.1:
            w = rng.choice([1, 1, 4])
            a = addr(w, pick_old())
            if a is None:
                continue
            ty = f"i{8 * w}"
            ops.append(("store", a, ("const", rng.randint(0, 255)), ty))
            addrs.append(a)
        elif r < 0.4:
            w = rng.choice([1, 1, 4])
            a = addr(w, pick_old())
            if a is None:
                continue
            name = f"l{len(loads)}"
            loads.append((name, f"i{8 * w}"))
            ops.append(("load", name, a, f"i{8 * w}"))
            addrs.append(a)
        else:
            n = rng.randint(1, 4)
            d, s = addr(n, pick_old()), addr(n, pick_old())
            if d is None or s is None:
                continue
            # NKI's spec: bytes k of the source read directly (the expected
            # values), before the copy, and the same bytes read back at the
            # destination after it (gather: expected vs actual, one compare each)
            ks = rng.sample(range(n), rng.randint(0, n))
            pre = []
            for k in ks:
                if rng.random() < 0.8:
                    name = f"l{len(loads)}"
                    loads.append((name, "i8"))
                    ops.append(("load", name, (s[0], s[1], s[2] + k), "i8"))
                    pre.append((k, name))
            ops.append(("memmove", d, s, n))
            addrs += [d, s]
            for k in ks:
                if rng.random() < 0.8:
                    name = f"l{len(loads)}"
                    loads.append((name, "i8"))
                    ops.append(("load", name, (d[0], d[1], d[2] + k), "i8"))
                    for k2, e in pre:
                        if k2 == k:
                            pairs.append((e, name))
    # the assertion: the expected-vs-copied pairs, and compares of loads of
    # one type with each other / numerals
    conds = [("eq", e, a, "i8") for e, a in pairs if rng.random() < 0.7]
    for _ in range(rng.randint(0 if conds else 1, 2)):
        if not loads:
            break
        a, ty = rng.choice(loads)
        same = [x for x, t in loads if t == ty]
        b = rng.choice(same) if rng.random() < 0.6 else ("const", rng.randint(0, 3))
        conds.append((rng.choice(["eq", "ne", "ule"]), a, b, ty))
    ops.append(("assert", conds))
    return ops


FORM_STYLES = ["flat", "chain", "index", "nested", "premul"]


class FormRender:
    """Text of an abstract program; `style` = an address style for every
    access, or "mixed" (each occurrence its own style, operands commuted,
    constants split at random); bytes: memmove as byte loads then stores."""

    def __init__(self, style, rng, bytes_=False, reload=False, via="direct"):
        self.style, self.rng, self.bytes, self.reload = style, rng, bytes_, reload
        self.via = via  # direct | copy | mixed: which of an index's two equal values a use takes
        self.lines, self.n, self.consts = [], 0, {}

    def fresh(self):
        self.n += 1
        return f"%v{self.n}"

    def emit(self, s):
        self.lines.append("  " + s)

    def const(self, v, ty="i64"):
        x = self.fresh()
        self.emit(f"{x} = sea.const {v} : {ty}")
        return x

    def mixed(self):
        return self.style == "mixed" and self.rng.random() < 0.5

    def add(self, a, b):
        if self.mixed():
            a, b = b, a
        x = self.fresh()
        self.emit(f"{x} = sea.add {a}, {b} : i64")
        return x

    def mul(self, a, b):
        if self.mixed():
            a, b = b, a
        x = self.fresh()
        self.emit(f"{x} = sea.mul {a}, {b} : i64")
        return x

    def term(self, cf, i):
        v = self.idxval(i)
        return v if cf == 1 and self.mixed() else self.mul(v, self.const(cf))

    def idxval(self, i):
        if self.idx[i][0] == "load" and (
            self.via == "copy" or (self.via == "mixed" and self.rng.random() < 0.5)
        ):
            return self.idx[i][3]
        if self.reload and self.idx[i][0] == "load" and self.rng.random() < 0.5:
            x, y = self.fresh(), self.fresh()
            self.emit(
                f"{x} = sea.load {self.mem}[{self.idx[i][2]}] align 4 : {FM} -> i32"
            )
            self.emit(f"{y} = sea.sext {x} : i32 to i64")
            return y
        return self.idx[i][1]

    def ptradd(self, p, o):
        x = self.fresh()
        self.emit(f"{x} = sea.ptr.add {p}, {o} : {FPT}, i64")
        return x

    def pindex(self, p, terms, c):
        x = self.fresh()
        ix = ", ".join(self.idxval(i) for _, i in terms)
        st = ", ".join(str(cf) for cf, _ in terms)
        tys = "".join(", i64" for _ in terms)
        self.emit(
            f"{x} = sea.ptr.index {p} [{ix}] strides [{st}] offset {c} : {FPT}{tys}"
        )
        return x

    def cadd(self, p, c, ptr=True):
        """p + c (pointer or integer), c maybe split in two"""
        parts = [c]
        if self.style == "mixed" and c > 1 and self.rng.random() < 0.5:
            k = self.rng.randint(1, c - 1)
            parts = [k, c - k]
        for k in parts:
            p = self.ptradd(p, self.const(k)) if ptr else self.add(p, self.const(k))
        return p

    def address(self, a):
        o, terms, c = a
        base = self.objs[o]
        terms = list(terms)
        st = self.style
        if st == "mixed":
            st = self.rng.choice(FORM_STYLES)
            self.rng.shuffle(terms)
        if st == "flat" or (st in ("index", "nested") and not terms and st == "flat"):
            if not terms and c == 0:
                return (
                    base
                    if self.style != "mixed" or self.rng.random() < 0.5
                    else self.ptradd(base, self.const(0))
                )
            s = None
            for cf, i in terms:
                t = self.term(cf, i)
                s = t if s is None else self.add(s, t)
            if s is None:
                s = self.const(c)
            elif c:
                s = (
                    self.cadd(s, c, ptr=False)
                    if self.style == "mixed"
                    else self.add(s, self.const(c))
                )
            return self.ptradd(base, s)
        if st == "chain":
            p = base
            for cf, i in terms:
                p = self.ptradd(p, self.term(cf, i))
            return self.cadd(p, c) if c else p
        if st == "index":
            return self.pindex(base, terms, c)
        if st == "nested":
            if not terms:
                return self.pindex(self.pindex(base, [], 0), [], c)
            p = base
            for k, t in enumerate(terms):
                p = self.pindex(p, [t], c if k == len(terms) - 1 else 0)
            return p
        # premul: one index of stride 1, the products computed
        if not terms:
            return self.pindex(base, [], c)
        s = None
        for cf, i in terms:
            t = self.mul(self.idxval(i), self.const(cf))
            s = t if s is None else self.add(s, t)
        x = self.fresh()
        self.emit(
            f"{x} = sea.ptr.index {base} [{s}] strides [1] offset {c} : {FPT}, i64"
        )
        return x

    def program(self, ops):
        self.objs, self.idx, self.vals = {}, {}, {}
        self.mem = self.fresh()
        self.emit(f'{self.mem} = sea.mem.nondet "region.0" [0] : {FM}')
        site = [1]
        for op in ops:
            k = op[0]
            if k == "obj":
                p, c = self.fresh(), self.const(op[2])
                self.emit(f"{p} = sea.alloca {c} align 8 [{site[0]}] : i64 -> {FPT}")
                site[0] += 1
                self.objs[op[1]] = p
            elif k == "copyix":
                m = self.fresh()
                self.emit(
                    f"{m} = sea.memmove {self.mem}[{self.objs['sb']}], {self.mem}[{self.objs['ix']}], "
                    f"{self.const(op[1])} inplace : {FM}"
                )
                self.mem = m
            elif k == "idx":
                _, name, kind, off, b = op
                if kind == "load":
                    a = (
                        self.objs["ix"]
                        if off == 0
                        else self.ptradd(self.objs["ix"], self.const(off))
                    )
                    x, y = self.fresh(), self.fresh()
                    self.emit(f"{x} = sea.load {self.mem}[{a}] align 4 : {FM} -> i32")
                    self.emit(f"{y} = sea.sext {x} : i32 to i64")
                    lo, hi = self.fresh(), self.fresh()
                    self.emit(f"{lo} = sea.icmp sge, {y}, {self.const(0)} : i64")
                    self.emit(f"{hi} = sea.icmp slt, {y}, {self.const(b)} : i64")
                    ok = self.fresh()
                    self.emit(f"{ok} = sea.and {lo}, {hi} : i1")
                    self.emit(f"sea.assume {ok}")
                    # the same cell read through the copy of the index tile (NKI: dma to sbuf)
                    ca = (
                        self.objs["sb"]
                        if off == 0
                        else self.ptradd(self.objs["sb"], self.const(off))
                    )
                    x2, y2 = self.fresh(), self.fresh()
                    self.emit(f"{x2} = sea.load {self.mem}[{ca}] align 4 : {FM} -> i32")
                    self.emit(f"{y2} = sea.sext {x2} : i32 to i64")
                    self.idx[name] = ("load", y, a, y2)
                else:
                    y = self.fresh()
                    self.emit(f'{y} = sea.nondet "i" [{site[0]}] : i64')
                    site[0] += 1
                    ok = self.fresh()
                    self.emit(f"{ok} = sea.icmp ult, {y}, {self.const(b)} : i64")
                    self.emit(f"sea.assume {ok}")
                    self.idx[name] = ("nondet", y, None)
            elif k == "store":
                _, a, v, ty = op
                p = self.address(a)
                w = self.const(v[1], ty)
                m = self.fresh()
                self.emit(
                    f"{m} = sea.store {self.mem}[{p}], {w} inplace align 1 : {FM}, {ty}"
                )
                self.mem = m
            elif k == "load":
                _, name, a, ty = op
                p = self.address(a)
                x = self.fresh()
                self.emit(f"{x} = sea.load {self.mem}[{p}] align 1 : {FM} -> {ty}")
                self.vals[name] = x
            elif k == "memmove":
                _, d, s, n = op
                if self.bytes:
                    bs = []
                    for j in range(n):
                        p = self.address((s[0], s[1], s[2] + j))
                        x = self.fresh()
                        self.emit(
                            f"{x} = sea.load {self.mem}[{p}] align 1 : {FM} -> i8"
                        )
                        bs.append(x)
                    for j in range(n):
                        p = self.address((d[0], d[1], d[2] + j))
                        m = self.fresh()
                        self.emit(
                            f"{m} = sea.store {self.mem}[{p}], {bs[j]} inplace align 1 : {FM}, i8"
                        )
                        self.mem = m
                else:
                    pd, ps = self.address(d), self.address(s)
                    m = self.fresh()
                    self.emit(
                        f"{m} = sea.memmove {self.mem}[{pd}], {self.mem}[{ps}], {self.const(n)} inplace : {FM}"
                    )
                    self.mem = m
            elif k == "assert":
                acc = None
                for pred, a, b, ty in op[1]:
                    bv = self.const(b[1], ty) if isinstance(b, tuple) else self.vals[b]
                    c = self.fresh()
                    self.emit(f"{c} = sea.icmp {pred}, {self.vals[a]}, {bv} : {ty}")
                    if acc is None:
                        acc = c
                    else:
                        z = self.fresh()
                        self.emit(f"{z} = sea.and {acc}, {c} : i1")
                        acc = z
                if acc is None:
                    acc = self.const(1, "i1")
                self.emit(f"sea.assert {acc}")
                self.emit("sea.halt")
        return 'sea.program @"main" {\n^bb0:\n' + "\n".join(self.lines) + "\n}\n"


RUN_TIMEOUT = 120


def run(sea_vc, path, flags, solver):
    # a run over the budget is a timeout result (a failure of the program,
    # reported), not a crash of the fuzzer
    try:
        r = subprocess.run(
            [sea_vc, path, "--lower", f"--solver={solver}", "--widen-memory=0"] + flags,
            capture_output=True,
            text=True,
            timeout=RUN_TIMEOUT,
        )
    except subprocess.TimeoutExpired:
        return "timeout", -1, f"timed out after {RUN_TIMEOUT} s", ""
    m = re.search(r"^result: (\S+)", r.stdout, re.M)
    return (m.group(1) if m else "none"), r.returncode, r.stderr, r.stdout


COUNTERS = [
    "wow.commute",
    "smap.commute",
    "cmp.diff-base",
    "arm.skip-diff-base",
    "row.smap-skip",
    "premise.a-inb",
    "wide.hit",
    "wide.sub",
    "wide.skip",
    "wide.memop-hit",
    "wide.expanded",
    "row.smap-abort-stride",
    "premise.ptr-index",
]


def counter(out, name):
    m = re.search(r"^seam\.rule\." + re.escape(name) + r": (\d+)", out, re.M)
    return int(m.group(1)) if m else 0


def inb_state(out):
    """What the answer says about A-INB: 'fallback' (the static check of numeral
    offsets turned it off), 'off' (not asked: --seam-inb=0), 'assumed'."""
    m = re.search(r"^seam\.inb: (off|assumed)(.*)$", out, re.M)
    if not m:
        return "none"
    if m.group(1) == "off":
        return "fallback" if "fallback" in m.group(2) else "off"
    return "assumed"


def leaves_object(sea_run, path, plain_out):
    """Does the counterexample of the plain VC (its nondet values) make the
    abstract program access memory outside an object? True / False / None
    (undetermined: the run stops for another reason)."""
    # --missing=error: only the scalar nondets of the counterexample are known;
    # a run that needs a memory cell is not that witness (undetermined)
    args = [sea_run, path, "--missing=error"]
    for m in re.finditer(r"^nondet: site=(\d+) .* value=(\d+)", plain_out, re.M):
        args.append(f"--input={m.group(1)}={m.group(2)}")
    r = subprocess.run(args, capture_output=True, text=True, timeout=120)
    if "memory access out of bounds" in r.stdout + r.stderr:
        return True
    st = re.search(r"^status: (\S+)", r.stdout, re.M)
    if st and st.group(1) in ("halted", "assert-failed"):
        return False
    return None


# --accesses=addr-forms: the renderings of one abstract program. The first is
# the reference; every other must give the same FORM_KEYS. bytes (memmove as
# byte loads then stores: a different memory program) is held to the verdict
# only; its counter deltas are reported (memop-expansion-deltas), not failed.
FORM_VARIANTS = [
    # name, style, via, reload, bytes, extra sea-vc flags
    ("ref", "flat", "direct", False, False, []),
    ("ref-noopt", "flat", "direct", False, False, ["--lower-opt=false"]),
    ("chain", "chain", "direct", False, False, []),
    ("index", "index", "direct", False, False, []),
    ("nested", "nested", "direct", False, False, []),
    ("premul", "premul", "direct", False, False, []),
    ("via-copy", "flat", "copy", False, False, []),
    ("mixed", "mixed", "mixed", False, False, []),
    ("mixed-noopt", "mixed", "mixed", False, False, ["--lower-opt=false"]),
    ("mixed-reload", "mixed", "mixed", True, False, []),
    ("bytes", "mixed", "mixed", False, True, []),
]
FORM_KEYS = [
    "result",
    "exit",
    "vc.ackermann-arrays",
    "vc.ackermann-reads",
    "vc.ackermann-constraints",
    "seam.inb",
    "seam.alias.open.all",
]


def form_stats(r):
    """FORM_KEYS of a run (BRUNCH_STAT lines, else `key: value` lines)."""
    verdict, rc, err, out = r
    d = {"result": verdict, "exit": rc}
    for k in FORM_KEYS[2:]:
        m = re.search(
            r"^BRUNCH_STAT " + re.escape(k) + r" (\S+)", out + err, re.M
        ) or re.search(r"^" + re.escape(k) + r": (\S+)", out, re.M)
        d[k] = m.group(1) if m else "-"
    return d


def forms_main(args):
    """The metamorphic address-form mode: one abstract program, rendered in
    each FORM_VARIANTS form; the SeaM resolution counters must not depend on
    the form. A difference is a finding: the reference and the variant are
    kept (forms-<seed>-<i>-{ref,<variant>}.mlir + .txt, the commands and the
    counters) in --keep (default: a temp directory, printed)."""
    global RUN_TIMEOUT
    RUN_TIMEOUT = args.run_timeout
    keep = args.keep or tempfile.mkdtemp(prefix="fuzz-seam-forms-")
    os.makedirs(keep, exist_ok=True)
    work = tempfile.mkdtemp(prefix="fuzz-seam-forms-run-")
    flags = ["--seam", "--seam-stats", "--brunch-stats"]
    paths = []
    for i in range(args.n):
        rng = random.Random(f"{args.seed}-{i}")
        ops = gen_forms(rng)
        for k, (name, style, via, reload, bytes_, extra) in enumerate(FORM_VARIANTS):
            text = FormRender(
                style, random.Random(f"{args.seed}-{i}-{k}"), bytes_, reload, via
            ).program(ops)
            path = os.path.join(work, f"forms-{args.seed}-{i}-{name}.mlir")
            with open(path, "w") as f:
                f.write(text)
            paths.append((i, name, path, flags + extra))
    with concurrent.futures.ThreadPoolExecutor(max(1, args.j)) as pool:
        outs = list(
            pool.map(lambda t: run(args.sea_vc, t[2], t[3], args.solver), paths)
        )
    res = {}
    for (i, name, path, fl), r in zip(paths, outs):
        res.setdefault(i, {})[name] = (path, fl, form_stats(r))
    bad, first, deltas, tally, fewer = 0, None, 0, {}, []
    for i in range(args.n):
        ref = res[i]["ref"][2]
        tally[ref["result"]] = tally.get(ref["result"], 0) + 1
        probs = []
        for name, (path, fl, st) in res[i].items():
            if name == "ref":
                continue
            keys = ["result"] if name == "bytes" else FORM_KEYS
            d = [k for k in keys if st[k] != ref[k]]
            if (
                name == "bytes"
                and not d
                and any(st[k] != ref[k] for k in FORM_KEYS[2:])
            ):
                deltas += 1
                try:  # the byte copy needs fewer constraints than the memmove
                    if int(st["vc.ackermann-constraints"]) < int(
                        ref["vc.ackermann-constraints"]
                    ):
                        fewer.append(i)
                except ValueError:
                    pass
            if st["exit"] not in (0,) and name != "bytes" and "exit" not in d:
                d.append("exit")
            if d:
                probs.append((name, path, fl, st, d))
        if ref["exit"] != 0:
            probs.append(("ref", res[i]["ref"][0], res[i]["ref"][1], ref, ["exit"]))
        if not probs:
            continue
        bad += 1
        first = i if first is None else first
        base = os.path.join(keep, f"forms-{args.seed}-{i}")
        shutil.copy(res[i]["ref"][0], base + "-ref.mlir")
        with open(base + ".txt", "w") as f:
            f.write(
                f"ref: sea-vc {base}-ref.mlir --lower --solver={args.solver} --widen-memory=0 "
                f"{' '.join(res[i]['ref'][1])}\nref: {ref}\n"
            )
            for name, path, fl, st, d in probs:
                shutil.copy(path, f"{base}-{name}.mlir")
                f.write(
                    f"{name}: sea-vc {base}-{name}.mlir --lower --solver={args.solver} "
                    f"--widen-memory=0 {' '.join(fl)}\n{name}: {st} differs={','.join(d)}\n"
                )
        print(
            f"FAIL {base}: "
            + "; ".join(
                f"{name} " + " ".join(f"{k}={ref[k]}->{st[k]}" for k in d)
                for name, _, _, st, d in probs
            ),
            flush=True,
        )
    print(f"programs: {args.n}")
    print(f"variants: {len(FORM_VARIANTS)}")
    for v, c in sorted(tally.items()):
        print(f"verdict-{v}: {c}")
    print(f"memop-expansion-deltas: {deltas}")
    # a hint, not a failure: the memmove form left constraints the byte form
    # resolves (programs listed: look at them by hand)
    print(
        f"memop-expansion-fewer-constraints: {len(fewer)} {' '.join(map(str, fewer[:20]))}"
    )
    print(f"first-finding: {'none' if first is None else first}")
    print(f"findings-dir: {keep if bad else 'none'}")
    print(f"failures: {bad}")
    shutil.rmtree(work, ignore_errors=True)
    if not args.keep and not bad:
        try:
            os.rmdir(keep)
        except OSError:
            pass
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sea-vc", default="sea-vc")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("-n", type=int, default=50, help="programs")
    ap.add_argument(
        "--run-timeout",
        type=int,
        default=120,
        help="seconds per sea-vc run; over it the run is a "
        "timeout (reported as a failure)",
    )
    ap.add_argument(
        "-j",
        type=int,
        default=1,
        help="run the sea-vc runs (programs x variants) on J "
        "workers; the programs, their order and the report "
        "are unchanged",
    )
    ap.add_argument(
        "--accesses",
        choices=[
            "bases",
            "any",
            "wide",
            "cells",
            "cells-violate",
            "circular",
            "ptrmem",
            "addr-forms",
        ],
        default="bases",
    )
    ap.add_argument("--solver", default="yices")
    ap.add_argument("--sea-run", help="sea-run (default: next to sea-vc)")
    ap.add_argument("--keep", help="directory to keep failing programs")
    ap.add_argument(
        "--seam-check",
        action="store_true",
        help="run every --seam variant with --seam-check",
    )
    ap.add_argument(
        "--ptr-index",
        action="store_true",
        help="write each ptr.add as the same address by "
        "sea.ptr.index (stride 1, a random promise: none, "
        "inbounds or nuw; docs/design/PtrIndex.md)",
    )
    ap.add_argument(
        "--ptr-index-strides",
        default="1",
        help="with --ptr-index: the strides to draw from, "
        "comma-separated (default 1: the same address; "
        "others change the program, e.g. to exercise "
        "--seam-stride)",
    )
    ap.add_argument(
        "--extra-args",
        default="",
        help="more sea-vc flags for every --seam variant "
        "(whitespace-separated, e.g. --seam-stride)",
    )
    args = ap.parse_args()
    if args.accesses == "addr-forms":
        return forms_main(args)
    if not args.sea_run:
        d = os.path.dirname(args.sea_vc)
        args.sea_run = os.path.join(d, "sea-run") if d else "sea-run"
    rng = random.Random(args.seed)
    workdir = args.keep or tempfile.mkdtemp(prefix="fuzz-seam-")
    os.makedirs(workdir, exist_ok=True)
    mode = args.accesses
    wide = mode == "wide"
    cells = mode in ("cells", "cells-violate")
    violate = mode == "cells-violate"
    # modes whose accesses stay in their objects: every variant agrees and a
    # run that falls back is a failure; the others are violating modes
    in_bounds = mode in ("bases", "cells")
    stats = (
        ["--seam-stats"]
        + (["--seam-check"] if args.seam_check else [])
        + args.extra_args.split()
    )
    check_keys = ["firings", "checked", "valid", "invalid", "unknown", "unexpressible"]
    check_totals = {k: 0 for k in check_keys}
    variants = [
        ("plain", []),
        ("seam", ["--seam"] + stats),
        ("seam-noarm", ["--seam", "--seam-arm=0"] + stats),
        ("seam-nosmap", ["--seam", "--seam-store-map=0"] + stats),
        ("seam-inb0", ["--seam", "--seam-inb=0"] + stats),
        # width terms (docs/design/SeaM-mixed-width.md): the i32 /
        # pointer accesses of byte memories stay whole
        ("seam-wide", ["--seam", "--seam-wide"] + stats),
        ("seam-wide-inb0", ["--seam", "--seam-wide", "--seam-inb=0"] + stats),
        # initial words (docs/design/SeaM-initial-words.md): every
        # eligible initial memory as words, the word query checked
        # against the byte query (--words-check: exit 2 if they
        # differ)
        (
            "seam-words",
            ["--seam", "--seam-wide", "--seam-words=all", "--words-check"] + stats,
        ),
        (
            "seam-words-inb0",
            [
                "--seam",
                "--seam-wide",
                "--seam-inb=0",
                "--seam-words=all",
                "--words-check",
            ]
            + stats,
        ),
    ]
    # the variants that assume A-INB
    asks_inb = [n for n, f in variants if "--seam" in f and "--seam-inb=0" not in f]
    bad = 0
    differs = 0
    oob = {True: 0, False: 0, None: 0}
    fired = 0
    smap_fired = 0
    tally = {}
    totals = {c: 0 for c in COUNTERS}
    programs_with = {c: 0 for c in COUNTERS}
    fallbacks = 0
    global RUN_TIMEOUT
    RUN_TIMEOUT = args.run_timeout
    # the programs, generated in order (deterministic for a seed)
    paths = []
    for i in range(args.n):
        if mode == "circular":
            text = gen_circular(rng)
        elif mode == "ptrmem":
            text = gen_ptrmem(rng)
        else:
            text = gen(rng, mode in ("any", "wide"), wide, cells, violate)
        if args.ptr_index:
            text = as_ptr_index(
                text,
                random.Random(args.seed * 7919 + i),
                tuple(int(x) for x in args.ptr_index_strides.split(",")),
            )
        path = os.path.join(workdir, f"fuzz-seam-{args.seed}-{i}.mlir")
        with open(path, "w") as f:
            f.write(text)
        paths.append(path)
    # every (program, variant) run, on J workers; results read in order
    jobs = [(path, name, flags) for path in paths for name, flags in variants]
    with concurrent.futures.ThreadPoolExecutor(max(1, args.j)) as pool:
        outs = list(
            pool.map(lambda job: run(args.sea_vc, job[0], job[2], args.solver), jobs)
        )
    results = {}
    for (path, name, _), r in zip(jobs, outs):
        results.setdefault(path, {})[name] = r
    for path in paths:
        res = results[path]
        verdicts = {name: r[0] for name, r in res.items()}
        out = res["seam"][3]
        m = re.search(r"seam\.rule\.arm\.skip: (\d+)", out)
        fired += 1 if m and int(m.group(1)) > 0 else 0
        smap_fired += (
            1
            if re.search(r"seam\.rule\.(smap\.new|row\.smap-)[a-z-]*: [1-9]", out)
            else 0
        )
        # the final run of the seam-check variant
        state = inb_state(out)
        if state == "fallback":
            fallbacks += 1
        else:
            for c in COUNTERS:
                # the width-term counters from the seam-wide variant
                v = counter(res["seam-wide"][3] if c.startswith("wide.") else out, c)
                totals[c] += v
                programs_with[c] += 1 if v else 0
        tally[verdicts["plain"]] = tally.get(verdicts["plain"], 0) + 1
        problems = []
        if args.seam_check:
            for n, r in res.items():
                if "--seam" not in dict(variants)[n]:
                    continue
                m = re.search(r"^seam-check: (.*)$", r[3], re.M)
                if not m:
                    if r[1] not in (1,):
                        problems.append(f"{n}: no seam-check line")
                    continue
                kv = dict(x.split("=") for x in m.group(1).split())
                for k in check_keys:
                    check_totals[k] += int(kv.get(k, 0))
                if int(kv.get("invalid", 0)):
                    problems.append(f"{n}: seam-check invalid firing")
        # a --seam run of a violating mode answers conditionally on A-INB: it
        # may differ from plain, and its sat may not replay: exit 4 when
        # sea-vc finds the abstract run leaving an object, exit 2 otherwise
        conditional = {n for n in asks_inb} if not in_bounds else set()
        if any(r[1] != 0 for n, r in res.items() if n not in conditional):
            problems.append("exit status")
        if any(r[1] not in (0, 2, 4) for n, r in res.items() if n in conditional):
            problems.append("exit status")
        unconditional = {n: v for n, v in verdicts.items() if n not in conditional}
        if len(set(unconditional.values())) > 1:
            problems.append("verdicts differ")
        # the agreement of plain and --seam-inb=0 holds only without A-INB:
        # no dropped memhavoc may rest on it (the generator makes none)
        for n, r in res.items():
            if re.search(r"^assume\.a-inb:", r[3], re.M):
                problems.append(f"{n}: the lowering rests on A-INB (assume.a-inb)")
        for name in asks_inb:
            if in_bounds and inb_state(res[name][3]) == "fallback":
                problems.append(
                    f"{name} fell back to no A-INB: an access left its object"
                )
        if conditional:
            if verdicts["seam"] != verdicts["plain"]:
                differs += 1
            if verdicts["seam"] == "unsat" and verdicts["plain"] == "sat":
                leaves = leaves_object(args.sea_run, path, res["plain"][3])
                oob[leaves] += 1
                if leaves is False:
                    problems.append(
                        "--seam unsat, plain sat, and the "
                        "counterexample of plain stays in "
                        "bounds"
                    )
        failed = bool(problems)
        if failed:
            bad += 1
            print(
                f"FAIL {path}: "
                + "; ".join(problems)
                + " | "
                + " ".join(f"{n}={r[0]}(exit {r[1]})" for n, r in res.items())
            )
        if not failed and not args.keep:
            os.remove(path)
    print(f"programs: {args.n}")
    for v, c in sorted(tally.items()):
        print(f"verdict-{v}: {c}")
    if not in_bounds:
        print(f"seam-differs: {differs}")
        print(f"oob-confirmed: {oob[True]}")
        print(f"oob-undetermined: {oob[None]}")
    print(f"arm-fired: {fired}")
    print(f"smap-fired: {smap_fired}")
    # the final run of the seam-check variant (a run that fell back has the
    # counters of the encoding without A-INB: counted apart)
    print("final-run counts, seam, programs that kept A-INB:")
    for c in COUNTERS:
        print(f"{c}: {totals[c]} (programs {programs_with[c]})")
    print(
        f"cross-base-skips: "
        f"{totals['cmp.diff-base'] + totals['arm.skip-diff-base'] + totals['row.smap-skip']}"
    )
    print(f"fallbacks: {fallbacks}")
    if args.seam_check:
        for k in check_keys:
            print(f"seam-check-{k}: {check_totals[k]}")
    print(f"failures: {bad}")
    if not args.keep:
        try:
            os.rmdir(workdir)
        except OSError:
            pass
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

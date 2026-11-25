# SPDX-FileCopyrightText: 2026 The sea-mlir Authors
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""A global initializer in SeaHorn's export form: a table of N entries of
struct {u16 fstate; u8 sym;} (4 bytes, 1 byte of padding), one 3-byte
sea.mem.fill per entry at stride 4 (the padding byte is not written), as
SeaHorn exports nghttp2's huff_decode_table. Programs:
  safe:    entry i (nondet, i < N) holds its data: unsat;
  shifted: entry i holds the data of entry i + 1: sat;
  pad:     the padding byte of entry i is 0: sat (no fill writes it).
--erase prints the seaimp form (seaimp.mem.fill on a region variable),
as SeaHorn's --horn-sea-mlir-mem-ssa=erase does."""

import sys

N = 64
ERASE = "--erase" in sys.argv


def fstate(k):
    return (k * 37 + 5) & 0x1FF


def sym(k):
    return (k * 11) & 0xFF


def program(name, check):
    out = [f"sea.program @{name} {{"]
    if ERASE:
        out.append('  %mem = seaimp.var "mem" [0] : !v')
        out.append(f"  %g = seaimp.global %mem @tab size {4 * N} align 16 : !v -> !p")
    else:
        out.append('  %m0 = sea.mem.nondet "mem" [0] : !m')
        out.append(f"  %g = sea.global @tab size {4 * N} align 16 : !p")
    for k in range(N):
        f, s = fstate(k), sym(k)
        data = f"dense<[{f & 0xFF}, {f >> 8}, {s}]> : tensor<3xi8>"
        out.append(f"  %o{k} = sea.const {4 * k} : i64")
        out.append(f"  %a{k} = sea.ptr.add %g, %o{k} : !p, i64")
        if ERASE:
            out.append(f"  seaimp.mem.fill %mem[%a{k}], {data} : !v")
        else:
            out.append(f"  %m{k + 1} = sea.mem.fill %m{k}[%a{k}], {data} inplace : !m")
    last = "%mem" if ERASE else f"%m{N}"
    load = "seaimp.load" if ERASE else "sea.load"
    ty = "!v" if ERASE else "!m"
    out += [
        '  %i = sea.nondet "i" [1] : i64',
        f"  %n = sea.const {N} : i64",
        "  %lt = sea.icmp ult, %i, %n : i64",
        "  sea.assume %lt",
        "  %c4 = sea.const 4 : i64",
        "  %off = sea.mul %i, %c4 : i64",
        "  %e = sea.ptr.add %g, %off : !p, i64",
    ]
    out += check(last, load, ty)
    out += ["  sea.assert %ok", "  sea.halt", "}"]
    return "\n".join(out)


def entry(shift):
    def check(last, load, ty):
        return [
            f"  %fs = {load} {last}[%e] align 4 : {ty} -> i16",
            "  %c2 = sea.const 2 : i64",
            "  %sp = sea.ptr.add %e, %c2 : !p, i64",
            f"  %sy = {load} {last}[%sp] : {ty} -> i8",
            # the data of entry j = i + shift: fstate (37 j + 5) & 0x1ff,
            # sym 11 j mod 256
            f"  %sh = sea.const {shift} : i64",
            "  %j = sea.add %i, %sh : i64",
            "  %c37 = sea.const 37 : i64",
            "  %c5 = sea.const 5 : i64",
            "  %c511 = sea.const 511 : i64",
            "  %f1 = sea.mul %j, %c37 : i64",
            "  %f2 = sea.add %f1, %c5 : i64",
            "  %f3 = sea.and %f2, %c511 : i64",
            "  %fw = sea.trunc %f3 : i64 to i16",
            "  %c11 = sea.const 11 : i64",
            "  %s1 = sea.mul %j, %c11 : i64",
            "  %sw = sea.trunc %s1 : i64 to i8",
            "  %okf = sea.icmp eq, %fs, %fw : i16",
            "  %oks = sea.icmp eq, %sy, %sw : i8",
            "  %ok = sea.and %okf, %oks : i1",
        ]

    return check


def pad(last, load, ty):
    return [
        "  %c3 = sea.const 3 : i64",
        "  %pp = sea.ptr.add %e, %c3 : !p, i64",
        f"  %pv = {load} {last}[%pp] : {ty} -> i8",
        "  %z = sea.const 0 : i8",
        "  %ok = sea.icmp eq, %pv, %z : i8",
    ]


print("!m = !sea.mem<!sea.ptr<64>, i8>")
print("!p = !sea.ptr<64>")
if ERASE:
    print("!v = !seaimp.var<!sea.mem<!sea.ptr<64>, i8>>")
print(program("safe", entry(0)))
print(program("shifted", entry(1)))
print(program("pad", pad))

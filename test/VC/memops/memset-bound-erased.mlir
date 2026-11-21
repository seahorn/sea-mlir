// assume.memset-bound is counted where sea-expand-memset makes the bound:
// here the length (20 or 30) never fits K = 16, the lowering blocks every
// path and erases the expansion with its assume, and the unsat rests on
// the bound all the same.
// RUN: sea-vc %s --program=phi --lower 2>&1 | FileCheck %s
// CHECK: assume.memset-bound: count=1 bound=16
// CHECK: result: unsat
//
// one bound per expanded op: two memsets and a memmove of symbolic length
// RUN: sea-vc %s --program=three --lower --vcgen-only 2>&1 | FileCheck %s --check-prefix=THREE
// THREE: assume.memset-bound: count=3 bound=16
//
// a program lowered by an older sea-mlir marks each assume with the i64
// bound (no program dictionary): still counted
// RUN: sea-vc %s --program=legacy 2>&1 | FileCheck %s --check-prefix=LEGACY
// LEGACY: assume.memset-bound: count=1 bound=16
// LEGACY: result: unsat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @phi {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c64 = sea.const 64 : i64
  %A = sea.malloc %c64 align 8 [1] : i64 -> !sea.ptr<64>
  %c = sea.nondet "c" [2] : i1
  %a = sea.const 20 : i64
  %b = sea.const 30 : i64
  sea.cond_br %c, ^l(%a : i64), ^r(%b : i64)
^l(%x : i64):
  sea.br ^j(%x : i64)
^r(%y : i64):
  sea.br ^j(%y : i64)
^j(%s : i64):
  %v = sea.const 255 : i8
  %m1 = sea.memset %m0[%A], %v, %s inplace : !m
  %f = sea.const 0 : i1
  sea.assert %f loc("a.c":9:3)
  sea.halt
}
sea.program @three {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c64 = sea.const 64 : i64
  %A = sea.malloc %c64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.malloc %c64 align 8 [2] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [3] : i64
  %k = sea.nondet "k" [4] : i64
  %j = sea.nondet "j" [5] : i64
  %v = sea.const 255 : i8
  %m1 = sea.memset %m0[%A], %v, %n inplace : !m
  %m2 = sea.memset %m1[%B], %v, %k inplace : !m
  %m3 = sea.memmove %m2[%B], %m2[%A], %j inplace : !m
  %x = sea.load %m3[%B] : !m -> i8
  %ok = sea.icmp eq, %x, %v : i8
  sea.assert %ok loc("a.c":9:3)
  sea.halt
}
sea.program @legacy {
  %n = sea.nondet "n" [1] : i64
  %c16 = sea.const 16 : i64
  %fits = sea.icmp ule, %n, %c16 : i64
  sea.assume %fits {sea.memset_bound = 16 : i64}
  %ok = sea.icmp ule, %n, %c16 : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

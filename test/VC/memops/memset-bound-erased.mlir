// assume.memset-bound is counted where sea-expand-memset makes the bound:
// here the length (20 or 30) never fits K = 16, the lowering blocks every
// path and erases the expansion with its assume, and the unsat rests on
// the bound all the same.
// RUN: sea-vc %s --lower 2>&1 | FileCheck %s
// CHECK: assume.memset-bound: count=1 bound=16
// CHECK: result: unsat
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

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

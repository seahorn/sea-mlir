// RUN: sea-vc %s --program=cc --lower --seam --vc-fold-top-eqs --smt2 2>&1 >/dev/null | FileCheck %s --check-prefix=NUMERAL
// RUN: sea-vc %s --program=cc --lower --seam --vc-fold-top-eqs --vc-fold-concat-ite=all --smt2 2>&1 >/dev/null | FileCheck %s --check-prefix=ALL
// RUN: sea-vc %s --program=cc --lower --seam --vc-fold-top-eqs --vc-fold-concat-ite=off --smt2 2>&1 >/dev/null | FileCheck %s --check-prefix=OFF
// The concat-ite hoist of --vc-fold-top-eqs in sea-vc (docs/design/FoldTopEqs.md).
//
// @cc: two bytes stored as select(c, x, 1) and select(c, y, 2), read back as
// one i16. The fold leaves concat(ite(c, y, 2), ite(c, x, 1)), whose
// then-leaf concat(y, x) is not numerals: the default (numeral) mode refuses
// it (counted), `all` hoists it, `off` does not look.
// NUMERAL: vc.fold-top-eqs:{{.*}} concat-ite=0 concat-ite-refused=1
// ALL: vc.fold-top-eqs:{{.*}} concat-ite=1 concat-ite-refused=0
// OFF: vc.fold-top-eqs:{{.*}} concat-ite=0 concat-ite-refused=0
!pm = !sea.mem<!sea.ptr<64>, i8>
sea.program @cc {
  %m0 = sea.mem.nondet "m" [0] : !pm
  %n = sea.const 16 : i64
  %p = sea.malloc %n align 8 [1] : i64 -> !sea.ptr<64>
  %a = sea.load %m0[%p] : !pm -> i8
  %c7 = sea.const 7 : i8
  %fact = sea.icmp eq, %a, %c7 : i8
  sea.assume %fact
  %x = sea.nondet "x" [2] : i8
  %y = sea.nondet "y" [3] : i8
  %c = sea.nondet "c" [4] : i1
  %c1 = sea.const 1 : i8
  %c2 = sea.const 2 : i8
  %b0 = sea.select %c, %x, %c1 : i8
  %b1 = sea.select %c, %y, %c2 : i8
  %o1 = sea.const 1 : i64
  %p1 = sea.ptr.add %p, %o1 : !sea.ptr<64>, i64
  %m1 = sea.store %m0[%p], %b0 inplace : !pm, i8
  %m2 = sea.store %m1[%p1], %b1 inplace : !pm, i8
  %w = sea.load %m2[%p] : !pm -> i16
  %k = sea.const 513 : i16
  %ok = sea.icmp ne, %w, %k : i16
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

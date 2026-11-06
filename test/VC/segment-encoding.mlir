// RUN: sea-vc %s --smt2 | FileCheck %s --check-prefix=PLAIN
// RUN: sea-vc %s --seam --seam-simplify=0 --smt2 | FileCheck %s --check-prefix=SEAM
// RUN: sea-vc %s --seam --smt2 | FileCheck %s --check-prefix=SIMP
// RUN: sea-vc %s | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --seam | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --seam | FileCheck %s --check-prefix=REG
// RUN: not sea-vc %s --seam --vc-join=eq 2>&1 | FileCheck %s --check-prefix=JOIN

// Without --seam a segment's result is the numeral base: the query is the
// same as before segments had results. With --seam it is the constant
// obj.<name> with a side equality to the base; the verdict (and the replay
// validation) do not change.
// PLAIN-NOT: obj.
// SEAM-DAG: obj.global.g
// SEAM-DAG: obj.stack.3
// SEAM-DAG: (= obj.stack.3 #x00000000bffffff0)
// SEAM-DAG: (= obj.global.g #x0000000008048004)
// With the normalization rules (default under --seam) the query is false: the
// assertion holds, so the side equalities are not needed.
// SIMP: (assert false)
// VERDICT: result: unsat
// The registry lists the objects in allocation (segment) order.
// REG: object: name=obj.stack.3 order=0 kind=stack base=3221225456 size=16 align=8
// REG-NEXT: object: name=obj.global.g order=1 kind=global base=134512644 size=4 align=4
// JOIN: SeaM needs --vc-join=ite
!am = !sea.mem<!sea.addr<64>, i8>
sea.program @x {
  %b = sea.segment "stack.3" base 3221225456 : i64 size 16 : i64 align 8 kind "stack" : !sea.addr<64>
  %g = sea.segment "global.g" base 134512644 : i64 size 4 : i64 align 4 kind "global" : !sea.addr<64>
  %m0 = sea.mem.nondet "m" [0] : !am
  %v = sea.const 7 : i8
  %m1 = sea.store %m0[%b], %v inplace : !am, i8
  %w = sea.const 9 : i8
  %m2 = sea.store %m1[%g], %w inplace : !am, i8
  %x = sea.load %m2[%b] : !am -> i8
  %c = sea.icmp eq, %x, %v : i8
  sea.assert %c
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

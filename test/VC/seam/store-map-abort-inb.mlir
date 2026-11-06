// R-ABORT decides its entries with cmp.diff-base (A-INB): idx = ite(c, q, o+5)
// has no pointer view; every entry o+0, o+1 is decided No, one leaf by A-INB.
// RUN: sea-vc %s --lower --seam --seam-stats | FileCheck %s --check-prefix=STATS
// RUN: sea-vc %s --lower --seam --seam-inb=0 --seam-stats | FileCheck %s --check-prefix=NOINB
// RUN: sea-vc %s --lower --seam --seam-store-map=0 | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=VERDICT
// VERDICT: result: unsat
// STATS-DAG: result: unsat
// STATS-DAG: seam.rule.row.smap-abort: 1
// STATS-DAG: seam.rule.row.smap-abort-skip: 2
// STATS-DAG: seam.rule.cmp.diff-base: 2
// NOINB: result: unsat
// NOINB-NOT: seam.rule.cmp.diff-base
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @ai {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %o = sea.malloc %c16 align 16 [1] : i64 -> !sea.ptr<64>
  %q = sea.malloc %c16 align 16 [2] : i64 -> !sea.ptr<64>
  %c1 = sea.const 1 : i64
  %c5 = sea.const 5 : i64
  %p1 = sea.ptr.add %o, %c1 : !sea.ptr<64>, i64
  %p5 = sea.ptr.add %o, %c5 : !sea.ptr<64>, i64
  %c = sea.nondet "c" [7] : i1
  %idx = sea.select %c, %q, %p5 : !sea.ptr<64>
  %v1 = sea.const 1 : i8
  %v2 = sea.const 2 : i8
  %m1 = sea.store %m0[%o], %v1 inplace : !m, i8
  %m2 = sea.store %m1[%p1], %v2 inplace : !m, i8
  %x = sea.load %m2[%idx] : !m -> i8
  %y = sea.load %m0[%idx] : !m -> i8
  %ok = sea.icmp eq, %x, %y : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

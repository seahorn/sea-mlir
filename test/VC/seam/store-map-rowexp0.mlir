// --seam-row-expand=0: a symbolic write on top of a store-map is a barrier,
// the read below it stays a select over store(store-map ...): the store-map
// reaches the solver boundary (smap.expanded > 0).
// RUN: sea-vc %s --lower --seam --seam-row-expand=0 --seam-stats | FileCheck %s --check-prefix=STATS
// RUN: sea-vc %s --lower --seam --seam-stats | FileCheck %s --check-prefix=DEFAULT
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=VERDICT
// VERDICT: result: unsat
// STATS-DAG: result: unsat
// STATS-DAG: seam.rule.smap.expanded: 1
// DEFAULT: result: unsat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @r0 {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %o = sea.malloc %c16 align 16 [1] : i64 -> !sea.ptr<64>
  %c1 = sea.const 1 : i64
  %c4 = sea.const 4 : i64
  %p1 = sea.ptr.add %o, %c1 : !sea.ptr<64>, i64
  %k = sea.nondet "k" [2] : i64
  %le = sea.icmp ule, %k, %c4 : i64
  sea.assume %le
  %pk = sea.ptr.add %o, %k : !sea.ptr<64>, i64
  %v1 = sea.const 1 : i8
  %v2 = sea.const 2 : i8
  %v9 = sea.const 9 : i8
  %m1 = sea.store %m0[%o], %v1 inplace : !m, i8
  %m2 = sea.store %m1[%p1], %v2 inplace : !m, i8
  %m3 = sea.store %m2[%pk], %v9 inplace : !m, i8
  %x = sea.load %m3[%p1] : !m -> i8
  %a = sea.icmp eq, %x, %v2 : i8
  %b = sea.icmp eq, %x, %v9 : i8
  %ok = sea.or %a, %b : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

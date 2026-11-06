// Old memory versions read and extended after the map moved (side table),
// a no-op store, R-MISS over a barrier write to the same base with a
// symbolic offset, and a symbolic-index read.
// RUN: sea-vc %s --lower --lower-opt=false --seam --seam-stats | FileCheck %s --check-prefix=STATS
// RUN: sea-vc %s --lower --seam | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --lower --seam --seam-store-map=0 | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --lower --seam --seam-inb=0 | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --lower --seam --seam-row-expand=0 | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=VERDICT
// VERDICT: result: unsat
// STATS-DAG: seam.rule.smap.rebuild: 1
// STATS-DAG: seam.rule.smap.scan: 3
// STATS-DAG: seam.rule.row.smap-miss: 2
// STATS-DAG: seam.rule.row.smap-abort: 1
// STATS-DAG: seam.rule.smap.expanded: 0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @fork {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c16 = sea.const 16 : i64
  %o = sea.malloc %c16 align 16 [1] : i64 -> !sea.ptr<64>
  %q = sea.malloc %c16 align 16 [2] : i64 -> !sea.ptr<64>
  %c1 = sea.const 1 : i64
  %c2 = sea.const 2 : i64
  %c3 = sea.const 3 : i64
  %c4 = sea.const 4 : i64
  %k = sea.nondet "k" [3] : i64
  %le = sea.icmp ule, %k, %c4 : i64
  sea.assume %le
  %pk = sea.ptr.add %o, %k : !sea.ptr<64>, i64
  %p1 = sea.ptr.add %o, %c1 : !sea.ptr<64>, i64
  %p2 = sea.ptr.add %o, %c2 : !sea.ptr<64>, i64
  %p3 = sea.ptr.add %o, %c3 : !sea.ptr<64>, i64
  %p4 = sea.ptr.add %o, %c4 : !sea.ptr<64>, i64
  %v1 = sea.const 11 : i8
  %v2 = sea.const 22 : i8
  %v3 = sea.const 33 : i8
  %v4 = sea.const 44 : i8
  %v9 = sea.const 99 : i8
  %ma = sea.store %m0[%pk], %v9 inplace : !m, i8
  %m1 = sea.store %ma[%p1], %v1 inplace : !m, i8
  %m2 = sea.store %m1[%p2], %v2 inplace : !m, i8
  %mq = sea.store %m2[%q], %v9 inplace : !m, i8
  %m3 = sea.store %mq[%p3], %v3 inplace : !m, i8
  %m4 = sea.store %m2[%p4], %v4 inplace : !m, i8
  %m5 = sea.store %m4[%p2], %v2 inplace : !m, i8
  %m6 = sea.store %m4[%p3], %v1 inplace : !m, i8
  %x3 = sea.load %m3[%p4] : !m -> i8
  %x4 = sea.load %m4[%p3] : !m -> i8
  %x5 = sea.load %m5[%p2] : !m -> i8
  %x6 = sea.load %m6[%p3] : !m -> i8
  %x7 = sea.load %m6[%pk] : !m -> i8
  %x8 = sea.load %m3[%q] : !m -> i8
  %e3 = sea.icmp eq, %x3, %x4 : i8
  %e5 = sea.icmp eq, %x5, %v2 : i8
  %e6 = sea.icmp eq, %x6, %v1 : i8
  %e8 = sea.icmp eq, %x8, %v9 : i8
  %n7 = sea.icmp ne, %x7, %z : i8
  %a1 = sea.and %e5, %e5 : i1
  %a2 = sea.and %a1, %e6 : i1
  %a3 = sea.and %a2, %n7 : i1
  %a4 = sea.and %a3, %e8 : i1
  sea.assert %a4
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

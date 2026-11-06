// Two branches extend one store-map (the second insert rebuilds the map of
// the old list), a join (ite of store-maps), writes on top of the join, and
// reads at numeral and symbolic offsets. Holds: unsat.
// RUN: sea-vc %s --lower --lower-opt=false --seam --seam-stats | FileCheck %s --check-prefix=STATS
// RUN: sea-vc %s --lower --seam | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --lower --seam --seam-store-map=0 | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --lower --seam --seam-inb=0 | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --lower --seam --seam-row-expand=0 | FileCheck %s --check-prefix=VERDICT
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=VERDICT
// The side table: the second branch inserts into a list whose map moved to the first
// (smap.rebuild), a read of the old list scans it (smap.scan), and R-MISS / R-ABORT
// run over the ite of store-maps of the join.
// VERDICT: result: unsat
// STATS-DAG: seam.rule.smap.rebuild: 1
// STATS-DAG: seam.rule.smap.scan: 1
// STATS-DAG: seam.rule.row.smap-miss: 3
// STATS-DAG: seam.rule.row.smap-abort: 4
// STATS-DAG: seam.rule.smap.expanded: 0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @jf {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c16 = sea.const 16 : i64
  %o1 = sea.alloca %c16 align 16 [1] : i64 -> !sea.ptr<64>
  %o2 = sea.alloca %c16 align 16 [2] : i64 -> !sea.ptr<64>
  %c1 = sea.const 1 : i64
  %c2 = sea.const 2 : i64
  %c3 = sea.const 3 : i64
  %c5 = sea.const 5 : i64
  %c6 = sea.const 6 : i64
  %p1 = sea.ptr.add %o1, %c1 : !sea.ptr<64>, i64
  %p2 = sea.ptr.add %o1, %c2 : !sea.ptr<64>, i64
  %p3 = sea.ptr.add %o1, %c3 : !sea.ptr<64>, i64
  %p5 = sea.ptr.add %o1, %c5 : !sea.ptr<64>, i64
  %p6 = sea.ptr.add %o1, %c6 : !sea.ptr<64>, i64
  %v1 = sea.const 1 : i8
  %v2 = sea.const 2 : i8
  %v3 = sea.const 3 : i8
  %v4 = sea.const 4 : i8
  %v5 = sea.const 5 : i8
  %v6 = sea.const 6 : i8
  %c = sea.nondet "nd" [3] : i1
  %k = sea.nondet "k" [4] : i64
  %le = sea.icmp ule, %k, %c3 : i64
  sea.assume %le
  %pk = sea.ptr.add %o1, %k : !sea.ptr<64>, i64
  %ma = sea.store %m0[%o1], %v1 inplace : !m, i8
  %mb = sea.store %ma[%p1], %v2 inplace : !m, i8
  %mq = sea.store %mb[%o2], %v6 inplace : !m, i8
  sea.cond_br %c, ^t, ^e
^t:
  %mt = sea.store %mq[%p2], %v3 inplace : !m, i8
  %mt2 = sea.store %mt[%o1], %v4 inplace : !m, i8
  sea.br ^j(%mt2 : !m)
^e:
  %me = sea.store %mq[%p3], %v4 inplace : !m, i8
  %me2 = sea.store %me[%p2], %v5 inplace : !m, i8
  sea.br ^j(%me2 : !m)
^j(%mj : !m):
  %mk = sea.store %mj[%p5], %v5 inplace : !m, i8
  %ml = sea.store %mk[%p6], %v6 inplace : !m, i8
  %x0 = sea.load %ml[%o1] : !m -> i8
  %x1 = sea.load %ml[%p1] : !m -> i8
  %x2 = sea.load %ml[%p2] : !m -> i8
  %xq = sea.load %ml[%o2] : !m -> i8
  %xk = sea.load %ml[%pk] : !m -> i8
  %xk0 = sea.load %mb[%pk] : !m -> i8
  // x0 = c ? 4 : 1; x1 = 2; x2 = c ? 3 : 5; xq = 6; xk in {x0,2,x2,x3}
  %s0 = sea.select %c, %v4, %v1 : i8
  %s2 = sea.select %c, %v3, %v5 : i8
  %e0 = sea.icmp eq, %x0, %s0 : i8
  %e1 = sea.icmp eq, %x1, %v2 : i8
  %e2 = sea.icmp eq, %x2, %s2 : i8
  %eq = sea.icmp eq, %xq, %v6 : i8
  %ek = sea.icmp ne, %xk, %v6 : i8
  %a0 = sea.and %e0, %e1 : i1
  %a1 = sea.and %a0, %e2 : i1
  %a2 = sea.and %a1, %eq : i1
  sea.assert %a2
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

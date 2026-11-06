// An expanded read over consecutive undecided writes of one value is one
// ite over the disjunction of their compares (row.store-group). Merging one
// ite per write afterwards (bv.ite-merge) rebuilt
// a growing disjunction at every level: quadratic in the chain. Four zero
// bytes are written into a buffer at symbolic offsets i1..i4 (the writes
// stay plain stores: no numeral offset), then read at j. The read is 0 if
// j is one of them, else the initial cell; the property x = 0 or x = old
// holds (unsat), with and without grouping.
// RUN: sea-vc %s --lower --seam --seam-stats 2>&1 | FileCheck %s --check-prefixes=CHECK,ON
// RUN: sea-vc %s --lower --seam --seam-stats --seam-row-group=0 2>&1 | FileCheck %s --check-prefixes=CHECK,OFF
// RUN: sea-vc %s --lower --seam --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// ON: seam.rule.row.store-group: {{[1-9][0-9]*}}
// OFF-NOT: row.store-group
// OFF: seam.rule.bv.ite-merge: {{[1-9][0-9]*}}
// CHECK: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @group {
  %c64 = sea.const 64 : i64
  %B = sea.malloc %c64 align 8 [1] : i64 -> !sea.ptr<64>
  %m0 = sea.mem.nondet "m" [2] : !m
  %i1 = sea.nondet "i1" [3] : i64
  %i2 = sea.nondet "i2" [4] : i64
  %i3 = sea.nondet "i3" [5] : i64
  %i4 = sea.nondet "i4" [6] : i64
  %j = sea.nondet "j" [7] : i64
  %z = sea.const 0 : i8
  %p1 = sea.ptr.add %B, %i1 : !sea.ptr<64>, i64
  %p2 = sea.ptr.add %B, %i2 : !sea.ptr<64>, i64
  %p3 = sea.ptr.add %B, %i3 : !sea.ptr<64>, i64
  %p4 = sea.ptr.add %B, %i4 : !sea.ptr<64>, i64
  %pj = sea.ptr.add %B, %j : !sea.ptr<64>, i64
  %old = sea.load %m0[%pj] : !m -> i8
  %m1 = sea.store %m0[%p1], %z inplace : !m, i8
  %m2 = sea.store %m1[%p2], %z inplace : !m, i8
  %m3 = sea.store %m2[%p3], %z inplace : !m, i8
  %m4 = sea.store %m3[%p4], %z inplace : !m, i8
  %x = sea.load %m4[%pj] : !m -> i8
  %e0 = sea.icmp eq, %x, %z : i8
  %eo = sea.icmp eq, %x, %old : i8
  %ok = sea.or %e0, %eo : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

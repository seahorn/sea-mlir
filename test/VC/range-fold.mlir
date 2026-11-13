// RUN: sea-vc %s --program=rows --lower --seam | FileCheck %s --check-prefix=OFF
// RUN: sea-vc %s --program=rows --lower --seam --range-fold=on | FileCheck %s --check-prefix=ON
// RUN: sea-vc %s --program=rows --lower --seam --range-fold=check | FileCheck %s --check-prefix=CHECK-JOB
// RUN: sea-vc %s --program=sibling --lower --range-fold=on | FileCheck %s --check-prefix=SIBLING
// RUN: sea-vc %s --program=wrap --lower | FileCheck %s --check-prefix=WRAP-OFF
// RUN: sea-vc %s --program=wrap --lower --range-fold=on | FileCheck %s --check-prefix=WRAP-ON
// RUN: sea-vc %s --program=wrap --lower --range-fold=check | FileCheck %s --check-prefix=WRAP-CHECK
// RUN: not sea-vc %s --program=rows --range-fold=on 2>&1 | FileCheck %s --check-prefix=NOLOWER
// RUN: not sea-vc %s --program=rows --lower --range-fold=check --check-align 2>&1 | FileCheck %s --check-prefix=JOBS
// sea-vc --range-fold (docs/design/RangeFold.md). @rows: a guarded store
// destination under a bound assumption: each of two distinct slots in
// [0, 6) writes its row at select(in_range, 2 * slot, sink), and in_range
// follows from the slot's assumed bounds. Off, the store addresses are selects and the reads get an
// Ackermann constraint; on, both selects fold to the row and SeaM decides
// every read. The check job proves the folds (unsat).
// OFF-NOT: vc.range-fold
// OFF: vc.ackermann-constraints: {{[1-9]}}
// OFF: result: unsat
// ON: vc.range-fold: mode=on
// ON-SAME: select=2
// ON: vc.ackermann-constraints: 0
// ON: result: unsat
// CHECK-JOB: vc.range-fold: mode=check
// CHECK-JOB-SAME: checks={{[1-9]}}
// CHECK-JOB: result: unsat
// @sibling: the bound is assumed on one branch only; the assert after the
// join can fail on the other (sat, replayed). A fold by the sibling's fact
// would make it unsat.
// SIBLING: icmp=0
// SIBLING: result: sat
// SIBLING: replay: assert-failed
// @wrap: nsw / nuw are binding semantics. x + 250 nuw with x in [0, 10]
// cannot wrap by the flag, so y >= 250 and the fold makes it unsat: the
// intended answer. The VC encoding ignores the flag (wraps for x >= 6, an
// over-approximation), so without the fold it is sat; the check job asserts
// the fold against that encoding and is sat too. The fold is counted
// (wrap-flags=1), not reported as an assumption.
// WRAP-OFF: result: sat
// WRAP-ON: vc.range-fold: mode=on
// WRAP-ON-SAME: wrap-flags=1
// WRAP-ON-NOT: assume.wrap-flags
// WRAP-ON: result: unsat
// WRAP-CHECK: result: sat
// NOLOWER: --range-fold needs --lower
// JOBS: [vc.check-jobs]
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @rows {
  %c0 = sea.const 0 : i64
  %c1 = sea.const 1 : i64
  %c2 = sea.const 2 : i64
  %c6 = sea.const 6 : i64
  %c12 = sea.const 12 : i64
  %c14 = sea.const 14 : i64
  %m0 = sea.mem.nondet "m" [0] : !m
  %cache = sea.alloca %c14 align 8 [1] : i64 -> !sea.ptr<64>
  %s1 = sea.nondet "slot" [2] : i32
  %s2 = sea.nondet "slot" [3] : i32
  %v1 = sea.nondet "v" [4] : i8
  %v2 = sea.nondet "v" [5] : i8
  %x1 = sea.sext %s1 : i32 to i64
  %x2 = sea.sext %s2 : i32 to i64
  %a1 = sea.icmp sge, %x1, %c0 : i64
  %b1 = sea.icmp slt, %x1, %c6 : i64
  %ab1 = sea.and %a1, %b1 : i1
  sea.assume %ab1
  %a2 = sea.icmp sge, %x2, %c0 : i64
  %b2 = sea.icmp slt, %x2, %c6 : i64
  %ab2 = sea.and %a2, %b2 : i1
  sea.assume %ab2
  %d = sea.icmp ne, %x1, %x2 : i64
  sea.assume %d
  %lo1 = sea.mul %x1, %c2 : i64
  %g11 = sea.icmp sge, %lo1, %c0 : i64
  %hi1 = sea.add %lo1, %c1 : i64
  %g12 = sea.icmp slt, %hi1, %c12 : i64
  %in1 = sea.and %g11, %g12 : i1
  %sel1 = sea.select %in1, %lo1, %c12 : i64
  %p1 = sea.ptr.add %cache, %sel1 : !sea.ptr<64>, i64
  %m1 = sea.store %m0[%p1], %v1 inplace : !m, i8
  %lo2 = sea.mul %x2, %c2 : i64
  %g21 = sea.icmp sge, %lo2, %c0 : i64
  %hi2 = sea.add %lo2, %c1 : i64
  %g22 = sea.icmp slt, %hi2, %c12 : i64
  %in2 = sea.and %g21, %g22 : i1
  %sel2 = sea.select %in2, %lo2, %c12 : i64
  %p2 = sea.ptr.add %cache, %sel2 : !sea.ptr<64>, i64
  %m2 = sea.store %m1[%p2], %v2 inplace : !m, i8
  %q1 = sea.ptr.add %cache, %lo1 : !sea.ptr<64>, i64
  %r1 = sea.load %m2[%q1] : !m -> i8
  %q2 = sea.ptr.add %cache, %lo2 : !sea.ptr<64>, i64
  %r2 = sea.load %m2[%q2] : !m -> i8
  %ok1 = sea.icmp eq, %r1, %v1 : i8
  %ok2 = sea.icmp eq, %r2, %v2 : i8
  %ok = sea.and %ok1, %ok2 : i1
  sea.assert %ok
  sea.halt
}

sea.program @wrap {
  %c11 = sea.const 11 : i8
  %c250 = sea.const 250 : i8
  %x = sea.nondet "x" [0] : i8
  %a = sea.icmp ult, %x, %c11 : i8
  sea.assume %a
  %y = sea.add %x, %c250 nuw : i8
  %ok = sea.icmp uge, %y, %c250 : i8
  sea.assert %ok
  sea.halt
}

sea.program @sibling {
  %c6 = sea.const 6 : i8
  %x = sea.nondet "x" [0] : i8
  %c = sea.nondet "c" [1] : i1
  sea.cond_br %c, ^a, ^b
^a:
  %a = sea.icmp ult, %x, %c6 : i8
  sea.assume %a
  sea.br ^j
^b:
  sea.br ^j
^j:
  %ok = sea.icmp ult, %x, %c6 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

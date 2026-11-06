// ARM (docs/design/SeaM.md "ARM", thesis Ch. 5): the store address q = p + 4
// is a sum over an ite with five leaves (objects o1..o5), which the syntactic rules can
// neither pull through the ite (more than four leaves) nor decide against e
// leaf by leaf. ARM reads q as a pointer into one of o1..o5 and e as a
// pointer into e: different objects (A-INB), so the read of e skips the store
// to q and sees the earlier store of 1: the query has no address compare. With
// --seam-arm=0 the compare stays unknown: ROW expands it into ite(q = e, 9, 1). The verdict is the
// same on every setting: the assertion holds.
// The offset 4 is inside every object: no A-INB condition is needed (conditions=0).
// RUN: sea-vc %s --lower --seam --seam-stats | FileCheck %s --check-prefix=ARM
// RUN: sea-vc %s --lower --seam --smt2 | FileCheck %s --check-prefix=SMT-ARM
// RUN: sea-vc %s --lower --seam --seam-arm=0 --seam-stats | FileCheck %s --check-prefix=NOARM
// RUN: sea-vc %s --lower --seam --seam-arm=0 --smt2 | FileCheck %s --check-prefix=SMT-NOARM
// RUN: sea-vc %s --lower --seam | FileCheck %s --check-prefix=CHECKED
// RUN: sea-vc %s --lower --seam --seam-inb=0 --seam-stats | FileCheck %s --check-prefix=NOINB
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=VERDICT
// RUN: not sea-vc %s --lower --seam-arm=0 2>&1 | FileCheck %s --check-prefix=NOSEAM
// ARM: seam.rule.arm.skip: 1
// ARM: seam.rule.row.store-skip: 1
// ARM-NOT: seam.rule.row.store:
// ARM: result: unsat
// SMT-ARM-NOT: bvadd
// NOARM-NOT: seam.rule.arm.skip
// NOARM: seam.rule.row.store: 1
// NOARM: result: unsat
// SMT-NOARM: bvadd
// CHECKED: seam.inb: assumed accesses=3 conditions=0
// CHECKED: result: unsat
// NOINB-NOT: seam.rule.arm.skip
// NOINB: result: unsat
// VERDICT: result: unsat
// NOSEAM: need --seam
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @arm {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c16 = sea.const 16 : i64
  %o1 = sea.alloca %c16 align 8 [1] : i64 -> !sea.ptr<64>
  %o2 = sea.alloca %c16 align 8 [2] : i64 -> !sea.ptr<64>
  %o3 = sea.alloca %c16 align 8 [3] : i64 -> !sea.ptr<64>
  %o4 = sea.alloca %c16 align 8 [4] : i64 -> !sea.ptr<64>
  %o5 = sea.alloca %c16 align 8 [5] : i64 -> !sea.ptr<64>
  %e = sea.alloca %c16 align 8 [6] : i64 -> !sea.ptr<64>
  %b1 = sea.nondet "b1" [11] : i1
  %b2 = sea.nondet "b2" [12] : i1
  %b3 = sea.nondet "b3" [13] : i1
  %b4 = sea.nondet "b4" [14] : i1
  %p4 = sea.select %b4, %o4, %o5 : !sea.ptr<64>
  %p3 = sea.select %b3, %o3, %p4 : !sea.ptr<64>
  %p2 = sea.select %b2, %o2, %p3 : !sea.ptr<64>
  %p = sea.select %b1, %o1, %p2 : !sea.ptr<64>
  %c4 = sea.const 4 : i64
  %q = sea.ptr.add %p, %c4 : !sea.ptr<64>, i64
  %v1 = sea.const 1 : i8
  %v9 = sea.const 9 : i8
  %m1 = sea.store %m0[%e], %v1 inplace : !m, i8
  %m2 = sea.store %m1[%q], %v9 inplace : !m, i8
  %x = sea.load %m2[%e] : !m -> i8
  %ok = sea.icmp eq, %x, %v1 : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

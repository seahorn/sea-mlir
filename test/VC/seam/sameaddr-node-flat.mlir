// Address equivalence in an NKI gather: the byte of a
// memmove's source read back through the copy, d + (3i + 1) as the copy's
// read equation builds it (a node over the source node d + 3i), and the same
// byte read directly at d + (3i + 1) are one address (sameAddr's erased
// fallback, cmp.refl-erase): no Ackermann read is left.
// RUN: sea-vc %s --lower --seam --seam-stats | FileCheck %s
// CHECK: vc.ackermann-reads: 0
// CHECK: vc.ackermann-constraints: 0
// CHECK: seam.rule.cmp.refl-erase: {{[1-9]}}
// CHECK: result: unsat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @g {
^bb0:
  %m0 = sea.mem.nondet "r" [0] : !m
  %c0 = sea.const 0 : i64
  %c1 = sea.const 1 : i64
  %c2 = sea.const 2 : i64
  %c3 = sea.const 3 : i64
  %c4 = sea.const 4 : i64
  %c6 = sea.const 6 : i64
  %d = sea.alloca %c6 align 8 [1] : i64 -> !sea.ptr<64>
  %ix = sea.alloca %c4 align 8 [2] : i64 -> !sea.ptr<64>
  %s = sea.alloca %c3 align 8 [3] : i64 -> !sea.ptr<64>
  %i32 = sea.load %m0[%ix] align 4 : !m -> i32
  %i = sea.sext %i32 : i32 to i64
  %a = sea.icmp sge, %i, %c0 : i64
  %b = sea.icmp slt, %i, %c2 : i64
  %ab = sea.and %a, %b : i1
  sea.assume %ab
  %t = sea.mul %i, %c3 : i64
  %t1 = sea.add %t, %c1 : i64
  %pe = sea.ptr.add %d, %t1 : !sea.ptr<64>, i64
  %e = sea.load %m0[%pe] align 1 : !m -> i8
  %ps = sea.ptr.add %d, %t : !sea.ptr<64>, i64
  %m1 = sea.memmove %m0[%s], %m0[%ps], %c3 inplace : !m
  %pa = sea.ptr.add %s, %c1 : !sea.ptr<64>, i64
  %x = sea.load %m1[%pa] align 1 : !m -> i8
  %eq = sea.icmp eq, %x, %e : i8
  sea.assert %eq
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

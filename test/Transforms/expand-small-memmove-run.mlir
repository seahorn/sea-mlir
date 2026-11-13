// RUN: sea-run %s | FileCheck %s
// RUN: sea-opt %s --sea-place-static | sea-run - | FileCheck %s
// RUN: sea-opt %s --sea-place-static --sea-expand-small-memmove=max-bytes=4 | FileCheck %s --check-prefix=SHAPE
// RUN: sea-opt %s --sea-place-static --sea-expand-small-memmove=max-bytes=4 | sea-run - | FileCheck %s

// Overlapping memmoves, forward (dst = src + 1) and backward (dst = src - 1),
// in the interpreter (the golden semantics) before and after
// sea-expand-small-memmove: the asserts state memmove's result.
// SHAPE-NOT: sea.memmove
// CHECK: status: halted
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @overlap {
  %m0 = sea.mem.nondet "m" [1] : !m
  %eight = sea.const 8 : i64
  %p = sea.alloca %eight align 8 [2] : i64 -> !sea.ptr<64>
  %q = sea.alloca %eight align 8 [3] : i64 -> !sea.ptr<64>
  %o0 = sea.const 0 : i64
  %c0 = sea.const 10 : i8
  %o1 = sea.const 1 : i64
  %c1 = sea.const 11 : i8
  %o2 = sea.const 2 : i64
  %c2 = sea.const 12 : i8
  %o3 = sea.const 3 : i64
  %c3 = sea.const 13 : i8
  %o4 = sea.const 4 : i64
  %c4 = sea.const 14 : i8
  %o5 = sea.const 5 : i64
  %c5 = sea.const 15 : i8
  %p0 = sea.ptr.add %p, %o0 : !sea.ptr<64>, i64
  %p1 = sea.ptr.add %p, %o1 : !sea.ptr<64>, i64
  %p2 = sea.ptr.add %p, %o2 : !sea.ptr<64>, i64
  %p3 = sea.ptr.add %p, %o3 : !sea.ptr<64>, i64
  %p4 = sea.ptr.add %p, %o4 : !sea.ptr<64>, i64
  %p5 = sea.ptr.add %p, %o5 : !sea.ptr<64>, i64
  %q0 = sea.ptr.add %q, %o0 : !sea.ptr<64>, i64
  %q1 = sea.ptr.add %q, %o1 : !sea.ptr<64>, i64
  %q2 = sea.ptr.add %q, %o2 : !sea.ptr<64>, i64
  %q3 = sea.ptr.add %q, %o3 : !sea.ptr<64>, i64
  %q4 = sea.ptr.add %q, %o4 : !sea.ptr<64>, i64
  %q5 = sea.ptr.add %q, %o5 : !sea.ptr<64>, i64
  %m1 = sea.store %m0[%p0], %c0 inplace : !m, i8
  %m2 = sea.store %m1[%p1], %c1 inplace : !m, i8
  %m3 = sea.store %m2[%p2], %c2 inplace : !m, i8
  %m4 = sea.store %m3[%p3], %c3 inplace : !m, i8
  %m5 = sea.store %m4[%p4], %c4 inplace : !m, i8
  %m6 = sea.store %m5[%p5], %c5 inplace : !m, i8
  %m7 = sea.store %m6[%q0], %c0 inplace : !m, i8
  %m8 = sea.store %m7[%q1], %c1 inplace : !m, i8
  %m9 = sea.store %m8[%q2], %c2 inplace : !m, i8
  %m10 = sea.store %m9[%q3], %c3 inplace : !m, i8
  %m11 = sea.store %m10[%q4], %c4 inplace : !m, i8
  %m12 = sea.store %m11[%q5], %c5 inplace : !m, i8
  %four = sea.const 4 : i64
  %m13 = sea.memmove %m12[%p1], %m12[%p0], %four inplace : !m
  %m14 = sea.memmove %m13[%q0], %m13[%q1], %four inplace : !m
  %xp0 = sea.load %m14[%p0] : !m -> i8
  %ep0 = sea.const 10 : i8
  %kp0 = sea.icmp eq, %xp0, %ep0 : i8
  %xp1 = sea.load %m14[%p1] : !m -> i8
  %ep1 = sea.const 10 : i8
  %kp1 = sea.icmp eq, %xp1, %ep1 : i8
  %xp2 = sea.load %m14[%p2] : !m -> i8
  %ep2 = sea.const 11 : i8
  %kp2 = sea.icmp eq, %xp2, %ep2 : i8
  %xp3 = sea.load %m14[%p3] : !m -> i8
  %ep3 = sea.const 12 : i8
  %kp3 = sea.icmp eq, %xp3, %ep3 : i8
  %xp4 = sea.load %m14[%p4] : !m -> i8
  %ep4 = sea.const 13 : i8
  %kp4 = sea.icmp eq, %xp4, %ep4 : i8
  %xp5 = sea.load %m14[%p5] : !m -> i8
  %ep5 = sea.const 15 : i8
  %kp5 = sea.icmp eq, %xp5, %ep5 : i8
  %xq0 = sea.load %m14[%q0] : !m -> i8
  %eq0 = sea.const 11 : i8
  %kq0 = sea.icmp eq, %xq0, %eq0 : i8
  %xq1 = sea.load %m14[%q1] : !m -> i8
  %eq1 = sea.const 12 : i8
  %kq1 = sea.icmp eq, %xq1, %eq1 : i8
  %xq2 = sea.load %m14[%q2] : !m -> i8
  %eq2 = sea.const 13 : i8
  %kq2 = sea.icmp eq, %xq2, %eq2 : i8
  %xq3 = sea.load %m14[%q3] : !m -> i8
  %eq3 = sea.const 14 : i8
  %kq3 = sea.icmp eq, %xq3, %eq3 : i8
  %xq4 = sea.load %m14[%q4] : !m -> i8
  %eq4 = sea.const 14 : i8
  %kq4 = sea.icmp eq, %xq4, %eq4 : i8
  %xq5 = sea.load %m14[%q5] : !m -> i8
  %eq5 = sea.const 15 : i8
  %kq5 = sea.icmp eq, %xq5, %eq5 : i8
  %a0 = sea.and %kp0, %kp1 : i1
  %a1 = sea.and %a0, %kp2 : i1
  %a2 = sea.and %a1, %kp3 : i1
  %a3 = sea.and %a2, %kp4 : i1
  %a4 = sea.and %a3, %kp5 : i1
  %a5 = sea.and %a4, %kq0 : i1
  %a6 = sea.and %a5, %kq1 : i1
  %a7 = sea.and %a6, %kq2 : i1
  %a8 = sea.and %a7, %kq3 : i1
  %a9 = sea.and %a8, %kq4 : i1
  %a10 = sea.and %a9, %kq5 : i1
  sea.assert %a10
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

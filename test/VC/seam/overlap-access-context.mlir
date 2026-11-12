// IS_OVERLAP is an access question, decided in the access context (D13):
// W-ROW-ALIGNED (--seam-wide, both accesses align 8) builds
// ite(overlap(i, 1, j, 1), v, below) for a store at i = ite(c, A + 8x,
// B + 8y) and a load at j = C + 8z. The leaves are in other objects than C,
// so under A-INB neither overlaps j: the question is false (premise.a-inb).
// Decided as the program compare PTR_EQ(i, j) it stayed open (a one-past-
// the-end pointer may equal the next object's base).
// RUN: sea-vc %s --lower --seam --seam-wide --seam-stats 2>&1 | FileCheck %s
// RUN: sea-vc %s --lower --seam --seam-wide --seam-check 2>&1 | FileCheck %s --check-prefix=CHK
// CHECK-NOT: seam.alias.open
// CHECK: seam.rule.premise.a-inb: {{[1-9]}}
// CHECK: seam.rule.ptr.overlap.decided: 1
// CHECK: seam.rule.wide.row-aligned: 1
// CHECK: result: unsat
// CHK: result: unsat
// CHK: seam-check: {{.*}} invalid=0 unknown=0 unexpressible=0
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @overlap_access {
  %m0 = sea.mem.nondet "m" [0] : !m
  %c64 = sea.const 64 : i64
  %c7 = sea.const 7 : i64
  %A = sea.alloca %c64 align 8 [1] : i64 -> !sea.ptr<64>
  %B = sea.alloca %c64 align 8 [2] : i64 -> !sea.ptr<64>
  %C = sea.alloca %c64 align 8 [3] : i64 -> !sea.ptr<64>
  %x0 = sea.nondet "x" [4] : i64
  %y0 = sea.nondet "y" [5] : i64
  %z0 = sea.nondet "z" [6] : i64
  %x = sea.and %x0, %c7 : i64
  %y = sea.and %y0, %c7 : i64
  %z = sea.and %z0, %c7 : i64
  %c = sea.nondet "c" [7] : i1
  %v = sea.nondet "v" [8] : i64
  %ax = sea.ptr.index %A [%x] strides [8] offset 0 inbounds : !sea.ptr<64>, i64
  %by = sea.ptr.index %B [%y] strides [8] offset 0 inbounds : !sea.ptr<64>, i64
  %cz = sea.ptr.index %C [%z] strides [8] offset 0 inbounds : !sea.ptr<64>, i64
  %w0 = sea.load %m0[%cz] align 8 : !m -> i64
  %a = sea.select %c, %ax, %by : !sea.ptr<64>
  %m1 = sea.store %m0[%a], %v inplace align 8 : !m, i64
  %w1 = sea.load %m1[%cz] align 8 : !m -> i64
  %ok = sea.icmp eq, %w0, %w1 : i64
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

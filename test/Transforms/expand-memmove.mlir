// RUN: sea-opt %s --sea-expand-memset 2>/dev/null | FileCheck %s
// RUN: sea-opt %s --sea-expand-memset 2>/dev/null | sea-run - --input=0=0 --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-opt %s --sea-expand-memset 2>/dev/null | sea-run - --input=0=1 --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-opt %s --sea-expand-memset 2>/dev/null | sea-run - --input=0=3 --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-opt %s --sea-expand-memset 2>/dev/null | sea-run - --input=0=5 --missing=zero | FileCheck %s --check-prefix=OK
// RUN: sea-vc %s --lower | FileCheck %s --check-prefix=VC

// A memmove with a symbolic length, overlapping forward (dst = src + 1):
// every source cell is read before any write.
// CHECK-NOT: sea.memmove
// OK: status: halted
// VC: result: unsat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @overlap {
  %m0 = sea.mem.nondet "m" [1] : !m
  %eight = sea.const 8 : i64
  %p = sea.alloca %eight align 8 [2] : i64 -> !sea.ptr<64>
  // cells 0..5 := 10, 11, 12, 13, 14, 15
  %c0 = sea.const 10 : i8
  %c1 = sea.const 11 : i8
  %c2 = sea.const 12 : i8
  %c3 = sea.const 13 : i8
  %c4 = sea.const 14 : i8
  %c5 = sea.const 15 : i8
  %one = sea.const 1 : i64
  %two = sea.const 2 : i64
  %three = sea.const 3 : i64
  %four = sea.const 4 : i64
  %five = sea.const 5 : i64
  %p1 = sea.ptr.add %p, %one : !sea.ptr<64>, i64
  %p2 = sea.ptr.add %p, %two : !sea.ptr<64>, i64
  %p3 = sea.ptr.add %p, %three : !sea.ptr<64>, i64
  %p4 = sea.ptr.add %p, %four : !sea.ptr<64>, i64
  %p5 = sea.ptr.add %p, %five : !sea.ptr<64>, i64
  %m1 = sea.store %m0[%p], %c0 inplace : !m, i8
  %m2 = sea.store %m1[%p1], %c1 inplace : !m, i8
  %m3 = sea.store %m2[%p2], %c2 inplace : !m, i8
  %m4 = sea.store %m3[%p3], %c3 inplace : !m, i8
  %m5 = sea.store %m4[%p4], %c4 inplace : !m, i8
  %m6 = sea.store %m5[%p5], %c5 inplace : !m, i8
  %n = sea.nondet "n" [0] : i64
  %ok_n = sea.icmp ule, %n, %five : i64
  sea.assume %ok_n
  %m7 = sea.memmove %m6[%p1], %m6[%p], %n inplace : !m
  // cell 1 is 10 if n >= 1 else 11; cell 3 is 12 if n >= 3 else 13
  %x1 = sea.load %m7[%p1] : !m -> i8
  %x3 = sea.load %m7[%p3] : !m -> i8
  %ge1 = sea.icmp uge, %n, %one : i64
  %ge3 = sea.icmp uge, %n, %three : i64
  %e1 = sea.select %ge1, %c0, %c1 : i8
  %e3 = sea.select %ge3, %c2, %c3 : i8
  %k1 = sea.icmp eq, %x1, %e1 : i8
  %k3 = sea.icmp eq, %x3, %e3 : i8
  %ok = sea.and %k1, %k3 : i1
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

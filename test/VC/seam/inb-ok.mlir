// An access that stays in its object: --seam assumes A-INB, says so, and
// the assumption holds (not checked by sea-vc).
// RUN: sea-vc %s --lower --seam 2>&1 | FileCheck %s
// CHECK-NOT: Warning
// CHECK: seam.inb: assumed accesses=3 conditions={{[1-9][0-9]*}}
// CHECK: result: unsat
!m = !sea.mem<!sea.ptr<64>, i8>
sea.program @ok {
  %z = sea.const 0 : i8
  %m0 = sea.mem.const %z : !m
  %c16 = sea.const 16 : i64
  %a = sea.malloc %c16 align 16 [1] : i64 -> !sea.ptr<64>
  %b = sea.malloc %c16 align 16 [2] : i64 -> !sea.ptr<64>
  %n = sea.nondet "n" [3] : i64
  %c15 = sea.const 15 : i64
  %le = sea.icmp ule, %n, %c15 : i64
  sea.assume %le
  %v7 = sea.const 7 : i8
  %m1 = sea.store %m0[%b], %z inplace : !m, i8
  %p = sea.ptr.add %a, %n : !sea.ptr<64>, i64
  %m2 = sea.store %m1[%p], %v7 inplace : !m, i8
  %x = sea.load %m2[%b] : !m -> i8
  %ok = sea.icmp eq, %x, %z : i8
  sea.assert %ok
  sea.halt
}

// SPDX-FileCopyrightText: 2026 The sea-mlir Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
